/*
 * Gutenprint - a printer.device driver that prints through Gutenprint.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * printer.device hands this driver unscaled 24-bit page dumps. The driver
 * collects each dump and passes it to GutenprintRender (a separate program
 * that runs Gutenprint), then writes the printer data that comes back through
 * printer.device's own port. The pages between one open and close of the
 * printer are one Gutenprint job. Text (CMD_WRITE) is laid out on pages in a
 * monospaced outline font and printed as page images in the same job;
 * printer text commands (styles, margins) are ignored.
 */
#define DEBUG 0
#include <aros/debug.h>
#include <aros/printertag.h>

#include <devices/printer.h>
#include <devices/prtgfx.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <prefs/printergfx.h>
#include <prefs/printertxt.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

#include <proto/diskfont.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

#include <string.h>

#include "../core/gpproto.h"

static LONG gp_Init(struct PrinterData *pd);
static VOID gp_Expunge(VOID);
static LONG gp_Open(union printerIO *ior);
static VOID gp_Close(union printerIO *ior);
static LONG gp_Render(SIPTR ct, LONG x, LONG y, LONG status);
static LONG gp_ConvFunc(UBYTE *buf, UBYTE c, LONG crlf_flag);
static LONG gp_DoSpecial(UWORD *command, UBYTE output_buffer[],
    BYTE *current_line_position, BYTE *current_line_spacing,
    BYTE *crlf_flag, UBYTE params[]);
static LONG gp_DoPreferences(union printerIO *ior, LONG command);

/* No text commands: every one of the 77 is unsupported. */
static CONST_STRPTR PED_Commands[] = {
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377",
    "\377"
};

static struct TagItem PED_TagList[] = {
    { PRTA_8BitGuns, TRUE },
    { PRTA_ConvertSource, TRUE },
    { PRTA_ColorCorrection, TRUE },
    { PRTA_FloydDithering, TRUE },
    { PRTA_NoScaling, TRUE },
    { TAG_END }
};

AROS_PRINTER_TAG(PED, 44, 0,
        .ped_PrinterName = "Gutenprint",
        .ped_Init = gp_Init,
        .ped_Expunge = gp_Expunge,
        .ped_Open = gp_Open,
        .ped_Close = gp_Close,
        .ped_PrinterClass = PPC_COLORGFX | PPCF_EXTENDED,
        .ped_MaxColumns = 0,
        .ped_ColorClass = PCC_BGR,
        .ped_NumCharSets = 1,
        .ped_NumRows = 1,
        .ped_MaxXDots = 0,
        .ped_MaxYDots = 0,
        .ped_XDotsInch = GP_LAYOUT_DPI,
        .ped_YDotsInch = GP_LAYOUT_DPI,
        .ped_Commands = (STRPTR *)PED_Commands,
        .ped_DoSpecial = gp_DoSpecial,
        .ped_Render = gp_Render,
        .ped_TimeoutSecs = 1000,
        .ped_8BitChars = NULL,
        .ped_PrintMode = 1,
        .ped_ConvFunc = gp_ConvFunc,
        .ped_TagList = &PED_TagList[0],
        .ped_DoPreferences = gp_DoPreferences,
    );

struct PrinterData *PD;
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct UtilityBase *UtilityBase;
struct GfxBase *GfxBase;
struct Library *DiskfontBase;

#define GP_TEXT_FONT    "DejaVuSansMono.font"
#define GP_TEXT_POINTS  10
#define GP_TEXT_TAB     8

/* The text page being filled: rows of characters, one byte per column. */
static UBYTE *gp_Text;
static LONG gp_TextCols, gp_TextRows, gp_TextRow, gp_TextCol;
static BOOL gp_TextUsed;
static struct TextFont *gp_TextFont;
/* The profile's page and printable area in GP_LAYOUT_DPI dots. */
static LONG gp_PageWidth, gp_PageHeight;
static LONG gp_AreaLeft, gp_AreaTop, gp_AreaWidth, gp_AreaHeight;

