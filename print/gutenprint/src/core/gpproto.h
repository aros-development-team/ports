/*
 * gpproto - messages between the Gutenprint printer driver and GutenprintRender.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#ifndef GPPROTO_H
#define GPPROTO_H

#include <exec/ports.h>
#include <exec/types.h>

#define GP_PORTNAME   "GUTENPRINT.RENDER"
#define GP_RENDERER   "GUTENPRINT:GutenprintRender"
#define GP_PREFS      "GUTENPRINT:GutenprintPrefs"
#define GP_LAYOUT_DPI 300      /* printer.device's view of the page */

enum
{
    GPCMD_GEOMETRY = 1, /* -> page size and printable area for the current settings */
    GPCMD_PRINT,        /* print one page image of job gm_JobID, starting it if needed */
    GPCMD_DATA,         /* (renderer -> driver) printer data; reply when written */
    GPCMD_QUIT,
    GPCMD_ENDJOB        /* end job gm_JobID: the printer's end-of-job data */
};

#define GP_MAX_UNITS  10   /* printer.device units */

struct GPMsg
{
    struct Message gm_Msg;
    ULONG gm_Command;
    LONG gm_Result;               /* 0 = ok */
    LONG gm_Unit;                 /* printer.device unit: selects the printer profile */
    /* GPCMD_GEOMETRY reply, in dots at GP_LAYOUT_DPI */
    LONG gm_PageWidth, gm_PageHeight;
    LONG gm_AreaLeft, gm_AreaTop, gm_AreaWidth, gm_AreaHeight;
    /* GPCMD_PRINT: 8-bit RGB image and where it goes in the printable area (dots) */
    LONG gm_ImageWidth, gm_ImageHeight;
    UBYTE *gm_Pixels;
    LONG gm_PlaceLeft, gm_PlaceTop, gm_PlaceWidth, gm_PlaceHeight;
    struct MsgPort *gm_DataPort;  /* where GPCMD_DATA messages go */
    /* GPCMD_DATA */
    const UBYTE *gm_Data;
    LONG gm_Length;
    char gm_Error[160];
    ULONG gm_JobID;               /* GPCMD_PRINT, GPCMD_ENDJOB: one per printer.device open */
};

#endif
