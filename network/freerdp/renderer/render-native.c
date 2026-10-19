/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Native page output through stock printer.device; Gutenprint remains its driver. */
#include "render-native.h"
#include <devices/printer.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <stdlib.h>
#include <stdio.h>
struct render_printer { struct MsgPort *port; struct IODRPReq *io; int opened; };
struct render_printer *render_printer_open(void)
{
    struct render_printer *p=calloc(1,sizeof(*p)); if (!p) return NULL;
    p->port=CreateMsgPort();
    if (p->port) p->io=(struct IODRPReq *)CreateIORequest(p->port,sizeof(*p->io));
    if (!p->io) { render_printer_close(p); return NULL; }
    int error=OpenDevice((CONST_STRPTR)"printer.device",0,(struct IORequest *)p->io,0);
    if (error) { fprintf(stderr,"printer.device open error=%d\n",error); render_printer_close(p); return NULL; }
    p->opened=1; return p;
}
int render_printer_page(struct render_printer *p,const unsigned char *rgb,int width,int height,int stride,int dpi)
{
    if (!p || !rgb || width<1 || height<1 || width>6000 || height>6000 || stride<width*3 || dpi<1) return 0;
    struct BitMap *bitmap=AllocBitMap(width,height,24,BMF_CLEAR,NULL); if (!bitmap) return 0;
    struct RastPort rp; InitRastPort(&rp); rp.BitMap=bitmap;
    ULONG written=WritePixelArray((APTR)rgb,0,0,stride,&rp,0,0,width,height,RECTFMT_RGB);
    int ok=0;
    if (written==(ULONG)width*(ULONG)height) {
        struct IODRPReq *io=p->io;
        io->io_Command=PRD_DUMPRPORT; io->io_Error=0; io->io_RastPort=&rp;
        io->io_ColorMap=NULL; io->io_Modes=INVALID_ID;
        io->io_SrcX=io->io_SrcY=0; io->io_SrcWidth=width; io->io_SrcHeight=height;
        io->io_DestCols=width*1000/dpi; io->io_DestRows=height*1000/dpi;
        io->io_Special=SPECIAL_MILCOLS|SPECIAL_MILROWS|SPECIAL_CENTER;
        LONG error=DoIO((struct IORequest *)io);
        fprintf(stdout,"PRINTER_PAGE_RESULT=%ld\n",(long)error); fflush(stdout);
        ok=error==0;
    }
    FreeBitMap(bitmap); return ok;
}
void render_printer_close(struct render_printer *p)
{
    if (!p) return;
    if (p->opened) CloseDevice((struct IORequest *)p->io);
    if (p->io) DeleteIORequest((struct IORequest *)p->io);
    if (p->port) DeleteMsgPort(p->port);
    free(p);
}

void render_printer_error(const char *path)
{
    struct EasyStruct request={sizeof(request),0,(STRPTR)"Remote Desktop printing",
        (STRPTR)"Document printing failed.\nCheck printer preferences and the job log:\n%s.log",(STRPTR)"OK"};
    IPTR args[]={(IPTR)path}; EasyRequestArgs(NULL,&request,NULL,(RAWARG)args);
}
