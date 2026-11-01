/*
 * gppreview - the print preview window of GutenprintRender.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Shows a page the way it will print, the way TurboPrint's preview did:
 * the paper, the printable area and the page image placed on it, with
 * brightness, contrast, saturation and gamma sliders whose effect is shown
 * on the image at once (core/gpcolor.c, Gutenprint's own curves). The
 * application waits in printer.device until Print, Print all or Cancel.
 */
#include <aros/asmcall.h>
#include <aros/debug.h>
#include <clib/alib_protos.h>
#include <exec/memory.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <graphics/gfxmacros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gppreview.h"
#include "../core/gpproto.h"

#define GPA_View_Page     (TAG_USER | 0x47600001)  /* struct gppreview_page *, at OM_NEW */
#define GPM_View_Adjust   (TAG_USER | 0x47600002)  /* the page's adjust changed: show it */

enum { ID_PRINT = 1, ID_PRINT_ALL, ID_CANCEL, ID_RESET };

static const char *const slider_names[4] = { "Brightness", "Contrast", "Saturation", "Gamma" };
/* slider ranges in hundredths: Gutenprint's bounds, saturation cut at 3 */
static const LONG slider_min[4] = { 0, 0, 0, 10 };
static const LONG slider_max[4] = { 200, 400, 300, 400 };

/* ------------------------------------------------------------ the page */

struct view_data
{
    struct gppreview_page *page;
    struct gpcolor_lut lut;
    UBYTE *base;                /* the image box-filtered to its size on screen */
    UBYTE *shown;               /* base with the adjustments */
    LONG base_w, base_h;
    LONG ix, iy;                /* where the image was last drawn */
};

static struct MUI_CustomClass *view_class;

/* The image at iw x ih, every screen pixel the mean of its source pixels. */
static UBYTE *scale_image(const struct gppreview_page *p, LONG iw, LONG ih)
{
    UBYTE *out = AllocVec((ULONG)iw * ih * 3, MEMF_ANY);
    LONG dx, dy;

    if (out == NULL)
        return NULL;
    for (dy = 0; dy < ih; dy++)
    {
        LONG sy0 = dy * p->image_height / ih;
        LONG sy1 = (dy + 1) * p->image_height / ih;
        UBYTE *dst = out + (ULONG)dy * iw * 3;

        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (dx = 0; dx < iw; dx++)
        {
            LONG sx0 = dx * p->image_width / iw;
            LONG sx1 = (dx + 1) * p->image_width / iw;
            ULONG sum[3] = { 0, 0, 0 }, n = 0;
            LONG x, y;

            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            for (y = sy0; y < sy1; y++)
            {
                const UBYTE *src = p->pixels + ((ULONG)y * p->image_width + sx0) * 3;

                for (x = sx0; x < sx1; x++, n++)
                {
                    sum[0] += *src++;
                    sum[1] += *src++;
                    sum[2] += *src++;
                }
            }
            *dst++ = sum[0] / n;
            *dst++ = sum[1] / n;
            *dst++ = sum[2] / n;
        }
    }
    return out;
}

static void view_free(struct view_data *d)
{
    FreeVec(d->base);
    FreeVec(d->shown);
    d->base = d->shown = NULL;
    d->base_w = d->base_h = 0;
}

/* The page's scale and position inside the object, and the image's. */
static void view_layout(Object *obj, const struct gppreview_page *p, LONG *px, LONG *py,
    LONG *pw, LONG *ph, LONG *ix, LONG *iy, LONG *iw, LONG *ih, double *scale)
{
    LONG w = _mwidth(obj), h = _mheight(obj);
    LONG left, top, width, height;

    *scale = (double)(w - 16) / p->page_width;
    if ((double)(h - 16) / p->page_height < *scale)
        *scale = (double)(h - 16) / p->page_height;
    *pw = (LONG)(p->page_width * *scale);
    *ph = (LONG)(p->page_height * *scale);
    *px = _mleft(obj) + (w - *pw) / 2;
    *py = _mtop(obj) + (h - *ph) / 2;
    if (p->place_width > 0)
    {
        left = p->area_left + p->place_left;
        top = p->area_top + p->place_top;
        width = p->place_width;
        height = p->place_height;
    }
    else
    {
        left = p->area_left;
        top = p->area_top;
        width = p->area_width;
        height = p->area_height;
    }
    *ix = *px + (LONG)(left * *scale + 0.5);
    *iy = *py + (LONG)(top * *scale + 0.5);
    *iw = (LONG)(width * *scale + 0.5);
    *ih = (LONG)(height * *scale + 0.5);
    if (*iw < 1)
        *iw = 1;
    if (*ih < 1)
        *ih = 1;
}