static UBYTE *gp_Image;
static LONG gp_ImageWidth, gp_ImageHeight, gp_Row;
static LONG gp_PlaceLeft, gp_PlaceTop, gp_PlaceWidth, gp_PlaceHeight;
static ULONG gp_JobID;
static BOOL gp_JobOpen;

static LONG gp_Init(struct PrinterData *pd)
{
    PD = pd;
    SysBase = pd->pd_Device.dd_ExecBase;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    UtilityBase = (struct UtilityBase *)OpenLibrary("utility.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    return DOSBase != NULL && UtilityBase != NULL && GfxBase != NULL ? 0 : -1;
}

static VOID gp_Expunge(VOID)
{
    CloseLibrary((struct Library *)GfxBase);
    GfxBase = NULL;
    CloseLibrary((struct Library *)UtilityBase);
    UtilityBase = NULL;
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    PD = NULL;
}

static LONG gp_Call(struct GPMsg *m);
static LONG gp_Geometry(void);
static LONG gp_TextFlush(void);
static void gp_TextFree(void);

static LONG gp_Open(union printerIO *ior)
{
    (void)ior;
    /* Unique across driver reloads: printer.device expunges the driver on
       every close. */
    gp_JobID = GetUniqueID();
    gp_JobOpen = FALSE;
    gp_AreaWidth = gp_AreaHeight = 0;
    /* The printer is known from the first moment the application can look:
       its page goes into the extended data and the preferences now. */
    if (gp_Geometry() != PDERR_NOERR)
        bug("[Gutenprint] unit %ld: no page geometry at open\n", (long)PD->pd_UnitNumber);
    return 0;
}

static VOID gp_Close(union printerIO *ior)
{
    (void)ior;
    gp_TextFlush();
    gp_TextFree();
    if (gp_JobOpen)
    {
        struct GPMsg m;

        memset(&m, 0, sizeof(m));
        m.gm_Command = GPCMD_ENDJOB;
        m.gm_JobID = gp_JobID;
        gp_Call(&m);
        gp_JobOpen = FALSE;
    }
}

/* The renderer's port, starting GutenprintRender if it is not running. */
static struct MsgPort *gp_FindRenderer(void)
{
    struct MsgPort *port;
    int tries;

    Forbid();
    port = FindPort(GP_PORTNAME);
    Permit();
    if (port != NULL)
        return port;
    if (SystemTags(GP_RENDERER,
            SYS_Asynch, TRUE,
            SYS_Input, Open("NIL:", MODE_OLDFILE),
            SYS_Output, Open("NIL:", MODE_NEWFILE),
            NP_StackSize, 2000000,
            NP_Name, (IPTR)"GutenprintRender",
            TAG_END) == -1)
        return NULL;
    for (tries = 0; tries < 100; tries++)
    {
        Delay(5);
        Forbid();
        port = FindPort(GP_PORTNAME);
        Permit();
        if (port != NULL)
            return port;
    }
    return NULL;
}

/* Sends m to the renderer; GPCMD_DATA messages are written while it works. */
static LONG gp_Call(struct GPMsg *m)
{
    struct MsgPort *reply = CreateMsgPort();
    struct MsgPort *renderer;
    BOOL done = FALSE;

    if (reply == NULL)
        return PDERR_INTERNALMEMORY;
    m->gm_Msg.mn_ReplyPort = reply;
    m->gm_Msg.mn_Length = sizeof(*m);
    m->gm_DataPort = reply;
    m->gm_Result = 1;
    m->gm_Unit = PD->pd_UnitNumber;
    /* Started outside Forbid(); the port is looked up again under it, since a
       renderer that quits could remove it in between. */
    renderer = gp_FindRenderer();
    if (renderer != NULL)
    {
        Forbid();
        renderer = FindPort(GP_PORTNAME);
        if (renderer != NULL)
            PutMsg(renderer, &m->gm_Msg);
        Permit();
    }
    if (renderer == NULL)
    {
        DeleteMsgPort(reply);
        bug("[Gutenprint] cannot start " GP_RENDERER "\n");
        return PDERR_CANCEL;
    }
    while (!done)
    {
        struct GPMsg *in;

        WaitPort(reply);
        while ((in = (struct GPMsg *)GetMsg(reply)) != NULL)
        {
            if (in == m)
            {
                done = TRUE;
                continue;
            }
            /* A chunk of printer data: written before it is handed back. */
            in->gm_Result = PD->pd_PWrite((APTR)in->gm_Data, in->gm_Length);
            if (in->gm_Result == 0)
                in->gm_Result = PD->pd_PBothReady();
            ReplyMsg(&in->gm_Msg);
        }
    }
    DeleteMsgPort(reply);
    if (m->gm_Result != 0)
    {
        bug("[Gutenprint] %s\n", m->gm_Error);
        return PDERR_CANCEL;
    }
    return PDERR_NOERR;
}

static LONG gp_RenderPreInit(struct IODRPReq *io, LONG flags)
{
    (void)io;
    (void)flags;
    /* Asked again for every dump: the profile may have changed since open. */
    return gp_Geometry();
}

static LONG gp_RenderInit(struct IODRPReq *io, LONG width, LONG height)
{
    LONG err = gp_TextFlush();

    if (err != PDERR_NOERR)
        return err;
    gp_ImageWidth = io->io_SrcWidth;
    gp_ImageHeight = io->io_SrcHeight;
    gp_PlaceWidth = width;
    gp_PlaceHeight = height;
    gp_PlaceLeft = gp_PlaceTop = -1;
    gp_Row = 0;
    gp_Image = AllocVec(gp_ImageWidth * gp_ImageHeight * 3, MEMF_PUBLIC);
    return gp_Image != NULL ? PDERR_NOERR : PDERR_BUFFERMEMORY;
}

static LONG gp_RenderTransfer(struct PrtInfo *pi, LONG y)
{
    union colorEntry *src = pi->pi_ColorInt;
    UBYTE *dst;
    LONG x;

    if (gp_Image == NULL || gp_Row >= gp_ImageHeight)
        return PDERR_NOERR;
    if (gp_PlaceLeft < 0)
    {
        gp_PlaceLeft = pi->pi_xpos;
        gp_PlaceTop = y;
    }
    dst = gp_Image + gp_Row * gp_ImageWidth * 3;
    for (x = 0; x < gp_ImageWidth; x++, src++)
    {
        *dst++ = src->colorByte[PCMRED];
        *dst++ = src->colorByte[PCMGREEN];
        *dst++ = src->colorByte[PCMBLUE];
    }
    gp_Row++;
    return PDERR_NOERR;
}

static LONG gp_RenderClose(SIPTR error, ULONG flags)
{
    LONG err = (LONG)error;

    (void)flags;
    if (gp_Image != NULL && err == PDERR_NOERR && gp_Row == gp_ImageHeight)
    {
        struct GPMsg m;

        memset(&m, 0, sizeof(m));
        m.gm_Command = GPCMD_PRINT;
        m.gm_ImageWidth = gp_ImageWidth;
        m.gm_ImageHeight = gp_ImageHeight;
        m.gm_Pixels = gp_Image;
        m.gm_PlaceLeft = gp_PlaceLeft;
        m.gm_PlaceTop = gp_PlaceTop;
        m.gm_PlaceWidth = gp_PlaceWidth;
        m.gm_PlaceHeight = gp_PlaceHeight;
        m.gm_JobID = gp_JobID;
        err = gp_Call(&m);
        if (err == PDERR_NOERR)
            gp_JobOpen = TRUE;
    }
    FreeVec(gp_Image);
    gp_Image = NULL;
    return err;
}

static LONG gp_Render(SIPTR ct, LONG x, LONG y, LONG status)
{
    switch (status)
    {
    case PRS_PREINIT:
        return gp_RenderPreInit((struct IODRPReq *)ct, x);
    case PRS_INIT:
        return gp_RenderInit((struct IODRPReq *)ct, x, y);
    case PRS_TRANSFER:
        return gp_RenderTransfer((struct PrtInfo *)ct, y);
    case PRS_CLOSE:
        return gp_RenderClose(ct, x);
    case PRS_FLUSH:
    case PRS_CLEAR:
    case PRS_CONVERT:
    case PRS_CORRECT:
        /* Gutenprint does its own colour conversion, scaling and dithering. */
        return PDERR_NOERR;
    default:
        return PDERR_CANCEL;
    }
}

static LONG gp_DoSpecial(UWORD *command, UBYTE output_buffer[],
    BYTE *current_line_position, BYTE *current_line_spacing,
    BYTE *crlf_flag, UBYTE params[])
{
    (void)command; (void)output_buffer; (void)current_line_position;
    (void)current_line_spacing; (void)crlf_flag; (void)params;
    return -2;
}

/* The Preferences paper size code for a page, by its dimensions. */
static UWORD gp_PaperSize(LONG width, LONG height)
{
    static const struct { UWORD code; UWORD width, height; } sizes[] = {
        /* in GP_LAYOUT_DPI dots, portrait */
        { US_LETTER, 2550,  3300  },
        { US_LEGAL,  2550,  4200  },
        { N_TRACTOR, 2850,  3300  },
        { W_TRACTOR, 4458,  3300  },
        { EURO_A0,   9933,  14043 },
        { EURO_A1,   7016,  9933  },
        { EURO_A2,   4961,  7016  },
        { EURO_A3,   3508,  4961  },
        { EURO_A4,   2480,  3508  },
        { EURO_A5,   1748,  2480  },
        { EURO_A6,   1240,  1748  },
        { EURO_A7,   874,   1240  },
        { EURO_A8,   614,   874   },
    };
    const LONG tolerance = GP_LAYOUT_DPI / 10;
    ULONG i;

    if (width > height)
    {
        LONG t = width;
        width = height;
        height = t;
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        if (width >= sizes[i].width - tolerance && width <= sizes[i].width + tolerance
            && height >= sizes[i].height - tolerance && height <= sizes[i].height + tolerance)
            return sizes[i].code;
    }
    return CUSTOM;
}

/* What an application sees of the printer through printer.device: the
   driver's extended data and the preferences, from the profile's page. */
static void gp_ApplyGeometry(void)
{
    struct Preferences *prefs = &PD->pd_Preferences;
    LONG columns = gp_AreaWidth * 10 / GP_LAYOUT_DPI;

    PED->ped_XDotsInch = GP_LAYOUT_DPI;
    PED->ped_YDotsInch = GP_LAYOUT_DPI;
    PED->ped_MaxXDots = gp_AreaWidth;
    PED->ped_MaxYDots = gp_AreaHeight;
    PED->ped_MaxColumns = columns > 255 ? 255 : columns;

    prefs->PaperSize = gp_PaperSize(gp_PageWidth, gp_PageHeight);
    prefs->PaperType = SINGLE;
    prefs->PaperLength = gp_PageHeight * 6 / GP_LAYOUT_DPI;
    prefs->PrintMaxWidth = gp_AreaWidth * 10 / GP_LAYOUT_DPI;
    prefs->PrintMaxHeight = gp_AreaHeight * 10 / GP_LAYOUT_DPI;
}

/* The page and printable area in GP_LAYOUT_DPI dots, from the renderer. */
static LONG gp_Geometry(void)
{
    struct GPMsg m;
    LONG err;

    memset(&m, 0, sizeof(m));
    m.gm_Command = GPCMD_GEOMETRY;
    err = gp_Call(&m);
    if (err == PDERR_NOERR)
    {
        gp_PageWidth = m.gm_PageWidth;
        gp_PageHeight = m.gm_PageHeight;
        gp_AreaLeft = m.gm_AreaLeft;
        gp_AreaTop = m.gm_AreaTop;
        gp_AreaWidth = m.gm_AreaWidth;
        gp_AreaHeight = m.gm_AreaHeight;
        gp_ApplyGeometry();
    }
    return err;
}

/* PRD_READPREFS: the unit's preferences in the V44 structures. printer.device
   copies the unit's prefs file into pd_Preferences when it makes the unit;
   this is that copy the other way round, with the profile's page in the
   paper fields. */
static LONG gp_ReadPrefs(struct IOPrefsReq *req)
{
    const struct Preferences *prefs = &PD->pd_Preferences;
    struct PrinterTxtPrefs *txt = req->io_TxtPrefs;
    struct PrinterUnitPrefs *unit = req->io_UnitPrefs;
    struct PrinterDeviceUnitPrefs *devunit = req->io_DevUnitPrefs;
    struct PrinterGfxPrefs *gfx = req->io_GfxPrefs;

    if (txt != NULL)
    {
        memset(txt, 0, sizeof(*txt));
        strncpy((char *)txt->pt_Driver, (const char *)prefs->PrinterFilename, DRIVERNAMESIZE - 1);
        txt->pt_Port = strcmp((const char *)prefs->PrtDevName, "serial") == 0 ? PP_SERIAL : PP_PARALLEL;
        txt->pt_PaperType = prefs->PaperType >> 7;
        txt->pt_PaperSize = prefs->PaperSize >> 4;
        txt->pt_PaperLength = prefs->PaperLength;
        txt->pt_Pitch = prefs->PrintPitch >> 10;
        txt->pt_Spacing = prefs->PrintSpacing >> 9;
        txt->pt_LeftMargin = prefs->PrintLeftMargin;
        txt->pt_RightMargin = prefs->PrintRightMargin;
        txt->pt_Quality = prefs->PrintQuality >> 8;
    }
    if (unit != NULL)
    {
        memset(unit, 0, sizeof(*unit));
        unit->pu_UnitNum = prefs->DefaultPrtUnit;
        strncpy((char *)unit->pu_DeviceName, (const char *)prefs->PrtDevName, DEVICENAMESIZE - 1);
    }
    if (devunit != NULL)
    {
        memset(devunit, 0, sizeof(*devunit));
        devunit->pd_UnitNum = PD->pd_UnitNumber;
        strncpy((char *)devunit->pd_UnitName, FindTask(NULL)->tc_Node.ln_Name, UNITNAMESIZE - 1);
    }
    if (gfx != NULL)
    {
        memset(gfx, 0, sizeof(*gfx));
        gfx->pg_Aspect = prefs->PrintAspect;
        gfx->pg_Shade = prefs->PrintShade;
        gfx->pg_Image = prefs->PrintImage;
        gfx->pg_Threshold = prefs->PrintThreshold;
        gfx->pg_GraphicFlags = prefs->PrintFlags;
        gfx->pg_PrintDensity = prefs->PrintDensity;
        gfx->pg_PrintMaxWidth = prefs->PrintMaxWidth;
        gfx->pg_PrintMaxHeight = prefs->PrintMaxHeight;
        gfx->pg_PrintXOffset = prefs->PrintXOffset;
    }
    return PDERR_NOERR;
}

/* PRD_EDITPREFS: GutenprintPrefs on this unit, the way TurboPrint put its
   own dialog behind the system one. Runs until the editor quits; the page is
   read again when it returns, so a saved change is seen at once. */
static LONG gp_EditPrefs(void)
{
    char cmd[] = GP_PREFS " UNIT 00";
    char *digits = cmd + sizeof(cmd) - 3;
    ULONG unit = PD->pd_UnitNumber;
    BPTR in, out;
    LONG rc;

    digits[0] = '0' + (unit / 10) % 10;
    digits[1] = '0' + unit % 10;
    in = Open("NIL:", MODE_OLDFILE);
    out = Open("NIL:", MODE_NEWFILE);
    rc = SystemTags(cmd,
        SYS_Input, in,
        SYS_Output, out,
        NP_StackSize, 2000000,
        NP_Name, (IPTR)"GutenprintPrefs",
        TAG_END);
    Close(in);
    Close(out);
    if (rc == -1)
    {
        bug("[Gutenprint] unit %lu: cannot run %s\n", (unsigned long)unit, GP_PREFS);
        return IOERR_OPENFAIL;
    }
    return gp_Geometry();
}

/* The V44 preferences commands. The profile files belong to GutenprintPrefs,
   so reset, save and write are not commands here. */
static LONG gp_DoPreferences(union printerIO *ior, LONG command)
{
    switch (command)
    {
    case PRD_READPREFS:
        return gp_ReadPrefs((struct IOPrefsReq *)ior);
    case PRD_LOADPREFS:
    case PRD_USEPREFS:
        return gp_Geometry();
    case PRD_EDITPREFS:
        return gp_EditPrefs();
    default:
        return IOERR_NOCMD;
    }
}

static void gp_TextFree(void)
{
    FreeVec(gp_Text);
    gp_Text = NULL;
    if (gp_TextFont != NULL)
        CloseFont(gp_TextFont);
    gp_TextFont = NULL;
    if (DiskfontBase != NULL)
        CloseLibrary(DiskfontBase);
    DiskfontBase = NULL;
}

/* Starts a text page sized from the printable area and the font. */
static BOOL gp_TextStart(void)
{
    struct TextAttr ta = { (STRPTR)GP_TEXT_FONT, GP_TEXT_POINTS * GP_LAYOUT_DPI / 72, 0, 0 };

    if (gp_Text != NULL)
        return TRUE;
    if (gp_AreaWidth <= 0 && gp_Geometry() != PDERR_NOERR)
        return FALSE;
    if (gp_TextFont == NULL)
    {
        if (DiskfontBase == NULL)
            DiskfontBase = OpenLibrary("diskfont.library", 39);
        if (DiskfontBase != NULL)
            gp_TextFont = OpenDiskFont(&ta);
        if (gp_TextFont == NULL)
        {
            bug("[Gutenprint] no " GP_TEXT_FONT ", text printed in topaz\n");
            ta.ta_Name = (STRPTR)"topaz.font";
            ta.ta_YSize = 8;
            gp_TextFont = OpenFont(&ta);
        }
        if (gp_TextFont == NULL)
            return FALSE;
    }
    gp_TextCols = gp_AreaWidth / gp_TextFont->tf_XSize;
    gp_TextRows = gp_AreaHeight / gp_TextFont->tf_YSize;
    if (gp_TextCols < 1 || gp_TextRows < 1)
        return FALSE;
    gp_Text = AllocVec(gp_TextCols * gp_TextRows, MEMF_ANY);
    if (gp_Text == NULL)
        return FALSE;
    memset(gp_Text, ' ', gp_TextCols * gp_TextRows);
    gp_TextRow = gp_TextCol = 0;
    gp_TextUsed = FALSE;
    return TRUE;
}

/* Draws the text page and prints it as a page of the job. */
static LONG gp_TextFlush(void)
{
    struct BitMap *bm;
    struct RastPort rp;
    struct GPMsg m;
    UBYTE *rgb, *pens;
    LONG row, x, y, err = PDERR_NOERR;

    if (gp_Text == NULL || !gp_TextUsed)
        return PDERR_NOERR;
    bm = AllocBitMap(gp_AreaWidth, gp_AreaHeight, 1, BMF_CLEAR, NULL);
    rgb = AllocVec(gp_AreaWidth * gp_AreaHeight * 3, MEMF_ANY);
    pens = AllocVec(((gp_AreaWidth + 15) & ~15), MEMF_ANY);
    if (bm == NULL || rgb == NULL || pens == NULL)
        err = PDERR_BUFFERMEMORY;
    else
    {
        struct RastPort temp;
        struct BitMap *tempbm = AllocBitMap(((gp_AreaWidth + 15) & ~15), 1, 1, 0, bm);

        InitRastPort(&rp);
        rp.BitMap = bm;
        SetFont(&rp, gp_TextFont);
        SetAPen(&rp, 1);
        SetDrMd(&rp, JAM1);
        for (row = 0; row < gp_TextRows; row++)
        {
            UBYTE *line = gp_Text + row * gp_TextCols;
            LONG len = gp_TextCols;

            while (len > 0 && line[len - 1] == ' ')
                len--;
            if (len == 0)
                continue;
            Move(&rp, 0, row * gp_TextFont->tf_YSize + gp_TextFont->tf_Baseline);
            Text(&rp, (STRPTR)line, len);
        }
        temp = rp;
        temp.Layer = NULL;
        temp.BitMap = tempbm;
        for (y = 0; y < gp_AreaHeight; y++)
        {
            UBYTE *dst = rgb + y * gp_AreaWidth * 3;

            ReadPixelLine8(&rp, 0, y, gp_AreaWidth, pens, tempbm != NULL ? &temp : NULL);
            for (x = 0; x < gp_AreaWidth; x++)
            {
                UBYTE v = pens[x] ? 0 : 255;

                *dst++ = v;
                *dst++ = v;
                *dst++ = v;
            }
        }
        if (tempbm != NULL)
            FreeBitMap(tempbm);
        memset(&m, 0, sizeof(m));
        m.gm_Command = GPCMD_PRINT;
        m.gm_ImageWidth = gp_AreaWidth;
        m.gm_ImageHeight = gp_AreaHeight;
        m.gm_Pixels = rgb;
        m.gm_PlaceWidth = gp_AreaWidth;
        m.gm_PlaceHeight = gp_AreaHeight;
        m.gm_JobID = gp_JobID;
        err = gp_Call(&m);
        if (err == PDERR_NOERR)
            gp_JobOpen = TRUE;
    }
    FreeVec(pens);
    FreeVec(rgb);
    if (bm != NULL)
        FreeBitMap(bm);
    memset(gp_Text, ' ', gp_TextCols * gp_TextRows);
    gp_TextRow = gp_TextCol = 0;
    gp_TextUsed = FALSE;
    return err;
}

static void gp_TextNewLine(void)
{
    gp_TextCol = 0;
    if (++gp_TextRow >= gp_TextRows)
        gp_TextFlush();
}

/* Text goes onto the text page instead of to the printer (nothing is
   written here, so the function returns 0 bytes). */
static LONG gp_ConvFunc(UBYTE *buf, UBYTE c, LONG crlf_flag)
{
    (void)buf;
    (void)crlf_flag;
    if (!gp_TextStart())
        return 0;
    switch (c)
    {
    case '\r':
        gp_TextCol = 0;
        break;
    case '\n':
        gp_TextUsed = TRUE;
        gp_TextNewLine();
        break;
    case '\f':
        gp_TextUsed = TRUE;
        gp_TextFlush();
        break;
    case '\t':
        gp_TextCol = (gp_TextCol / GP_TEXT_TAB + 1) * GP_TEXT_TAB;
        if (gp_TextCol >= gp_TextCols)
            gp_TextNewLine();
        break;
    default:
        if (c < 0x20 || (c >= 0x7f && c < 0xa0))
            break;
        if (gp_TextCol >= gp_TextCols)
            gp_TextNewLine();
        gp_Text[gp_TextRow * gp_TextCols + gp_TextCol++] = c;
        gp_TextUsed = TRUE;
        break;
    }
    return 0;
}
