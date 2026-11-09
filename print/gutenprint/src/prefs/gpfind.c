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
        else if (strcmp(line, "UUID") == 0)
            copy(p->uuid, sizeof(p->uuid), value);
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
