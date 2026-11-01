/*
 * gp-dumptest IMAGE.ppm [TITLE] - print a PPM through printer.device
 * (PRD_DUMPRPORT), as an application would: drawn into a 24-bit bitmap,
 * 6 x 4 inches, centred. TITLE names the document for print queues
 * (PRD_SETJOBINFO, AROS).
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/printer.h>
#include <graphics/modeid.h>
#include <exec/memory.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    int width, height, maxval;
    UBYTE *pixels;
    struct BitMap *bm;
    struct RastPort rp;
    struct MsgPort *port;
    struct IODRPReq *io;
    int rc = 20;

    if (argc < 2 || argc > 3 || (f = fopen(argv[1], "rb")) == NULL)
    {
        printf("usage: gp-dumptest IMAGE.ppm [TITLE]\n");
        return 20;
    }
    if (fscanf(f, "P6 %d %d %d", &width, &height, &maxval) != 3 || maxval != 255 ||
        fgetc(f) == EOF || (pixels = malloc((size_t)width * height * 3)) == NULL ||
        fread(pixels, 1, (size_t)width * height * 3, f) != (size_t)width * height * 3)
    {
        printf("gp-dumptest: cannot read %s\n", argv[1]);
        fclose(f);
        return 20;
    }
    fclose(f);
    bm = AllocBitMap(width, height, 24, BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_RGB24), NULL);
    if (bm == NULL)
    {
        printf("gp-dumptest: no 24-bit bitmap\n");
        return 20;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;
    WritePixelArray(pixels, 0, 0, width * 3, &rp, 0, 0, width, height, RECTFMT_RGB);

    port = CreateMsgPort();
    io = (struct IODRPReq *)CreateIORequest(port, sizeof(*io));
    if (io != NULL && OpenDevice((CONST_STRPTR)"printer.device", 0, (struct IORequest *)io, 0) == 0)
    {
#ifdef PRD_SETJOBINFO
        if (argc == 3)
        {
            struct TagItem info[] = { { PJIA_Document, (IPTR)argv[2] }, { TAG_END, 0 } };

            io->io_Command = PRD_SETJOBINFO;
            ((struct IOStdReq *)io)->io_Data = info;
            DoIO((struct IORequest *)io);
        }
#endif
        io->io_Command = PRD_DUMPRPORT;
        io->io_RastPort = &rp;
        io->io_ColorMap = NULL;
        io->io_Modes = INVALID_ID;
        io->io_SrcX = 0;
        io->io_SrcY = 0;
        io->io_SrcWidth = width;
        io->io_SrcHeight = height;
        io->io_DestCols = 6000;
        io->io_DestRows = 4000;
        io->io_Special = SPECIAL_MILCOLS | SPECIAL_MILROWS | SPECIAL_CENTER;
        struct DateStamp t0, t1;

        DateStamp(&t0);
        DoIO((struct IORequest *)io);
        DateStamp(&t1);
        printf("gp-dumptest: PRD_DUMPRPORT %dx%d -> %ldx%ld dots, io_Error=%d, %ld ms\n", width, height,
            (long)io->io_DestCols, (long)io->io_DestRows, io->io_Error,
            (long)((t1.ds_Minute - t0.ds_Minute) * 60000 + (t1.ds_Tick - t0.ds_Tick) * 20));
        rc = io->io_Error == 0 ? 0 : 10;
        CloseDevice((struct IORequest *)io);
    }
    else
        printf("gp-dumptest: cannot open printer.device\n");
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeBitMap(bm);
    free(pixels);
    return rc;
}
