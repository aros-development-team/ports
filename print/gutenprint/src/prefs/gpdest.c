/*
 * gpdest - where a printer.device unit prints, for GutenprintPrefs.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <exec/types.h>
#include <exec/errors.h>
#include <aros/macros.h>
#include <devices/newstyle.h>
#include <devices/printer.h>
#include <prefs/prefhdr.h>
#include <prefs/printertxt.h>
#include <prefs/printergfx.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/iffparse.h>

#include <stdio.h>
#include <string.h>

#include "gpdest.h"

struct prefs_header
{
    UBYTE ph_Version;
    UBYTE ph_Type;
    UBYTE ph_Flags[4];
};

struct chunks
{
    struct PrinterTxtPrefs txt;
    struct PrinterUnitPrefs unit;
    struct PrinterDeviceUnitPrefs devunit;
    struct PrinterGfxPrefs gfx;
};

static void prefs_path(char *out, size_t size, const char *dir, int unit)
{
    if (unit == 0)
        snprintf(out, size, "%sSYS/printer.prefs", dir);
    else
        snprintf(out, size, "%sSYS/printer%d.prefs", dir, unit);
}

/* Printer prefs' defaults, stored big-endian, with the Gutenprint driver: these
   preferences are made for a unit GutenprintPrefs sets up. */
static void defaults(struct chunks *c, int unit)
{
    memset(c, 0, sizeof(*c));
    strcpy((char *)c->txt.pt_Driver, "Gutenprint");
    c->txt.pt_Port = PP_PARALLEL;
    c->txt.pt_PaperType = AROS_WORD2BE(PT_SINGLE);
    c->txt.pt_PaperSize = AROS_WORD2BE(PS_US_LETTER);
    c->txt.pt_PaperLength = AROS_WORD2BE(65);
    c->txt.pt_Pitch = AROS_WORD2BE(PP_PICA);
    c->txt.pt_Spacing = AROS_WORD2BE(PS_SIX_LPI);
    c->txt.pt_LeftMargin = AROS_WORD2BE(5);
    c->txt.pt_RightMargin = AROS_WORD2BE(75);
    c->txt.pt_Quality = AROS_WORD2BE(PQ_LETTER);
    strcpy((char *)c->unit.pu_DeviceName, "parallel.device");
    snprintf((char *)c->devunit.pd_UnitName, sizeof(c->devunit.pd_UnitName), "Unit %d", unit);
    c->devunit.pd_UnitNum = AROS_LONG2BE(unit);
    c->gfx.pg_Aspect = AROS_WORD2BE(PA_VERTICAL);
    c->gfx.pg_Shade = AROS_WORD2BE(PS_COLOR);
    c->gfx.pg_Image = AROS_WORD2BE(PI_POSITIVE);
    c->gfx.pg_Threshold = AROS_WORD2BE(256);
    c->gfx.pg_Dimensions = PD_IGNORE;
    c->gfx.pg_GraphicFlags = AROS_WORD2BE(PGFB_CENTER_IMAGE);
    c->gfx.pg_PrintDensity = PD_FLOYD;
}

/* TRUE when the file had its text chunk, which Printer prefs needs to count it. */
static int load_chunks(const char *path, struct chunks *c)
{
    static LONG stops[] = { ID_PREF, ID_PTXT, ID_PREF, ID_PUNT, ID_PREF, ID_PDEV, ID_PREF, ID_PGFX };
    struct IFFHandle *iff;
    int txt = 0;
    BPTR fh;

    if ((fh = Open((CONST_STRPTR)path, MODE_OLDFILE)) == BNULL)
        return 0;
    if ((iff = AllocIFF()) != NULL)
    {
        iff->iff_Stream = (IPTR)fh;
        InitIFFasDOS(iff);
        if (OpenIFF(iff, IFFF_READ) == 0)
        {
            if (StopChunks(iff, stops, 4) == 0)
                while (ParseIFF(iff, IFFPARSE_SCAN) == 0)
                {
                    struct ContextNode *cn = CurrentChunk(iff);
                    APTR to = NULL;
                    LONG size = 0;

                    if (cn->cn_ID == ID_PTXT) { to = &c->txt; size = sizeof(c->txt); }
                    else if (cn->cn_ID == ID_PUNT) { to = &c->unit; size = sizeof(c->unit); }
                    else if (cn->cn_ID == ID_PDEV) { to = &c->devunit; size = sizeof(c->devunit); }
                    else if (cn->cn_ID == ID_PGFX) { to = &c->gfx; size = sizeof(c->gfx); }
                    if (to != NULL && cn->cn_Size == size && ReadChunkBytes(iff, to, size) == size && to == &c->txt)
                        txt = 1;
                }
            CloseIFF(iff);
        }
        FreeIFF(iff);
    }
    Close(fh);
    return txt;
}

