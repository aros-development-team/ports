/*
 * gpfind - network printers for GutenprintPrefs.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpfind.h"

#define FIND_LIST "T:GutenprintFind"

static void copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

/* Runs C:FindPrinters with args and reads the list it writes. */
static int run_find(const char *args, struct gp_found *found, int max, char *why, size_t why_size)
{
    char line[700], cmd[400];
    FILE *f;
    BPTR nil;
    LONG rc;
    int n = -1;

    DeleteFile((STRPTR)FIND_LIST);
    snprintf(cmd, sizeof(cmd), "C:FindPrinters TO " FIND_LIST " %s", args);
    nil = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((CONST_STRPTR)cmd,
        SYS_Input, (IPTR)nil, SYS_Output, (IPTR)NULL, SYS_Asynch, FALSE, TAG_END);
    if (nil)
        Close(nil);
    if (rc != 0 && rc != 5)
    {
        snprintf(why, why_size, rc == -1 ? "C:FindPrinters is missing." :
            "C:FindPrinters failed (is the network running?).");
        return -1;
    }
    f = fopen(FIND_LIST, "r");
    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *value = strchr(line, '=');
        struct gp_found *p;

        line[strcspn(line, "\r\n")] = '\0';
        if (value == NULL)
            continue;
        *value++ = '\0';
        if (strcmp(line, "Printer") == 0)
        {
            if (n + 1 >= max)
                break;
            p = &found[++n];
            memset(p, 0, sizeof(*p));
            copy(p->name, sizeof(p->name), value);
            continue;
        }
        if (n < 0)
            continue;
        p = &found[n];
        if (strcmp(line, "URI") == 0)
            copy(p->uri, sizeof(p->uri), value);
        else if (strcmp(line, "Formats") == 0)
            copy(p->formats, sizeof(p->formats), value);
        else if (strcmp(line, "Model") == 0)
            copy(p->model, sizeof(p->model), value);
        else if (strcmp(line, "State") == 0)
            copy(p->state, sizeof(p->state), value);
        else if (strcmp(line, "Reasons") == 0)
            copy(p->reasons, sizeof(p->reasons), value);
        else if (strcmp(line, "Message") == 0)
            copy(p->message, sizeof(p->message), value);
        else if (strcmp(line, "Paper") == 0)
            copy(p->paper, sizeof(p->paper), value);
        else if (strcmp(line, "Picture") == 0)
            copy(p->picture, sizeof(p->picture), value);
        else if (strcmp(line, "Supply") == 0 && p->nsupplies < GPFIND_SUPPLIES)
        {
            /* level;#RRGGBB;name */
            char *a = strchr(value, ';'), *b = a ? strchr(a + 1, ';') : NULL;

            if (b == NULL)
                continue;
            *a++ = '\0';
            *b++ = '\0';
            p->level[p->nsupplies] = atoi(value);
            copy(p->colour[p->nsupplies], sizeof(p->colour[0]), a);
            copy(p->supply[p->nsupplies], sizeof(p->supply[0]), b);
            p->nsupplies++;
        }
    }
    fclose(f);
    return n + 1;
}

int gpfind_scan(struct gp_found *found, int max, char *why, size_t why_size)
{
    return run_find("", found, max, why, why_size);
}

int gpfind_query(const char *uri, struct gp_found *found, char *why, size_t why_size)
{
    char args[300];
    int n;

    snprintf(args, sizeof(args), "URI \"%s\"", uri);
    n = run_find(args, found, 1, why, why_size);
    return n < 0 ? -1 : n == 1 && found->state[0] != '\0';
}

