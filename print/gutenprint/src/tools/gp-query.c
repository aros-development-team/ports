/*
 * gp-query - what an application sees of a printer.device unit.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * gp-query [unit] [READ] [USE] [EDIT] ...
 *
 * Opens printer.device unit N (default 0) and prints the driver's extended
 * data and the preferences, the way an application reads a printer's page
 * before it prints. Each further word sends one of the V44 preferences
 * commands to the unit and prints what the application sees afterwards:
 * READ (PRD_READPREFS, the preferences structures), USE (PRD_USEPREFS),
 * EDIT (PRD_EDITPREFS, the driver's own editor; returns when it closes).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <intuition/preferences.h>
#include <prefs/printertxt.h>
#include <prefs/printergfx.h>
#include <utility/tagitem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* printer.device's request union, as applications declare it. */
union printerIO
{
    struct IOStdReq ios;
    struct IODRPReq iodrp;
    struct IOPrtCmdReq iopc;
    struct IOPrtPrefsReq iopp;
    struct IOPrefsReq iopr;
};

static const char *paper_name(UWORD size)
{
    switch (size)
    {
    case US_LETTER: return "US Letter";
    case US_LEGAL:  return "US Legal";
    case N_TRACTOR: return "narrow tractor";
    case W_TRACTOR: return "wide tractor";
    case CUSTOM:    return "custom";
    case EURO_A0:   return "A0";
    case EURO_A1:   return "A1";
    case EURO_A2:   return "A2";
    case EURO_A3:   return "A3";
    case EURO_A4:   return "A4";
    case EURO_A5:   return "A5";
    case EURO_A6:   return "A6";
    case EURO_A7:   return "A7";
    case EURO_A8:   return "A8";
    default:        return "?";
    }
}

static void show(union printerIO *io, LONG unit)
{
    struct PrinterData *pd = (struct PrinterData *)io->ios.io_Device;
    struct PrinterExtendedData *ped = &pd->pd_SegmentData->ps_PED;
    struct Preferences *prefs = &pd->pd_Preferences;

    printf("printer.device unit %ld: driver %s (%s), port %s\n", (long)unit,
        ped->ped_PrinterName, prefs->PrinterFilename, prefs->PrtDevName);
    printf("extended data: %lu x %lu dots at %u x %u dpi, %u columns, %u rows\n",
        (unsigned long)ped->ped_MaxXDots, (unsigned long)ped->ped_MaxYDots,
        ped->ped_XDotsInch, ped->ped_YDotsInch, ped->ped_MaxColumns, ped->ped_NumRows);
    printf("preferences: paper 0x%02x %s, %s, %u lines, printable %u.%u x %u.%u inch\n",
        prefs->PaperSize, paper_name(prefs->PaperSize),
        prefs->PaperType == SINGLE ? "single sheet" : "fanfold",
        prefs->PaperLength,
        prefs->PrintMaxWidth / 10, prefs->PrintMaxWidth % 10,
        prefs->PrintMaxHeight / 10, prefs->PrintMaxHeight % 10);
}

/* PRD_READPREFS into the four V44 structures. */
static LONG read_prefs(union printerIO *io)
{
    struct PrinterTxtPrefs txt;
    struct PrinterUnitPrefs unit;
    struct PrinterDeviceUnitPrefs devunit;
    struct PrinterGfxPrefs gfx;

    memset(&txt, 0xee, sizeof(txt));
    memset(&unit, 0xee, sizeof(unit));
    memset(&devunit, 0xee, sizeof(devunit));
    memset(&gfx, 0xee, sizeof(gfx));
    io->iopr.io_Command = PRD_READPREFS;
    io->iopr.io_TxtPrefs = &txt;
    io->iopr.io_UnitPrefs = &unit;
    io->iopr.io_DevUnitPrefs = &devunit;
    io->iopr.io_GfxPrefs = &gfx;
    if (DoIO((struct IORequest *)io) != 0)
        return io->ios.io_Error;
    printf("PRD_READPREFS: txt driver %.*s port %u, paper type %u size %u (%s) length %u, "
        "pitch %u spacing %u margins %u-%u quality %u\n",
        DRIVERNAMESIZE, txt.pt_Driver, txt.pt_Port, txt.pt_PaperType, txt.pt_PaperSize,
        paper_name(txt.pt_PaperSize << 4), txt.pt_PaperLength, txt.pt_Pitch, txt.pt_Spacing,
        txt.pt_LeftMargin, txt.pt_RightMargin, txt.pt_Quality);
    printf("PRD_READPREFS: unit device %.*s unit %ld; printer.device unit %ld \"%.*s\"\n",
        DEVICENAMESIZE, unit.pu_DeviceName, (long)unit.pu_UnitNum,
        (long)devunit.pd_UnitNum, UNITNAMESIZE, devunit.pd_UnitName);
    printf("PRD_READPREFS: gfx aspect %u shade %u image %u threshold %d flags 0x%x density %u, "
        "max %u.%u x %u.%u inch, offset %u\n",
        gfx.pg_Aspect, gfx.pg_Shade, gfx.pg_Image, gfx.pg_Threshold, gfx.pg_GraphicFlags,
        gfx.pg_PrintDensity, gfx.pg_PrintMaxWidth / 10, gfx.pg_PrintMaxWidth % 10,
        gfx.pg_PrintMaxHeight / 10, gfx.pg_PrintMaxHeight % 10, gfx.pg_PrintXOffset);
    return 0;
}

static LONG edit_prefs(union printerIO *io)
{
    struct TagItem tags[] = { { PPRA_Window, 0 }, { TAG_END, 0 } };

    io->iopp.io_Command = PRD_EDITPREFS;
    io->iopp.io_TagList = tags;
    printf("PRD_EDITPREFS: sent, waiting for the editor\n");
    fflush(stdout);
    return DoIO((struct IORequest *)io) != 0 ? io->ios.io_Error : 0;
}

static LONG use_prefs(union printerIO *io)
{
    io->ios.io_Command = PRD_USEPREFS;
    return DoIO((struct IORequest *)io) != 0 ? io->ios.io_Error : 0;
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    union printerIO *io;
    LONG unit = argc > 1 ? atoi(argv[1]) : 0;
    int rc = 20, i;

    port = CreateMsgPort();
    io = port != NULL ? (union printerIO *)CreateIORequest(port, sizeof(*io)) : NULL;
    if (io == NULL)
    {
        printf("gp-query: no port\n");
        return 20;
    }
    if (OpenDevice((CONST_STRPTR)"printer.device", unit, (struct IORequest *)io, 0) == 0)
    {
        rc = 0;
        show(io, unit);
        for (i = 2; i < argc && rc == 0; i++)
        {
            LONG err;

            if (strcasecmp(argv[i], "READ") == 0)
                err = read_prefs(io);
            else if (strcasecmp(argv[i], "USE") == 0)
                err = use_prefs(io);
            else if (strcasecmp(argv[i], "EDIT") == 0)
                err = edit_prefs(io);
            else
            {
                printf("gp-query: unknown command %s\n", argv[i]);
                rc = 10;
                break;
            }
            if (err != 0)
            {
                printf("gp-query: %s failed (error %ld)\n", argv[i], (long)err);
                rc = 10;
                break;
            }
            printf("after %s:\n", argv[i]);
            show(io, unit);
        }
        CloseDevice((struct IORequest *)io);
    }
    else
        printf("gp-query: cannot open printer.device unit %ld (error %d)\n", (long)unit,
            io->ios.io_Error);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return rc;
}