static int write_chunk(struct IFFHandle *iff, LONG id, APTR data, LONG size)
{
    return PushChunk(iff, ID_PREF, id, size) == 0 && WriteChunkBytes(iff, data, size) == size
        && PopChunk(iff) == 0;
}

static int store_chunks(const char *dir, int unit, struct chunks *c)
{
    struct prefs_header head = { PHV_CURRENT, 0, { 0, 0, 0, 0 } };
    struct IFFHandle *iff;
    char path[64];
    int ok = 0;
    BPTR fh, lock;

    snprintf(path, sizeof(path), "%sSYS", dir);
    if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK)) != BNULL || (lock = CreateDir((CONST_STRPTR)path)) != BNULL)
        UnLock(lock);
    prefs_path(path, sizeof(path), dir, unit);
    if ((fh = Open((CONST_STRPTR)path, MODE_NEWFILE)) == BNULL)
        return 0;
    if ((iff = AllocIFF()) != NULL)
    {
        iff->iff_Stream = (IPTR)fh;
        InitIFFasDOS(iff);
        if (OpenIFF(iff, IFFF_WRITE) == 0)
        {
            if (PushChunk(iff, ID_PREF, ID_FORM, IFFSIZE_UNKNOWN) == 0)
            {
                ok = write_chunk(iff, ID_PRHD, &head, sizeof(head))
                    && write_chunk(iff, ID_PTXT, &c->txt, sizeof(c->txt))
                    && write_chunk(iff, ID_PUNT, &c->unit, sizeof(c->unit))
                    && write_chunk(iff, ID_PDEV, &c->devunit, sizeof(c->devunit))
                    && write_chunk(iff, ID_PGFX, &c->gfx, sizeof(c->gfx));
                PopChunk(iff);
            }
            CloseIFF(iff);
        }
        FreeIFF(iff);
    }
    Close(fh);
    return ok;
}

static int load_unit(int unit, struct chunks *c)
{
    char path[64];

    defaults(c, unit);
    prefs_path(path, sizeof(path), "ENV:", unit);
    if (load_chunks(path, c))
        return 1;
    defaults(c, unit);
    prefs_path(path, sizeof(path), "ENVARC:", unit);
    if (load_chunks(path, c))
        return 1;
    defaults(c, unit);
    return 0;
}

/* ------------------------------------------------------------ device settings */

/* The built-in ports are not opened just to ask: that would take them. */
static int builtin(const char *device)
{
    static const char *const ports[] = { "parallel", "serial", "usbparallel", "printtofile" };
    size_t i;

    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        if (strcmp(device, ports[i]) == 0)
            return 1;
    return device[0] == '\0';
}

static struct IOStdReq *open_settings(const char *device, int unit)
{
    char name[80];
    struct MsgPort *port;
    struct IOStdReq *io;

    snprintf(name, sizeof(name), "%s.device", device);
    if ((port = CreateMsgPort()) == NULL)
        return NULL;
    if ((io = (struct IOStdReq *)CreateIORequest(port, sizeof(*io))) != NULL)
    {
        if (OpenDevice((CONST_STRPTR)name, unit, (struct IORequest *)io, PRDOF_SETTINGS) == 0)
            return io;
        DeleteIORequest((struct IORequest *)io);
    }
    DeleteMsgPort(port);
    return NULL;
}

static void close_settings(struct IOStdReq *io)
{
    struct MsgPort *port = io->io_Message.mn_ReplyPort;

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
}

static int has_command(struct IOStdReq *io, UWORD cmd)
{
    struct NSDeviceQueryResult q;
    UWORD *c;

    memset(&q, 0, sizeof(q));
    io->io_Command = NSCMD_DEVICEQUERY;
    io->io_Data = &q;
    io->io_Length = sizeof(q);
    if (DoIO((struct IORequest *)io) != 0 || io->io_Actual < 16 || q.SupportedCommands == NULL)
        return 0;
    for (c = q.SupportedCommands; *c; c++)
        if (*c == cmd)
            return 1;
    return 0;
}

#define HAS_DEVICE  (1 << 0)
#define HAS_URI     (1 << 1)