void gpfind_reasons_text(const char *reasons, char *out, size_t size)
{
    static const struct { const char *key, *text; } words[] = {
        { "toner-low", "toner low" }, { "toner-empty", "toner empty" },
        { "marker-supply-low", "supply low" }, { "marker-supply-empty", "supply empty" },
        { "marker-waste-almost-full", "waste toner almost full" }, { "marker-waste-full", "waste toner full" },
        { "media-low", "paper low" }, { "media-empty", "out of paper" }, { "media-needed", "load paper" },
        { "media-jam", "paper jam" }, { "cover-open", "cover open" }, { "door-open", "door open" },
        { "input-tray-missing", "paper tray missing" }, { "output-area-full", "output tray full" },
        { "offline", "offline" }, { "paused", "paused" }, { "shutdown", "shut down" },
        { "connecting-to-device", "connecting" }, { "spool-area-full", "printer memory full" },
        { NULL, NULL }
    };
    const char *r = reasons;

    out[0] = '\0';
    while (r != NULL && *r)
    {
        char key[64];
        size_t n = strcspn(r, ","), k;
        const char *text = NULL;
        int i;

        snprintf(key, sizeof(key), "%.*s", (int)(n < sizeof(key) ? n : sizeof(key) - 1), r);
        /* -report, -warning and -error say how serious; the words say what. */
        for (k = strlen(key); k > 0 && key[k - 1] != '-'; k--)
            ;
        if (k > 0 && (strcmp(key + k, "report") == 0 || strcmp(key + k, "warning") == 0 || strcmp(key + k, "error") == 0))
            key[k - 1] = '\0';
        for (i = 0; words[i].key != NULL; i++)
            if (strcmp(key, words[i].key) == 0)
                text = words[i].text;
        if (strcmp(key, "none") != 0)
        {
            char word[64];

            if (text == NULL)
            {
                for (k = 0; key[k] && k < sizeof(word) - 1; k++)
                    word[k] = key[k] == '-' ? ' ' : key[k];
                word[k] = '\0';
                text = word;
            }
            snprintf(out + strlen(out), size - strlen(out), "%s%s", out[0] ? ", " : "", text);
        }
        r += n;
        if (*r == ',')
            r++;
    }
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 'a' - 'A';
}

const char *gpfind_page_size(const char *pwg)
{
    static const struct { const char *pwg, *size; } map[] = {
        { "na_letter_", "Letter" }, { "na_legal_", "Legal" }, { "na_executive_", "Executive" },
        { "iso_a3_", "A3" }, { "iso_a4_", "A4" }, { "iso_a5_", "A5" }, { "iso_a6_", "A6" },
        { "iso_b5_", "B5" }, { "jis_b5_", "B5" }, { NULL, NULL }
    };
    int i;

    for (i = 0; pwg != NULL && map[i].pwg != NULL; i++)
        if (strncmp(pwg, map[i].pwg, strlen(map[i].pwg)) == 0)
            return map[i].size;
    return NULL;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return 1;
    fputs(text, f);
    return fclose(f) != 0;
}

static unsigned char *put_be(unsigned char *p, unsigned long v, int bytes)
{
    while (bytes--)
        *p++ = (unsigned char)(v >> (8 * bytes));
    return p;
}

static unsigned char *chunk(unsigned char *p, const char *id, const unsigned char *data, unsigned long len)
{
    memcpy(p, id, 4);
    p = put_be(p + 4, len, 4);
    memcpy(p, data, len);
    p += len;
    if (len & 1)
        *p++ = 0;
    return p;
}

/* A printer prefs file for a unit that has none: the Gutenprint driver, A4/Letter text
   defaults, colour graphics centred on the page, output to printspool.device <unit>. */
static int new_prefs(const char *path, int unit, char *why, size_t why_size)
{
    unsigned char out[512], hdr[6] = { 0 }, txt[64] = { 0 }, unt[56] = { 0 }, dev[52] = { 0 }, gfx[40] = { 0 };
    unsigned char *p = out + 12;
    FILE *f;

    /* The chunks are printer.device's native structures (prefs/printertxt.h,
       printergfx.h) with big-endian fields. PrinterTxtPrefs: driver at 16, then port,
       paper type 48, size 50, length 52, pitch 54, spacing 56, margins 58/60, quality 62. */
    strcpy((char *)txt + 16, "GUTENPRINT:Gutenprint");
    put_be(txt + 48, 1, 2);     /* PT_SINGLE */
    put_be(txt + 50, 0, 2);     /* PS_US_LETTER */
    put_be(txt + 52, 66, 2);
    put_be(txt + 58, 5, 2);
    put_be(txt + 60, 75, 2);
    put_be(txt + 62, 1, 2);     /* PQ_LETTER */
    /* PrinterUnitPrefs: unit at 16, device name at 24. */
    put_be(unt + 16, unit, 4);
    strcpy((char *)unt + 24, "printspool");
    /* PrinterDeviceUnitPrefs: unit at 16. */
    put_be(dev + 16, unit, 4);
    /* PrinterGfxPrefs: shade 18, threshold 22, flags 28, density 30. */
    put_be(gfx + 18, 2, 2);     /* SHADE_COLOR */
    put_be(gfx + 22, 7, 2);
    put_be(gfx + 28, 1, 2);     /* PGFF_CENTER_IMAGE */
    gfx[30] = 5;
    p = chunk(p, "PRHD", hdr, sizeof(hdr));
    p = chunk(p, "PTXT", txt, sizeof(txt));
    p = chunk(p, "PUNT", unt, sizeof(unt));
    p = chunk(p, "PDEV", dev, sizeof(dev));
    p = chunk(p, "PGFX", gfx, sizeof(gfx));
    memcpy(out, "FORM", 4);
    put_be(out + 4, p - out - 8, 4);
    memcpy(out + 8, "PREF", 4);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(out, 1, p - out, f) != (size_t)(p - out))
    {
        if (f)
            fclose(f);
        snprintf(why, why_size, "Cannot write %s.", path);
        return 1;
    }
    return fclose(f) != 0;
}