static void view_rect(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h)
{
    Move(rp, x, y);
    Draw(rp, x + w - 1, y);
    Draw(rp, x + w - 1, y + h - 1);
    Draw(rp, x, y + h - 1);
    Draw(rp, x, y);
}

BOOPSI_DISPATCHER(IPTR, view_dispatcher, cl, obj, msg)
{
    switch (msg->MethodID)
    {
    case OM_NEW:
    {
        struct TagItem *tags = ((struct opSet *)msg)->ops_AttrList;
        Object *o = (Object *)DoSuperMethodA(cl, obj, msg);

        if (o != NULL)
        {
            struct view_data *d = INST_DATA(cl, o);

            d->page = (struct gppreview_page *)GetTagData(GPA_View_Page, 0, tags);
            gpcolor_prepare(&d->lut, &d->page->adjust);
        }
        return (IPTR)o;
    }
    case OM_DISPOSE:
        view_free(INST_DATA(cl, obj));
        break;
    case MUIM_AskMinMax:
    {
        struct MUI_MinMax *mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;

        DoSuperMethodA(cl, obj, msg);
        mm->MinWidth += 200;
        mm->MinHeight += 240;
        mm->DefWidth += 440;
        mm->DefHeight += 560;
        mm->MaxWidth = MUI_MAXMAX;
        mm->MaxHeight = MUI_MAXMAX;
        return 0;
    }
    case GPM_View_Adjust:
    {
        struct view_data *d = INST_DATA(cl, obj);

        gpcolor_prepare(&d->lut, &d->page->adjust);
        if (d->base != NULL && d->shown != NULL)
        {
            gpcolor_apply(&d->lut, d->base, d->shown, (size_t)d->base_w * d->base_h);
            MUI_Redraw(obj, MADF_DRAWUPDATE);
        }
        return 0;
    }
    case MUIM_Draw:
    {
        struct view_data *d = INST_DATA(cl, obj);
        const struct gppreview_page *p = d->page;
        struct RastPort *rp;
        LONG px, py, pw, ph, ix, iy, iw, ih;
        double scale;

        DoSuperMethodA(cl, obj, msg);
        if (!(((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)))
            return 0;
        rp = _rp(obj);
        view_layout(obj, p, &px, &py, &pw, &ph, &ix, &iy, &iw, &ih, &scale);
        if (iw != d->base_w || ih != d->base_h || d->base == NULL)
        {
            view_free(d);
            d->base = scale_image(p, iw, ih);
            d->shown = AllocVec((ULONG)iw * ih * 3, MEMF_ANY);
            if (d->base != NULL && d->shown != NULL)
            {
                d->base_w = iw;
                d->base_h = ih;
                gpcolor_apply(&d->lut, d->base, d->shown, (size_t)iw * ih);
            }
            else
                view_free(d);
        }
        if (((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT)
        {
            SetAPen(rp, _pens(obj)[MPEN_BACKGROUND]);
            RectFill(rp, _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj));
            /* the sheet and its shadow */
            SetAPen(rp, _pens(obj)[MPEN_SHADOW]);
            RectFill(rp, px + 3, py + 3, px + pw + 2, py + ph + 2);
            SetAPen(rp, _pens(obj)[MPEN_SHINE]);
            RectFill(rp, px, py, px + pw - 1, py + ph - 1);
            SetAPen(rp, _pens(obj)[MPEN_SHADOW]);
            view_rect(rp, px, py, pw, ph);
            /* the printable area, dotted */
            SetAPen(rp, _pens(obj)[MPEN_FILL]);
            SetDrPt(rp, 0xCCCC);
            view_rect(rp, px + (LONG)(p->area_left * scale), py + (LONG)(p->area_top * scale),
                (LONG)(p->area_width * scale), (LONG)(p->area_height * scale));
            SetDrPt(rp, 0xFFFF);
        }
        if (d->shown != NULL)
            WritePixelArray(d->shown, 0, 0, iw * 3, rp, ix, iy, iw, ih, RECTFMT_RGB);
        d->ix = ix;
        d->iy = iy;
        return 0;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

/* ---------------------------------------------------------- the sliders */

/* A slider in hundredths that shows the value. */
struct slider_data
{
    char text[16];
};

static struct MUI_CustomClass *slider_class;

BOOPSI_DISPATCHER(IPTR, slider_dispatcher, cl, obj, msg)
{
    if (msg->MethodID == MUIM_Numeric_Stringify)
    {
        struct slider_data *d = INST_DATA(cl, obj);
        LONG v = ((struct MUIP_Numeric_Stringify *)msg)->value;

        snprintf(d->text, sizeof(d->text), "%ld.%02ld", (long)(v / 100), (long)(v % 100));
        return (IPTR)d->text;
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

/* ----------------------------------------------------------- the window */

static struct
{
    struct gppreview_page *page;
    Object *app, *win, *view, *slider[4], *keep, *reset, *print, *print_all, *cancel;
    struct Hook adjust_hook;
} w;

static double *adjust_field(struct gpcolor_adjust *a, int i)
{
    switch (i)
    {
    case 0: return &a->brightness;
    case 1: return &a->contrast;
    case 2: return &a->saturation;
    default: return &a->gamma;
    }
}

static LONG to_hundredths(double v)
{
    return (LONG)(v * 100 + (v < 0 ? -0.5 : 0.5));
}

AROS_UFH3(IPTR, adjust_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, app, A2), AROS_UFHA(IPTR *, args, A1))
{
    AROS_USERFUNC_INIT
    int i;

    for (i = 0; i < 4; i++)
        if (w.slider[i] != NULL)
        {
            IPTR v = 0;

            get(w.slider[i], MUIA_Numeric_Value, &v);
            *adjust_field(&w.page->adjust, i) = (LONG)v / 100.0;
        }
    DoMethod(w.view, GPM_View_Adjust);
    return 0;
    AROS_USERFUNC_EXIT
}

static void set_sliders(const struct gpcolor_adjust *a)
{
    int i;

    for (i = 0; i < 4; i++)
        if (w.slider[i] != NULL)
            set(w.slider[i], MUIA_Numeric_Value, to_hundredths(*adjust_field((struct gpcolor_adjust *)a, i)));
}

static Object *build_window(struct gppreview_page *p, const char *title, const char *info)
{
    Object *sliders = ColGroup(2), End;
    int i;

    for (i = 0; i < 4 && sliders != NULL; i++)
    {
        if (!p->offers[i])
            continue;
        w.slider[i] = NewObject(slider_class->mcc_Class, NULL,
            MUIA_Numeric_Min, slider_min[i], MUIA_Numeric_Max, slider_max[i],
            MUIA_Numeric_Value, to_hundredths(*adjust_field(&p->adjust, i)),
            MUIA_CycleChain, TRUE, TAG_END);
        if (w.slider[i] == NULL)
            continue;
        DoMethod(sliders, OM_ADDMEMBER, (IPTR)Label1(slider_names[i]));
        DoMethod(sliders, OM_ADDMEMBER, (IPTR)w.slider[i]);
    }
    return WindowObject,
        MUIA_Window_Title, (IPTR)title,
        MUIA_Window_ID, MAKE_ID('G', 'P', 'P', 'V'),
        MUIA_Window_Width, MUIV_Window_Width_Screen(60),
        MUIA_Window_Height, MUIV_Window_Height_Screen(75),
        WindowContents, (IPTR)(VGroup,
            Child, (IPTR)(HGroup,
                Child, (IPTR)(w.view = NewObject(view_class->mcc_Class, NULL,
                    MUIA_Frame, MUIV_Frame_Virtual, MUIA_Weight, 70, GPA_View_Page, (IPTR)p, TAG_END)),
                Child, (IPTR)(VGroup, MUIA_Weight, 30,
                    Child, (IPTR)(VGroup, GroupFrameT("Colour"),
                        Child, (IPTR)sliders,
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(w.keep = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_Keep for this printer")),
                            Child, (IPTR)LLabel1("_Keep for this printer"),
                            Child, (IPTR)HSpace(0),
                            Child, (IPTR)(w.reset = SimpleButton("_Reset")),
                            End),
                        End),
                    Child, (IPTR)(TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                        MUIA_Text_Contents, (IPTR)info, MUIA_Text_SetMin, FALSE, End),
                    Child, (IPTR)HVSpace,
                    End),
                End),
            Child, (IPTR)(HGroup,
                Child, (IPTR)(w.print = SimpleButton("_Print")),
                Child, (IPTR)(w.print_all = SimpleButton("Print _all")),
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(w.cancel = SimpleButton("_Cancel")),
                End),
            End),
        End;
}

static void log_choice(const char *choice, const struct gpcolor_adjust *a, int keep)
{
    bug("[GutenprintRender] preview: %s; brightness %ld.%02ld contrast %ld.%02ld "
        "saturation %ld.%02ld gamma %ld.%02ld%s\n", choice,
        (long)(to_hundredths(a->brightness) / 100), (long)(to_hundredths(a->brightness) % 100),
        (long)(to_hundredths(a->contrast) / 100), (long)(to_hundredths(a->contrast) % 100),
        (long)(to_hundredths(a->saturation) / 100), (long)(to_hundredths(a->saturation) % 100),
        (long)(to_hundredths(a->gamma) / 100), (long)(to_hundredths(a->gamma) % 100),
        keep ? ", kept" : "");
}

int gppreview_show(struct gppreview_page *p)
{
    char title[160], info[300];
    struct gpcolor_adjust initial = p->adjust;
    int result = -1;
    IPTR open = 0;
    ULONG sigs = 0;

    memset(&w, 0, sizeof(w));
    w.page = p;
    p->keep = 0;
    view_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Area, NULL, sizeof(struct view_data), view_dispatcher);
    slider_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Slider, NULL, sizeof(struct slider_data), slider_dispatcher);
    if (view_class == NULL || slider_class == NULL)
    {
        if (view_class != NULL)
            MUI_DeleteCustomClass(view_class);
        if (slider_class != NULL)
            MUI_DeleteCustomClass(slider_class);
        bug("[GutenprintRender] preview: no window (custom classes)\n");
        return GPPREVIEW_FAILED;
    }
    snprintf(title, sizeof(title), "Print preview: %s (printer.device unit %ld), page %ld",
        p->printer, (long)p->unit, (long)p->page_number);
    snprintf(info, sizeof(info), "%s\nPage %ld x %ld mm\nPrintable %ld x %ld mm\nImage %ld x %ld dots at %ld dpi",
        p->model, (long)(p->page_width * 254 / GP_LAYOUT_DPI / 10), (long)(p->page_height * 254 / GP_LAYOUT_DPI / 10),
        (long)(p->area_width * 254 / GP_LAYOUT_DPI / 10), (long)(p->area_height * 254 / GP_LAYOUT_DPI / 10),
        (long)p->image_width, (long)p->image_height, (long)GP_LAYOUT_DPI);
    w.adjust_hook.h_Entry = (HOOKFUNC)(APTR)adjust_func;
    w.app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"GutenprintRender",
        MUIA_Application_Version, (IPTR)"$VER: GutenprintRender 0.2 (23.10.2026)",
        MUIA_Application_Description, (IPTR)"Gutenprint print preview",
        MUIA_Application_Base, (IPTR)"GUTENPRINTPREVIEW",
        SubWindow, (IPTR)(w.win = build_window(p, title, info)),
        End;
    if (w.app == NULL)
    {
        MUI_DeleteCustomClass(view_class);
        MUI_DeleteCustomClass(slider_class);
        bug("[GutenprintRender] preview: no window\n");
        return GPPREVIEW_FAILED;
    }
    DoMethod(w.print, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)w.app, 2,
        MUIM_Application_ReturnID, ID_PRINT);
    DoMethod(w.print_all, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)w.app, 2,
        MUIM_Application_ReturnID, ID_PRINT_ALL);
    DoMethod(w.cancel, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)w.app, 2,
        MUIM_Application_ReturnID, ID_CANCEL);
    DoMethod(w.reset, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)w.app, 2,
        MUIM_Application_ReturnID, ID_RESET);
    DoMethod(w.win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)w.app, 2,
        MUIM_Application_ReturnID, ID_CANCEL);
    {
        int i;

        for (i = 0; i < 4; i++)
            if (w.slider[i] != NULL)
                DoMethod(w.slider[i], MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime, (IPTR)w.app, 2,
                    MUIM_CallHook, (IPTR)&w.adjust_hook);
    }
    set(w.win, MUIA_Window_Open, TRUE);
    get(w.win, MUIA_Window_Open, &open);
    if (!open)
    {
        MUI_DisposeObject(w.app);
        MUI_DeleteCustomClass(view_class);
        MUI_DeleteCustomClass(slider_class);
        bug("[GutenprintRender] preview: the window did not open\n");
        return GPPREVIEW_FAILED;
    }
    bug("[GutenprintRender] preview: page %ld of unit %ld open\n", (long)p->page_number, (long)p->unit);
    while (result < 0)
    {
        IPTR id = DoMethod(w.app, MUIM_Application_NewInput, (IPTR)&sigs);

        switch (id)
        {
        case ID_PRINT:
            result = GPPREVIEW_PRINT;
            break;
        case ID_PRINT_ALL:
            result = GPPREVIEW_PRINT_ALL;
            break;
        case ID_CANCEL:
        case MUIV_Application_ReturnID_Quit:
            result = GPPREVIEW_CANCEL;
            break;
        case ID_RESET:
            set_sliders(&initial);
            break;
        }
        if (result < 0 && sigs != 0)
            Wait(sigs);
    }
    {
        IPTR keep = 0;

        get(w.keep, MUIA_Selected, &keep);
        p->keep = keep != 0 && result != GPPREVIEW_CANCEL;
    }
    set(w.win, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(w.app);
    MUI_DeleteCustomClass(view_class);
    MUI_DeleteCustomClass(slider_class);
    log_choice(result == GPPREVIEW_PRINT ? "print" : result == GPPREVIEW_PRINT_ALL ? "print all" : "cancel",
        &p->adjust, p->keep);
    return result;
}