/* The unit's settings; which ones it has in the result (0: none, or no such device). */
static int query(const char *device, int unit, char *target, LONG *target_unit,
    char *uri, char *name, char *uuid, char *format)
{
    struct IOStdReq *io;
    const Tag *have = NULL, *t;
    char buf[5][PDA_VALUESIZE];
    LONG tunit = 0;
    int r = 0, i;

    if (builtin(device) || (io = open_settings(device, unit)) == NULL)
        return 0;
    if (has_command(io, PRD_GETDESTINATION))
    {
        struct TagItem tags[] = {
            { PDA_Tags,   (IPTR)&have },
            { PDA_Device, (IPTR)buf[0] },
            { PDA_Unit,   (IPTR)&tunit },
            { PDA_URI,    (IPTR)buf[1] },
            { PDA_Name,   (IPTR)buf[2] },
            { PDA_UUID,   (IPTR)buf[3] },
            { PDA_Format, (IPTR)buf[4] },
            { TAG_DONE,   0 }
        };

        for (i = 0; i < 5; i++)
            buf[i][0] = '\0';
        io->io_Command = PRD_GETDESTINATION;
        io->io_Data = tags;
        io->io_Length = 0;
        if (DoIO((struct IORequest *)io) == 0 && have != NULL)
            for (t = have; *t != TAG_DONE; t++)
            {
                if (*t == PDA_Device)
                    r |= HAS_DEVICE;
                else if (*t == PDA_URI)
                    r |= HAS_URI;
            }
    }
    close_settings(io);
    if (r & HAS_DEVICE)
    {
        size_t n = strlen(buf[0]);

        if (n > 7 && strcmp(buf[0] + n - 7, ".device") == 0)
            buf[0][n - 7] = '\0';
        snprintf(target, 64, "%s", buf[0]);
        *target_unit = tunit;
    }
    if (r & HAS_URI)
    {
        snprintf(uri, 256, "%s", buf[1]);
        snprintf(name, 256, "%s", buf[2]);
        snprintf(uuid, 64, "%s", buf[3]);
        snprintf(format, 256, "%s", buf[4]);
    }
    return r;
}

static LONG apply(const char *device, int unit, struct TagItem *tags)
{
    struct IOStdReq *io;
    LONG err;

    if ((io = open_settings(device, unit)) == NULL)
        return IOERR_OPENFAIL;
    io->io_Command = PRD_SETDESTINATION;
    io->io_Data = tags;
    io->io_Length = 0;
    err = DoIO((struct IORequest *)io);
    close_settings(io);
    return err;
}

/* ---------------------------------------------------------------- the API */

void gpdest_follow(struct gpdest *d)
{
    char target[64];
    LONG tunit = 0;
    int r;

    d->has_sends = d->has_printer = 0;
    d->uri[0] = d->name[0] = d->uuid[0] = d->format[0] = '\0';
    r = query(d->port, d->port_unit, target, &tunit, d->uri, d->name, d->uuid, d->format);
    if (r & HAS_URI)
    {
        d->has_printer = 1;
        snprintf(d->printer_dev, sizeof(d->printer_dev), "%s", d->port);
        d->printer_unit = d->port_unit;
    }
    else if (r & HAS_DEVICE)
    {
        d->has_sends = 1;
        snprintf(d->sends, sizeof(d->sends), "%s", target);
        d->sends_unit = (int)tunit;
        gpdest_follow_sends(d);
    }
}

void gpdest_follow_sends(struct gpdest *d)
{
    char target[64];
    LONG tunit = 0;

    d->has_printer = 0;
    d->uri[0] = d->name[0] = d->uuid[0] = d->format[0] = '\0';
    if (query(d->sends, d->sends_unit, target, &tunit, d->uri, d->name, d->uuid, d->format) & HAS_URI)
    {
        d->has_printer = 1;
        snprintf(d->printer_dev, sizeof(d->printer_dev), "%s", d->sends);
        d->printer_unit = d->sends_unit;
    }
}

void gpdest_load(int unit, struct gpdest *d)
{
    struct chunks c;

    memset(d, 0, sizeof(*d));
    load_unit(unit, &c);
    snprintf(d->port, sizeof(d->port), "%.31s", (const char *)c.unit.pu_DeviceName);
    /* printer.prefs has the full name; older preferences left ".device" out */
    if (strlen(d->port) > 7 && strcmp(d->port + strlen(d->port) - 7, ".device") == 0)
        d->port[strlen(d->port) - 7] = '\0';
    d->port_unit = (int)AROS_BE2LONG(c.unit.pu_UnitNum);
    gpdest_follow(d);
}

void gpdest_driver(int unit, char *driver, size_t size)
{
    struct chunks c;

    if (load_unit(unit, &c))
        snprintf(driver, size, "%.29s", (const char *)c.txt.pt_Driver);
    else
        driver[0] = '\0';
}