/* printer.device's prefs for unit (ENV:Sys/printer.prefs, printer<n>.prefs): the output
   device in the PUNT chunk becomes printspool.device <unit>, the driver in PTXT Gutenprint. */
static int point_prefs_at_spooler(const char *dir, int unit, char *why, size_t why_size)
{
    char path[64];
    unsigned char *buf;
    long size, i;
    FILE *f;
    int punt = 0;

    if (unit == 0)
        snprintf(path, sizeof(path), "%sSys/printer.prefs", dir);
    else
        snprintf(path, sizeof(path), "%sSys/printer%d.prefs", dir, unit);
    f = fopen(path, "rb");
    if (f == NULL)
        return new_prefs(path, unit, why, why_size);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (buf == NULL || fread(buf, 1, size, f) != (size_t)size)
    {
        fclose(f);
        free(buf);
        snprintf(why, why_size, "Cannot read %s.", path);
        return 1;
    }
    fclose(f);
    /* Chunks after FORM....PREF: id, big-endian length, data, padded to even. */
    for (i = 12; i + 8 <= size;)
    {
        long len = ((long)buf[i + 4] << 24) | (buf[i + 5] << 16) | (buf[i + 6] << 8) | buf[i + 7];
        unsigned char *d = buf + i + 8;

        if (i + 8 + len > size)
            break;
        if (memcmp(buf + i, "PUNT", 4) == 0 && len >= 56)
        {
            /* pu_Reserved[4], pu_UnitNum, pu_OpenDeviceFlags, pu_DeviceName[32] */
            d[16] = d[17] = d[18] = 0;
            d[19] = (unsigned char)unit;
            memset(d + 24, 0, 32);
            strcpy((char *)d + 24, "printspool");
            punt = 1;
        }
        else if (memcmp(buf + i, "PTXT", 4) == 0 && len >= 46)
        {
            /* pt_Reserved[4], pt_Driver[30] */
            memset(d + 16, 0, 30);
            strcpy((char *)d + 16, "GUTENPRINT:Gutenprint");
        }
        i += 8 + len + (len & 1);
    }
    if (!punt)
    {
        free(buf);
        snprintf(why, why_size, "%s has no unit settings: save Printer prefs once first.", path);
        return 1;
    }
    f = fopen(path, "wb");
    if (f == NULL || fwrite(buf, 1, size, f) != (size_t)size)
    {
        if (f)
            fclose(f);
        free(buf);
        snprintf(why, why_size, "Cannot write %s.", path);
        return 1;
    }
    free(buf);
    return fclose(f) != 0;
}

static int setup_in(const char *dir, int unit, const char *uri, const char *name, const char *format,
    char *why, size_t why_size)
{
    char path[64], text[600];
    BPTR lock;

    snprintf(path, sizeof(path), "%sIPP", dir);
    if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK)) || (lock = CreateDir((CONST_STRPTR)path)))
        UnLock(lock);
    snprintf(path, sizeof(path), "%sIPP/Unit%d", dir, unit);
    snprintf(text, sizeof(text), "URI=%s\nNAME=%s\nFORMAT=%s\n", uri, name, format);
    if (write_text(path, text) != 0)
    {
        snprintf(why, why_size, "Cannot write %s.", path);
        return 1;
    }
    snprintf(path, sizeof(path), "%sPrintSpool", dir);
    if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK)) || (lock = CreateDir((CONST_STRPTR)path)))
        UnLock(lock);
    snprintf(path, sizeof(path), "%sPrintSpool/Unit%d", dir, unit);
    snprintf(text, sizeof(text), "ipp.device %d", unit);
    if (write_text(path, text) != 0)
    {
        snprintf(why, why_size, "Cannot write %s.", path);
        return 1;
    }
    return point_prefs_at_spooler(dir, unit, why, why_size);
}

int gpfind_setup_unit(int unit, const char *uri, const char *name, const char *format, int save,
    char *why, size_t why_size)
{
    if (setup_in("ENV:", unit, uri, name, format, why, why_size) != 0)
        return 1;
    return save ? setup_in("ENVARC:", unit, uri, name, format, why, why_size) : 0;
}