void gpdest_network(struct gpdest *d, int unit, const char *uri, const char *name, const char *uuid,
    const char *format)
{
    snprintf(d->port, sizeof(d->port), "printspool");
    d->port_unit = unit;
    d->has_sends = 1;
    snprintf(d->sends, sizeof(d->sends), "ipp");
    d->sends_unit = unit;
    d->has_printer = 1;
    snprintf(d->printer_dev, sizeof(d->printer_dev), "ipp");
    d->printer_unit = unit;
    snprintf(d->uri, sizeof(d->uri), "%s", uri);
    snprintf(d->name, sizeof(d->name), "%s", name != NULL ? name : "");
    snprintf(d->uuid, sizeof(d->uuid), "%s", uuid != NULL ? uuid : "");
    snprintf(d->format, sizeof(d->format), "%s", format != NULL ? format : "");
}

static void fail(char *why, size_t why_size, const char *what, LONG err)
{
    char reason[100];

    if (IoErr() != 0 && Fault(IoErr(), NULL, (STRPTR)reason, sizeof(reason)))
        snprintf(why, why_size, "%s: %s", what, reason);
    else
        snprintf(why, why_size, "%s (error %ld)", what, (long)err);
}

int gpdest_store(int unit, const struct gpdest *now, const struct gpdest *was, int save,
    char *why, size_t why_size)
{
    char devname[80];
    LONG err;

    /* the unit's output device, the rest of its preferences as they are */
    if (save || strcmp(now->port, was->port) != 0 || now->port_unit != was->port_unit)
    {
        struct chunks c;

        load_unit(unit, &c);
        if (strlen(now->port) + 8 > sizeof(c.unit.pu_DeviceName))
        {
            fail(why, why_size, "The output device's name is too long", 0);
            return 1;
        }
        memset(c.unit.pu_DeviceName, 0, sizeof(c.unit.pu_DeviceName));
        snprintf((char *)c.unit.pu_DeviceName, sizeof(c.unit.pu_DeviceName), "%s.device", now->port);
        c.unit.pu_UnitNum = AROS_LONG2BE(now->port_unit);
        if (!store_chunks("ENV:", unit, &c) || (save && !store_chunks("ENVARC:", unit, &c)))
        {
            fail(why, why_size, "Cannot write the printer preferences", 0);
            return 1;
        }
    }
    if (now->has_sends && (save || strcmp(now->sends, was->sends) != 0 || now->sends_unit != was->sends_unit
        || strcmp(now->port, was->port) != 0 || now->port_unit != was->port_unit))
    {
        struct TagItem tags[] = {
            { PDA_Device, (IPTR)devname },
            { PDA_Unit,   now->sends_unit },
            { PDA_Save,   save },
            { TAG_DONE,   0 }
        };

        snprintf(devname, sizeof(devname), "%s.device", now->sends);
        if ((err = apply(now->port, now->port_unit, tags)) != 0)
        {
            fail(why, why_size, "The print queue refused where to send its jobs", err);
            return 1;
        }
    }
    if (now->has_printer)
    {
        int moved = strcmp(now->printer_dev, was->printer_dev) != 0 || now->printer_unit != was->printer_unit;
        struct TagItem tags[6];
        int n = 0;

        /* a new address goes with its own name and ID */
        if (save || moved || strcmp(now->uri, was->uri) != 0)
        {
            tags[n].ti_Tag = PDA_URI;  tags[n++].ti_Data = (IPTR)now->uri;
            tags[n].ti_Tag = PDA_Name; tags[n++].ti_Data = (IPTR)now->name;
            tags[n].ti_Tag = PDA_UUID; tags[n++].ti_Data = (IPTR)now->uuid;
        }
        else if (strcmp(now->name, was->name) != 0)
        {
            tags[n].ti_Tag = PDA_Name; tags[n++].ti_Data = (IPTR)now->name;
        }
        if (save || moved || strcmp(now->format, was->format) != 0)
        {
            tags[n].ti_Tag = PDA_Format; tags[n++].ti_Data = (IPTR)now->format;
        }
        if (n > 0)
        {
            tags[n].ti_Tag = PDA_Save; tags[n++].ti_Data = save;
            tags[n].ti_Tag = TAG_DONE;
            if ((err = apply(now->printer_dev, now->printer_unit, tags)) != 0)
            {
                fail(why, why_size, "The network printer could not be set", err);
                return 1;
            }
        }
    }
    return 0;
}
