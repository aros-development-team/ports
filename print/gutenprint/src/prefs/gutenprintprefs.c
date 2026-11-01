/*
 * GutenprintPrefs - settings editor of the Gutenprint printer driver.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Edits printer profiles (core/gpprofile.h) and binds them to printer.device
 * units. The Paper, Quality, Colour and Advanced tabs are built from
 * Gutenprint's description of the chosen model's parameters (type, level,
 * category, choices), so every model gets a complete editor. "Find network
 * printers" lists the printers on the network with their pictures and supply
 * levels (gpfind.c) and sets a profile and its unit up for the one chosen.
 *
 *   GutenprintPrefs [UNIT=<n>]
 */
#include <aros/asmcall.h>
#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <devices/printer.h>
#include <graphics/modeid.h>
#include <graphics/gfxmacros.h>
#include <graphics/rpattr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <gutenprint/gutenprint.h>

#include "../core/gpjob.h"
#include "../core/gpprofile.h"
#include "gpfind.h"

#define TAB_PAPER    0
#define TAB_QUALITY  1
#define TAB_COLOUR   2
#define TAB_ADVANCED 3
#define GEN_TABS     4
#define MAX_PARAMS   200

/* Parameters the driver sets itself, or that have no place in an editor. */
static const char *const hidden_params[] = {
    "InputImageType", "ChannelBitDepth", "RawChannels", "JobMode", "PageNumber",
    "LUTDumpFile", NULL
};

static const char *const paper_params[] = {
    "PPDFile", "PageSize", "MediaType", "InputSlot", "FullBleed", "Duplex", "Orientation",
    "CDInnerRadius", "CDOuterDiameter", "CDXAdjustment", "CDYAdjustment", NULL
};

static const char *const colour_categories[] = {
    "Basic Image Adjustment", "Output Level Adjustment", "Gamma", "GrayBalance", NULL
};

static const char *const quality_categories[] = {
    "Basic Printer Setup", "Basic Output Adjustment", "Screening Adjustment",
    "Advanced Printer Setup", "Advanced Printer Functionality", "Advanced Output Adjustment",
    "Core Parameter", NULL
};

static const char *level_names[] = { "Basic", "Advanced", "Expert", NULL };
static const int level_max[] = { 0, 2, 5 };
static const char *unit_names[] = {
    "Not used", "Unit 0", "Unit 1", "Unit 2", "Unit 3", "Unit 4", "Unit 5", "Unit 6",
    "Unit 7", "Unit 8", "Unit 9", NULL
};
static const char *tab_titles[] = { "Printer", "Paper", "Quality", "Colour", "Advanced", NULL };

struct param_ui
{
    char name[48];
    stp_parameter_type_t type;
    Object *obj;          /* the control; for lists the string of the poplist */
    int nchoices;
    char (*choice_name)[64];
    char **choice_text;   /* NULL-terminated, for Cycle and Poplist */
    double lower, upper;
    LONG steps;           /* slider steps for a double: 0.001 apart */
};

static struct
{
    Object *app, *win;
    Object *profiles, *add, *copy, *remove, *level;
    Object *name, *unit, *search, *makers, *models, *model_text, *connection, *device_id;
    Object *preview_check;
    Object *pages;
    Object *tab_group[GEN_TABS], *tab_content[GEN_TABS];
    Object *status, *save, *use, *cancel, *defaults, *test, *preview;
    Object *find, *pic_group, *pic, *about;
    Object *fwin, *flist, *fpic_group, *fpic, *finfo, *fgauges, *fuse, *fcancel, *fstatus;
    Object *status_btn, *swin, *spic_group, *spic, *sinfo, *ssupplies, *squeue, *srefresh, *sclose;
    char status_uri[256], status_name[128];
    struct gp_found found[GPFIND_MAX];
    int nfound;

    struct gp_profile *profile;      /* GP_MAX_PROFILES */
    int nprofiles, current;
    char deleted[GP_MAX_PROFILES][64];
    int ndeleted;
    char units[GP_UNITS][64];

    int nmodels;
    char (*model_driver)[64];
    char (*model_name)[96];
    char (*model_maker)[48];
    int *sorted;                     /* model indexes by maker, then natural name order */
    int *filtered;                   /* what the model list shows */
    int nfiltered;
    char (*maker)[48];               /* sorted manufacturer names */
    int nmakers;

    struct param_ui params[MAX_PARAMS];
    int nparams;
    int quiet;                       /* set while the controls are filled in */
} g;

static void select_maker_of_current(void);

/* A slider whose knob shows the parameter value, not the slider step. */
#define GPA_Slider_Lower (TAG_USER | 0x47500001)  /* double *, at OM_NEW */
#define GPA_Slider_Upper (TAG_USER | 0x47500002)  /* double * */
#define GPA_Slider_Int   (TAG_USER | 0x47500003)  /* BOOL */
#define GPA_Slider_Steps (TAG_USER | 0x47500004)  /* LONG */

struct gpslider_data
{
    double lower, upper;
    LONG steps;
    BOOL is_int;
    char text[32];
};

static struct MUI_CustomClass *gpslider_class;

BOOPSI_DISPATCHER(IPTR, gpslider_dispatcher, cl, obj, msg)
{
    switch (msg->MethodID)
    {
    case OM_NEW:
    {
        struct TagItem *tags = ((struct opSet *)msg)->ops_AttrList;
        Object *o = (Object *)DoSuperMethodA(cl, obj, msg);

        if (o != NULL)
        {
            struct gpslider_data *d = INST_DATA(cl, o);
            double *lo = (double *)GetTagData(GPA_Slider_Lower, 0, tags);
            double *hi = (double *)GetTagData(GPA_Slider_Upper, 0, tags);

            d->lower = lo != NULL ? *lo : 0;
            d->upper = hi != NULL ? *hi : 1;
            d->is_int = GetTagData(GPA_Slider_Int, FALSE, tags);
            d->steps = GetTagData(GPA_Slider_Steps, 1000, tags);
        }
        return (IPTR)o;
    }
    case MUIM_Numeric_Stringify:
    {
        struct gpslider_data *d = INST_DATA(cl, obj);
        LONG step = ((struct MUIP_Numeric_Stringify *)msg)->value;

        if (d->is_int)
            snprintf(d->text, sizeof(d->text), "%ld", (long)step);
        else
            snprintf(d->text, sizeof(d->text), "%.3f",
                d->lower + (double)step * (d->upper - d->lower) / d->steps);
        return (IPTR)d->text;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

/* The page of the current settings: paper, printable area and their sizes. */
struct gppreview_data
{
    double page_w, page_h, left, top, width, height;   /* points */
    BOOL valid;
    char caption[96];
};

static struct MUI_CustomClass *gppreview_class;

static void preview_set(Object *o, const double geo[6], const char *paper)
{
    struct gppreview_data *d = INST_DATA(gppreview_class->mcc_Class, o);

    d->valid = geo != NULL;
    if (geo != NULL)
    {
        d->page_w = geo[0];
        d->page_h = geo[1];
        d->left = geo[2];
        d->top = geo[3];
        d->width = geo[4];
        d->height = geo[5];
        snprintf(d->caption, sizeof(d->caption), "%s  %.0f x %.0f mm, printable %.1f x %.1f mm",
            paper, geo[0] * 25.4 / 72, geo[1] * 25.4 / 72, geo[4] * 25.4 / 72, geo[5] * 25.4 / 72);
    }
    else
        snprintf(d->caption, sizeof(d->caption), "No page sizes for this model");
    MUI_Redraw(o, MADF_DRAWOBJECT);
}

BOOPSI_DISPATCHER(IPTR, gppreview_dispatcher, cl, obj, msg)
{
    switch (msg->MethodID)
    {
    case MUIM_AskMinMax:
    {
        struct MUI_MinMax *mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;

        DoSuperMethodA(cl, obj, msg);
        mm->MinWidth += 120;
        mm->MinHeight += 160;
        mm->DefWidth += 220;
        mm->DefHeight += 300;
        mm->MaxWidth = MUI_MAXMAX;
        mm->MaxHeight = MUI_MAXMAX;
        return 0;
    }
    case MUIM_Draw:
    {
        struct gppreview_data *d = INST_DATA(cl, obj);
        struct RastPort *rp;
        LONG x0, y0, w, h, line, pw, ph, px, py;
        double scale;

        DoSuperMethodA(cl, obj, msg);
        if (!(((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT))
            return 0;
        rp = _rp(obj);
        x0 = _mleft(obj);
        y0 = _mtop(obj);
        w = _mwidth(obj);
        line = _font(obj)->tf_YSize + 4;
        h = _mheight(obj) - line;
        SetAPen(rp, _pens(obj)[MPEN_BACKGROUND]);
        RectFill(rp, x0, y0, x0 + w - 1, y0 + _mheight(obj) - 1);
        SetAPen(rp, _pens(obj)[MPEN_TEXT]);
        if (d->valid && d->page_w > 0 && d->page_h > 0 && w > 8 && h > 8)
        {
            scale = (w - 8) / d->page_w;
            if ((h - 8) / d->page_h < scale)
                scale = (h - 8) / d->page_h;
            pw = (LONG)(d->page_w * scale);
            ph = (LONG)(d->page_h * scale);
            px = x0 + (w - pw) / 2;
            py = y0 + (h - ph) / 2;
            SetAPen(rp, _pens(obj)[MPEN_SHINE]);
            RectFill(rp, px, py, px + pw - 1, py + ph - 1);
            SetAPen(rp, _pens(obj)[MPEN_SHADOW]);
            Move(rp, px, py);
            Draw(rp, px + pw - 1, py);
            Draw(rp, px + pw - 1, py + ph - 1);
            Draw(rp, px, py + ph - 1);
            Draw(rp, px, py);
            /* The printable area, dotted. */
            SetAPen(rp, _pens(obj)[MPEN_FILL]);
            SetDrPt(rp, 0xCCCC);
            {
                LONG ax = px + (LONG)(d->left * scale), ay = py + (LONG)(d->top * scale);
                LONG aw = (LONG)(d->width * scale), ah = (LONG)(d->height * scale);

                Move(rp, ax, ay);
                Draw(rp, ax + aw - 1, ay);
                Draw(rp, ax + aw - 1, ay + ah - 1);
                Draw(rp, ax, ay + ah - 1);
                Draw(rp, ax, ay);
            }
            SetDrPt(rp, 0xFFFF);
            SetAPen(rp, _pens(obj)[MPEN_TEXT]);
        }
        SetFont(rp, _font(obj));
        Move(rp, x0 + 2, y0 + _mheight(obj) - line + 2 + _font(obj)->tf_Baseline);
        {
            struct TextExtent te;

            Text(rp, (CONST_STRPTR)d->caption, TextFit(rp, (CONST_STRPTR)d->caption,
                strlen(d->caption), &te, NULL, 1, w - 4, line));
        }
        return 0;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

/* A supply's level as a bar in the supply's own colour, with its name and level on it. */
struct gpsupply_data
{
    LONG level;         /* percent, -1 unknown */
    ULONG rgb;          /* 0xRRGGBB */
    char label[72];
};

static struct MUI_CustomClass *gpsupply_class;

static Object *supply_bar(const char *name, int level, const char *colour)
{
    Object *o = NewObject(gpsupply_class->mcc_Class, NULL, MUIA_Frame, MUIV_Frame_Gauge, TAG_END);
    struct gpsupply_data *d;

    if (o == NULL)
        return NULL;
    d = INST_DATA(gpsupply_class->mcc_Class, o);
    d->level = level;
    d->rgb = colour != NULL && colour[0] == '#' ? strtoul(colour + 1, NULL, 16) : 0x808080;
    if (level < 0)
        snprintf(d->label, sizeof(d->label), "%s: unknown", name);
    else
        snprintf(d->label, sizeof(d->label), "%s: %d%%", name, level);
    return o;
}

BOOPSI_DISPATCHER(IPTR, gpsupply_dispatcher, cl, obj, msg)
{
    switch (msg->MethodID)
    {
    case MUIM_AskMinMax:
    {
        struct MUI_MinMax *mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;

        DoSuperMethodA(cl, obj, msg);
        mm->MinWidth += 120;
        mm->DefWidth += 240;
        mm->MaxWidth = MUI_MAXMAX;
        mm->MinHeight += _font(obj)->tf_YSize + 4;
        mm->DefHeight += _font(obj)->tf_YSize + 4;
        mm->MaxHeight += _font(obj)->tf_YSize + 4;
        return 0;
    }
    case MUIM_Draw:
    {
        struct gpsupply_data *d = INST_DATA(cl, obj);
        struct RastPort *rp = _rp(obj);
        LONG x0 = _mleft(obj), y0 = _mtop(obj), w = _mwidth(obj), h = _mheight(obj);
        LONG filled = d->level > 0 ? w * (d->level > 100 ? 100 : d->level) / 100 : 0;
        ULONG r = (d->rgb >> 16) & 0xff, gr = (d->rgb >> 8) & 0xff, b = d->rgb & 0xff;
        struct TextExtent te;
        LONG n, tx;

        DoSuperMethodA(cl, obj, msg);
        if (!(((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT))
            return 0;
        SetAPen(rp, _pens(obj)[MPEN_BACKGROUND]);
        RectFill(rp, x0, y0, x0 + w - 1, y0 + h - 1);
        if (filled > 0)
            FillPixelArray(rp, x0, y0, filled, h, d->rgb);
        SetFont(rp, _font(obj));
        n = TextFit(rp, (CONST_STRPTR)d->label, strlen(d->label), &te, NULL, 1, w - 4, h);
        tx = x0 + (w - te.te_Width) / 2;
        /* Light text on a dark bar where the text stands on the bar. */
        SetAPen(rp, _pens(obj)[(r * 299 + gr * 587 + b * 114) / 1000 < 80 && tx < x0 + filled
            ? MPEN_SHINE : MPEN_TEXT]);
        SetDrMd(rp, JAM1);
        Move(rp, tx, y0 + (h - _font(obj)->tf_YSize) / 2 + _font(obj)->tf_Baseline);
        Text(rp, (CONST_STRPTR)d->label, n);
        return 0;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

static struct Hook rebuild_hook, param_hook, profile_hook, search_hook, model_hook,
    name_hook, unit_hook, misc_hook, maker_hook;

static int in_list(const char *s, const char *const *list)
{
    int i;

    for (i = 0; list[i] != NULL; i++)
        if (strcmp(s, list[i]) == 0)
            return 1;
    return 0;
}

static const char *category_of(const stp_parameter_t *p)
{
    const char *c = p->category != NULL ? strstr(p->category, "Category=") : NULL;

    return c != NULL ? c + 9 : "";
}

static int tab_of(const stp_parameter_t *p)
{
    char cat[64];
    const char *c = category_of(p);

    snprintf(cat, sizeof(cat), "%.*s", (int)strcspn(c, ","), c);
    if (in_list(p->name, paper_params))
        return TAB_PAPER;
    if (in_list(cat, colour_categories))
        return TAB_COLOUR;
    if (in_list(cat, quality_categories))
        return TAB_QUALITY;
    return TAB_ADVANCED;
}

/* Case-insensitive substring test. */
static int contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay != '\0'; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    return n == 0;
}

static struct gp_profile *cur(void)
{
    return g.current >= 0 && g.current < g.nprofiles ? &g.profile[g.current] : NULL;
}

static void set_status(const char *text)
{
    set(g.status, MUIA_Text_Contents, (IPTR)text);
}

/* stp_vars_t for the current profile, its options applied; NULL without a model. */
static stp_vars_t *profile_vars(const struct gp_profile *p)
{
    const stp_printer_t *printer = p != NULL ? stp_get_printer_by_driver(p->model) : NULL;
    stp_vars_t *v;
    int i;

    if (printer == NULL)
        return NULL;
    v = stp_vars_create();
    stp_set_printer_defaults(v, printer);
    for (i = 0; i < p->noptions; i++)
    {
        char opt[130];

        snprintf(opt, sizeof(opt), "%s=%s", p->opt_name[i], p->opt_value[i]);
        gpjob_set_option(v, opt);
    }
    stp_set_printer_defaults_soft(v, printer);
    return v;
}

/* ---------------------------------------------------------------- profiles */

static int unit_of(const char *name)
{
    int i;

    for (i = 0; i < GP_UNITS; i++)
        if (strcmp(g.units[i], name) == 0)
            return i;
    return -1;
}

static void fill_profile_list(void)
{
    int i;

    set(g.profiles, MUIA_List_Quiet, TRUE);
    DoMethod(g.profiles, MUIM_List_Clear);
    for (i = 0; i < g.nprofiles; i++)
    {
        char line[96];
        int u = unit_of(g.profile[i].name);

        if (u >= 0)
            snprintf(line, sizeof(line), "%s  \033r(unit %d)", g.profile[i].name, u);
        else
            snprintf(line, sizeof(line), "%s", g.profile[i].name);
        DoMethod(g.profiles, MUIM_List_InsertSingle, (IPTR)line, MUIV_List_Insert_Bottom);
    }
    set(g.profiles, MUIA_List_Quiet, FALSE);
    g.quiet++;
    set(g.profiles, MUIA_List_Active, g.current >= 0 ? g.current : MUIV_List_Active_Off);
    g.quiet--;
}

static const char *model_long_name(const char *driver)
{
    const stp_printer_t *p = stp_get_printer_by_driver(driver);

    return p != NULL ? stp_printer_get_long_name(p) : "(no model chosen)";
}

/* Shows the PNG at path (the printer's picture) in group, replacing what was there. */
static void set_picture(Object *group, Object **pic, const char *path)
{
    Object *o;

    if (group == NULL)
        return;
    o = path != NULL && path[0] != '\0'
        ? MUI_NewObject(MUIC_Dtpic, MUIA_Dtpic_Name, (IPTR)path, TAG_DONE)
        : NULL;
    if (o == NULL)
        o = TextObject, MUIA_Text_Contents, (IPTR)"\33c(no picture)", MUIA_FixWidth, 128, MUIA_FixHeight, 128, End;
    DoMethod(group, MUIM_Group_InitChange);
    if (*pic != NULL)
    {
        DoMethod(group, OM_REMMEMBER, (IPTR)*pic);
        MUI_DisposeObject(*pic);
    }
    *pic = o;
    if (o != NULL)
        DoMethod(group, OM_ADDMEMBER, (IPTR)o);
    DoMethod(group, MUIM_Group_ExitChange);
}

static int is_network(const char *connection)
{
    return strncmp(connection, "ipp://", 6) == 0 || strncmp(connection, "socket://", 9) == 0;
}

static void show_profile(void)
{
    struct gp_profile *p = cur();
    int u = p != NULL ? unit_of(p->name) : -1;
    char text[400];

    g.quiet++;
    set(g.name, MUIA_String_Contents, (IPTR)(p != NULL ? p->name : ""));
    set(g.unit, MUIA_Cycle_Active, u + 1);
    set(g.connection, MUIA_String_Contents, (IPTR)(p != NULL ? p->connection : ""));
    set(g.device_id, MUIA_Text_Contents, (IPTR)(p != NULL && p->device_id[0] ? p->device_id : "-"));
    set(g.preview_check, MUIA_Selected, p != NULL && p->preview);
    snprintf(text, sizeof(text), "%s", p != NULL ? model_long_name(p->model) : "");
    set(g.model_text, MUIA_Text_Contents, (IPTR)text);
    set_picture(g.pic_group, &g.pic, p != NULL ? p->picture : NULL);
    snprintf(text, sizeof(text), "\33b%s\33n\n%s", p != NULL ? p->name : "",
        p == NULL ? "" : p->connection[0] ? p->connection : "Connection set in Printer prefs");
    set(g.about, MUIA_Text_Contents, (IPTR)text);
    g.quiet--;
    select_maker_of_current();
}

static void new_name(char *out, size_t size, const char *base)
{
    int n = 1, i, taken;

    do
    {
        if (n == 1)
            snprintf(out, size, "%s", base);
        else
            snprintf(out, size, "%.50s %d", base, n);
        taken = 0;
        for (i = 0; i < g.nprofiles; i++)
            if (strcmp(g.profile[i].name, out) == 0)
                taken = 1;
        n++;
    } while (taken);
}

/* ---------------------------------------------------------------- models */

/* Natural order: "PIXMA MP9" before "PIXMA MP10". */
static int natural_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9')
        {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);

            if (x != y)
                return x < y ? -1 : 1;
            continue;
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return tolower((unsigned char)*a) - tolower((unsigned char)*b);
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int model_order(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    int c = strcasecmp(g.model_maker[a], g.model_maker[b]);

    return c != 0 ? c : natural_cmp(g.model_name[a], g.model_name[b]);
}

static int maker_order(const void *x, const void *y)
{
    return strcasecmp((const char *)x, (const char *)y);
}

static void sort_models(void)
{
    int i, j;

    for (i = 0; i < g.nmodels; i++)
        g.sorted[i] = i;
    qsort(g.sorted, g.nmodels, sizeof(int), model_order);
    g.nmakers = 0;
    for (i = 0; i < g.nmodels; i++)
    {
        const char *m = g.model_maker[g.sorted[i]];

        for (j = 0; j < g.nmakers; j++)
            if (strcasecmp(g.maker[j], m) == 0)
                break;
        if (j == g.nmakers)
            snprintf(g.maker[g.nmakers++], sizeof(g.maker[0]), "%s", m);
    }
    qsort(g.maker, g.nmakers, sizeof(g.maker[0]), maker_order);
}

static const char *active_maker(void)
{
    IPTR active = 0;

    get(g.makers, MUIA_List_Active, &active);
    return (LONG)active >= 0 && (LONG)active < g.nmakers ? g.maker[active] : NULL;
}

/* The models of the chosen manufacturer, or of all when searching. */
static void fill_model_list(void)
{
    char *search = NULL;
    const char *maker = active_maker();
    struct gp_profile *p = cur();
    LONG select = MUIV_List_Active_Off;
    int i;

    get(g.search, MUIA_String_Contents, &search);
    g.nfiltered = 0;
    g.quiet++;
    set(g.models, MUIA_List_Quiet, TRUE);
    DoMethod(g.models, MUIM_List_Clear);
    for (i = 0; i < g.nmodels; i++)
    {
        int m = g.sorted[i];

        if (search != NULL && search[0] != '\0')
        {
            if (!contains(g.model_name[m], search) && !contains(g.model_driver[m], search))
                continue;
        }
        else if (maker == NULL || strcasecmp(g.model_maker[m], maker) != 0)
            continue;
        if (p != NULL && strcmp(g.model_driver[m], p->model) == 0)
            select = g.nfiltered;
        g.filtered[g.nfiltered++] = m;
        DoMethod(g.models, MUIM_List_InsertSingle, (IPTR)g.model_name[m], MUIV_List_Insert_Bottom);
    }
    set(g.models, MUIA_List_Quiet, FALSE);
    set(g.models, MUIA_List_Active, select);
    if (select != MUIV_List_Active_Off)
        DoMethod(g.models, MUIM_List_Jump, select);
    g.quiet--;
}

/* Shows the manufacturer of the current profile's model. */
static void select_maker_of_current(void)
{
    struct gp_profile *p = cur();
    const stp_printer_t *pr = p != NULL ? stp_get_printer_by_driver(p->model) : NULL;
    const char *m = pr != NULL ? stp_printer_get_manufacturer(pr) : NULL;
    int i;

    g.quiet++;
    for (i = 0; m != NULL && i < g.nmakers; i++)
        if (strcasecmp(g.maker[i], m) == 0)
        {
            set(g.makers, MUIA_List_Active, i);
            DoMethod(g.makers, MUIM_List_Jump, i);
            break;
        }
    g.quiet--;
    fill_model_list();
}

/* ---------------------------------------------------------------- generated tabs */

static void free_params(void)
{
    int i, j;

    for (i = 0; i < g.nparams; i++)
    {
        struct param_ui *u = &g.params[i];

        if (u->choice_text != NULL)
            for (j = 0; j < u->nchoices; j++)
                free(u->choice_text[j]);
        free(u->choice_text);
        free(u->choice_name);
    }
    g.nparams = 0;
}

/* The control for one parameter, filled with its current value. */
static Object *make_control(stp_vars_t *v, const stp_parameter_t *d, struct param_ui *u,
    int index, Object **extra)
{
    stp_parameter_t desc;
    Object *o = NULL;
    int i;

    *extra = NULL;
    stp_describe_parameter(v, d->name, &desc);
    snprintf(u->name, sizeof(u->name), "%s", d->name);
    u->type = d->p_type;
    switch (d->p_type)
    {
    case STP_PARAMETER_TYPE_STRING_LIST:
    {
        const char *value = stp_get_string_parameter(v, d->name);
        int active = 0;

        u->nchoices = desc.bounds.str != NULL ? stp_string_list_count(desc.bounds.str) : 0;
        if (u->nchoices == 0)
            break;
        u->choice_name = calloc(u->nchoices, sizeof(*u->choice_name));
        u->choice_text = calloc(u->nchoices + 1, sizeof(char *));
        for (i = 0; i < u->nchoices; i++)
        {
            stp_param_string_t *s = stp_string_list_param(desc.bounds.str, i);

            snprintf(u->choice_name[i], sizeof(u->choice_name[i]), "%s", s->name);
            u->choice_text[i] = strdup(s->text);
            if (value == NULL && desc.deflt.str != NULL && strcmp(s->name, desc.deflt.str) == 0)
                active = i;
            if (value != NULL && strcmp(s->name, value) == 0)
                active = i;
        }
        if (u->nchoices <= 12)
        {
            o = CycleObject, MUIA_Cycle_Entries, (IPTR)u->choice_text, MUIA_Cycle_Active, active,
                MUIA_CycleChain, TRUE, End;
            if (o != NULL)
                DoMethod(o, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
                    (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&param_hook, index);
            u->obj = o;
        }
        else
        {
            Object *str = StringObject, StringFrame, MUIA_String_Contents,
                (IPTR)u->choice_text[active], MUIA_CycleChain, TRUE, End;

            o = PoplistObject,
                MUIA_Popstring_String, (IPTR)str,
                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopUp),
                MUIA_Poplist_Array, (IPTR)u->choice_text,
                End;
            if (o != NULL)
                DoMethod(str, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime,
                    (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&param_hook, index);
            u->obj = str;
        }
        break;
    }
    case STP_PARAMETER_TYPE_BOOLEAN:
        o = MUI_MakeObject(MUIO_Checkmark, (IPTR)NULL);
        if (o != NULL)
        {
            set(o, MUIA_Selected, stp_get_boolean_parameter(v, d->name));
            set(o, MUIA_CycleChain, TRUE);
            DoMethod(o, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
                (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&param_hook, index);
        }
        u->obj = o;
        break;
    case STP_PARAMETER_TYPE_DOUBLE:
    case STP_PARAMETER_TYPE_INT:
    {
        double value, lo, hi;
        LONG level;

        if (d->p_type == STP_PARAMETER_TYPE_DOUBLE)
        {
            lo = desc.bounds.dbl.lower;
            hi = desc.bounds.dbl.upper;
            value = stp_get_float_parameter(v, d->name);
        }
        else
        {
            lo = desc.bounds.integer.lower;
            hi = desc.bounds.integer.upper;
            value = stp_get_int_parameter(v, d->name);
        }
        if (hi <= lo)
            break;
        u->lower = lo;
        u->upper = hi;
        u->steps = (LONG)((hi - lo) * 1000 + 0.5);
        if (u->steps < 1)
            u->steps = 1;
        if (u->steps > 1000000)
            u->steps = 1000000;
        level = d->p_type == STP_PARAMETER_TYPE_INT ? (LONG)value :
            (LONG)((value - lo) * u->steps / (hi - lo) + 0.5);
        o = u->obj = NewObject(gpslider_class->mcc_Class, NULL,
            GPA_Slider_Lower, (IPTR)&u->lower, GPA_Slider_Upper, (IPTR)&u->upper,
            GPA_Slider_Int, d->p_type == STP_PARAMETER_TYPE_INT, GPA_Slider_Steps, u->steps,
            MUIA_CycleChain, TRUE,
            MUIA_Numeric_Min, d->p_type == STP_PARAMETER_TYPE_INT ? (LONG)lo : 0,
            MUIA_Numeric_Max, d->p_type == STP_PARAMETER_TYPE_INT ? (LONG)hi : u->steps,
            MUIA_Numeric_Value, level, TAG_END);
        if (o != NULL)
            DoMethod(o, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime,
                (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&param_hook, index);
        break;
    }
    case STP_PARAMETER_TYPE_FILE:
    {
        /* A file the model reads, such as a PostScript printer's PPD. */
        const char *value = stp_get_file_parameter(v, d->name);
        Object *str = StringObject, StringFrame, MUIA_CycleChain, TRUE,
            MUIA_String_Contents, (IPTR)(value != NULL ? value : ""), MUIA_String_MaxLen, 79, End;

        o = PopaslObject,
            MUIA_Popstring_String, (IPTR)str,
            MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopFile),
            ASLFR_TitleText, (IPTR)d->text,
            End;
        if (o != NULL)
            DoMethod(str, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
                (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&param_hook, index);
        u->obj = str;
        break;
    }
    default:
        break;
    }
    stp_parameter_description_destroy(&desc);
    return o;
}

/* Drops options the model does not have or whose value it no longer offers. */
static int drop_invalid_options(struct gp_profile *p, stp_vars_t *v)
{
    int i, dropped = 0;

    for (i = 0; i < p->noptions; i++)
    {
        stp_parameter_t desc;
        int ok;

        stp_describe_parameter(v, p->opt_name[i], &desc);
        ok = desc.p_type != STP_PARAMETER_TYPE_INVALID && desc.is_active;
        if (ok && desc.p_type == STP_PARAMETER_TYPE_STRING_LIST)
        {
            int j, n = desc.bounds.str != NULL ? stp_string_list_count(desc.bounds.str) : 0;

            ok = 0;
            for (j = 0; j < n; j++)
                if (strcmp(stp_string_list_param(desc.bounds.str, j)->name, p->opt_value[i]) == 0)
                    ok = 1;
        }
        stp_parameter_description_destroy(&desc);
        if (!ok)
        {
            gpprofile_set(p, p->opt_name[i], NULL);
            i--;
            dropped++;
        }
    }
    return dropped;
}

static void rebuild_tabs(void)
{
    struct gp_profile *p = cur();
    stp_vars_t *v = profile_vars(p);
    Object *col[GEN_TABS];
    int counts[GEN_TABS] = { 0, 0, 0, 0 };
    IPTR level = 0;
    int t, i, n = 0, dropped = 0, no_page = 0;
    stp_parameter_list_t list = NULL;
    char text[160];

    get(g.level, MUIA_Cycle_Active, &level);
    if (v != NULL)
    {
        dropped = drop_invalid_options(p, v);
        if (dropped)
        {
            stp_vars_destroy(v);
            v = profile_vars(p);
        }
    }
    for (t = 0; t < GEN_TABS; t++)
    {
        DoMethod(g.tab_group[t], MUIM_Group_InitChange);
        if (g.tab_content[t] != NULL)
        {
            DoMethod(g.tab_group[t], OM_REMMEMBER, (IPTR)g.tab_content[t]);
            MUI_DisposeObject(g.tab_content[t]);
        }
        col[t] = ColGroup(2), End;
        g.tab_content[t] = col[t];
    }
    free_params();
    if (v != NULL)
    {
        list = stp_get_parameter_list(v);
        n = stp_parameter_list_count(list);
    }
    for (i = 0; i < n && g.nparams < MAX_PARAMS; i++)
    {
        const stp_parameter_t *d = stp_parameter_list_param(list, i);
        stp_parameter_t desc;
        struct param_ui *u = &g.params[g.nparams];
        Object *ctl, *extra;
        int active;

        if ((int)d->p_level > level_max[level] || d->read_only || in_list(d->name, hidden_params))
            continue;
        stp_describe_parameter(v, d->name, &desc);
        active = desc.is_active;
        stp_parameter_description_destroy(&desc);
        if (!active)
            continue;
        memset(u, 0, sizeof(*u));
        ctl = make_control(v, d, u, g.nparams, &extra);
        if (ctl == NULL)
        {
            free(u->choice_name);
            if (u->choice_text != NULL)
            {
                int j;

                for (j = 0; j < u->nchoices; j++)
                    free(u->choice_text[j]);
                free(u->choice_text);
            }
            continue;
        }
        t = tab_of(d);
        DoMethod(col[t], OM_ADDMEMBER, (IPTR)Label2(d->text));
        DoMethod(col[t], OM_ADDMEMBER, (IPTR)ctl);
        counts[t]++;
        g.nparams++;
    }
    for (t = 0; t < GEN_TABS; t++)
    {
        if (counts[t] == 0)
        {
            DoMethod(col[t], OM_ADDMEMBER, (IPTR)HSpace(0));
            DoMethod(col[t], OM_ADDMEMBER, (IPTR)TextObject, MUIA_Text_Contents,
                (IPTR)(v == NULL ? "Choose a printer model on the Printer page." :
                    "Nothing here at this level; try Advanced or Expert."), End);
        }
        DoMethod(g.tab_group[t], OM_ADDMEMBER, (IPTR)col[t]);
        DoMethod(g.tab_group[t], MUIM_Group_ExitChange);
    }
    if (v != NULL)
    {
        stp_dimension_t pw, ph, l, r, b, t2;
        double geo[6];
        const char *paper = stp_get_string_parameter(v, "PageSize");
        const stp_papersize_t *ps = paper != NULL ? stp_describe_papersize(v, paper) : NULL;

        stp_get_media_size(v, &pw, &ph);
        stp_get_imageable_area(v, &l, &r, &b, &t2);
        geo[0] = pw; geo[1] = ph; geo[2] = l; geo[3] = t2; geo[4] = r - l; geo[5] = b - t2;
        if (r - l < 1 || b - t2 < 1)
        {
            preview_set(g.preview, NULL, NULL);
            no_page = 1;
        }
        else
            preview_set(g.preview, geo, ps != NULL && ps->text != NULL ? ps->text : (paper ? paper : ""));
    }
    else
        preview_set(g.preview, NULL, NULL);
    if (list != NULL)
        stp_parameter_list_destroy(list);
    if (v != NULL)
        stp_vars_destroy(v);
    if (p == NULL)
        snprintf(text, sizeof(text), "Add a printer to start.");
    else if (no_page)
        snprintf(text, sizeof(text), "%s: the model has no page sizes; a PostScript printer needs "
            "its PPD file (Paper page).", p->name);
    else if (dropped)
        snprintf(text, sizeof(text), "%s: %d setting%s the model does not offer were reset.",
            p->name, dropped, dropped == 1 ? "" : "s");
    else
        snprintf(text, sizeof(text), "%s: %d settings shown (%s), %d changed from the defaults.",
            p->name, g.nparams, level_names[level], p->noptions);
    set_status(text);
}

/* ---------------------------------------------------------------- hooks */

AROS_UFH3(IPTR, rebuild_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    (void)hook; (void)obj; (void)msg;
    rebuild_tabs();
    return 0;
    AROS_USERFUNC_EXIT
}

static void push_rebuild(void)
{
    DoMethod(g.app, MUIM_Application_PushMethod, (IPTR)g.app, 2, MUIM_CallHook, (IPTR)&rebuild_hook);
}

AROS_UFH3(IPTR, param_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    struct param_ui *u;
    IPTR value = 0;
    char text[64];
    int i;

    (void)hook; (void)obj;
    if (g.quiet || p == NULL || msg[0] >= (IPTR)g.nparams)
        return 0;
    u = &g.params[msg[0]];
    switch (u->type)
    {
    case STP_PARAMETER_TYPE_STRING_LIST:
        if (u->nchoices <= 12)
        {
            get(u->obj, MUIA_Cycle_Active, &value);
            if (value < (IPTR)u->nchoices)
                gpprofile_set(p, u->name, u->choice_name[value]);
        }
        else
        {
            char *s = NULL;

            get(u->obj, MUIA_String_Contents, &s);
            for (i = 0; s != NULL && i < u->nchoices; i++)
                if (strcmp(u->choice_text[i], s) == 0)
                {
                    gpprofile_set(p, u->name, u->choice_name[i]);
                    break;
                }
            if (s == NULL || i == u->nchoices)
                return 0;
        }
        push_rebuild();
        break;
    case STP_PARAMETER_TYPE_BOOLEAN:
        get(u->obj, MUIA_Selected, &value);
        gpprofile_set(p, u->name, value ? "true" : "false");
        push_rebuild();
        break;
    case STP_PARAMETER_TYPE_INT:
        get(u->obj, MUIA_Numeric_Value, &value);
        snprintf(text, sizeof(text), "%ld", (long)(LONG)value);
        gpprofile_set(p, u->name, text);
        break;
    case STP_PARAMETER_TYPE_DOUBLE:
    {
        double d;

        get(u->obj, MUIA_Numeric_Value, &value);
        d = u->lower + (double)(LONG)value * (u->upper - u->lower) / u->steps;
        snprintf(text, sizeof(text), "%.3f", d);
        gpprofile_set(p, u->name, text);
        break;
    }
    case STP_PARAMETER_TYPE_FILE:
    {
        char *str = NULL;

        get(u->obj, MUIA_String_Contents, &str);
        gpprofile_set(p, u->name, str != NULL && str[0] != '\0' ? str : NULL);
        push_rebuild();
        break;
    }
    default:
        break;
    }
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, profile_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    IPTR active = 0;

    (void)hook; (void)obj; (void)msg;
    if (g.quiet)
        return 0;
    get(g.profiles, MUIA_List_Active, &active);
    if ((LONG)active >= 0 && (LONG)active < g.nprofiles)
    {
        g.current = active;
        show_profile();
        push_rebuild();
    }
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, search_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    (void)hook; (void)obj; (void)msg;
    fill_model_list();
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, maker_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    (void)hook; (void)obj; (void)msg;
    if (!g.quiet)
        fill_model_list();
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, model_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    IPTR active = 0;

    (void)hook; (void)obj; (void)msg;
    if (g.quiet || p == NULL)
        return 0;
    get(g.models, MUIA_List_Active, &active);
    if ((LONG)active < 0 || (LONG)active >= g.nfiltered)
        return 0;
    snprintf(p->model, sizeof(p->model), "%s", g.model_driver[g.filtered[active]]);
    show_profile();
    push_rebuild();
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, name_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    char *s = NULL, old[64];
    int i;

    (void)hook; (void)obj; (void)msg;
    if (g.quiet || p == NULL)
        return 0;
    get(g.name, MUIA_String_Contents, &s);
    if (s == NULL || s[0] == '\0' || strcmp(s, p->name) == 0)
        return 0;
    for (i = 0; i < g.nprofiles; i++)
        if (strcmp(g.profile[i].name, s) == 0)
        {
            set_status("Another printer already has that name.");
            return 0;
        }
    snprintf(old, sizeof(old), "%s", p->name);
    snprintf(p->name, sizeof(p->name), "%s", s);
    for (i = 0; i < GP_UNITS; i++)
        if (strcmp(g.units[i], old) == 0)
            snprintf(g.units[i], sizeof(g.units[i]), "%s", p->name);
    if (g.ndeleted < GP_MAX_PROFILES)
        snprintf(g.deleted[g.ndeleted++], 64, "%s", old);
    fill_profile_list();
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3(IPTR, unit_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    IPTR active = 0;
    int i;

    (void)hook; (void)obj; (void)msg;
    if (g.quiet || p == NULL)
        return 0;
    get(g.unit, MUIA_Cycle_Active, &active);
    for (i = 0; i < GP_UNITS; i++)
        if (strcmp(g.units[i], p->name) == 0)
            g.units[i][0] = '\0';
    if (active > 0)
        snprintf(g.units[active - 1], sizeof(g.units[0]), "%s", p->name);
    fill_profile_list();
    return 0;
    AROS_USERFUNC_EXIT
}

enum { MISC_ADD = 1, MISC_COPY, MISC_REMOVE, MISC_LEVEL, MISC_DEFAULTS, MISC_CONNECTION,
    MISC_FIND, MISC_FOUND_SELECT, MISC_FOUND_USE, MISC_FOUND_CANCEL, MISC_FOUND_STATUS,
    MISC_STATUS, MISC_STATUS_REFRESH, MISC_STATUS_CLOSE, MISC_PREVIEW };

/* ---------------------------------------------------------------- network printers */

/* group gets a bar in its own colour for each of f's supplies. */
static void fill_supplies(Object *group, const struct gp_found *f)
{
    int k;

    DoMethod(group, MUIM_Group_InitChange);
    for (;;)
    {
        struct List *kids = NULL;
        Object *o;
        APTR state;

        get(group, MUIA_Group_ChildList, &kids);
        state = kids != NULL ? (APTR)kids->lh_Head : NULL;
        o = state != NULL ? NextObject((Object **)&state) : NULL;
        if (o == NULL)
            break;
        DoMethod(group, OM_REMMEMBER, (IPTR)o);
        MUI_DisposeObject(o);
    }
    for (k = 0; f != NULL && k < f->nsupplies; k++)
    {
        Object *o = supply_bar(f->supply[k], f->level[k], f->colour[k]);

        if (o != NULL)
            DoMethod(group, OM_ADDMEMBER, (IPTR)o);
    }
    if (f == NULL || f->nsupplies == 0)
        DoMethod(group, OM_ADDMEMBER, (IPTR)(TextObject, MUIA_Text_Contents,
            (IPTR)(f == NULL ? "" : "No supply levels reported."), End));
    DoMethod(group, MUIM_Group_ExitChange);
}

/* The chosen printer's details and a bar for each supply. */
static void show_found(int i)
{
    struct gp_found *f = i >= 0 && i < g.nfound ? &g.found[i] : NULL;
    const char *paper = f != NULL ? gpfind_page_size(f->paper) : NULL;
    char text[700];

    if (f == NULL)
        return;
    snprintf(text, sizeof(text), "\33b%s\33n\n%s\n%s\nState: %s    Paper loaded: %s",
        f->model[0] ? f->model : f->name, f->name, f->uri, f->state[0] ? f->state : "-",
        paper != NULL ? paper : f->paper[0] ? f->paper : "-");
    set(g.finfo, MUIA_Text_Contents, (IPTR)text);
    set_picture(g.fpic_group, &g.fpic, f->picture);

    fill_supplies(g.fgauges, f);
}

/* The Gutenprint model for a printer: the one Gutenprint names like the printer names
   itself, else PostScript Level 2 or generic PCL 5 from the formats it takes. */
static const char *model_for(const struct gp_found *f)
{
    int i;

    for (i = 0; i < g.nmodels && f->model[0]; i++)
        if (strcasecmp(g.model_name[i], f->model) == 0)
            return g.model_driver[i];
    if (strstr(f->formats, "application/postscript") != NULL)
        return "ps2";
    if (strstr(f->formats, "vnd.hp-PCL") != NULL)
        return contains(f->model, "colo") ? "pcl-g_5c" : "pcl-g_5";
    return NULL;
}

/* Fills a printer's connection, picture, model and paper in from a network printer
   found; returns the Gutenprint model chosen, NULL when there is none. */
static const char *adopt_found(struct gp_profile *p, const struct gp_found *f)
{
    const char *driver, *paper;

    snprintf(p->connection, sizeof(p->connection), "%s", f->uri);
    snprintf(p->device_id, sizeof(p->device_id), "MDL:%s;CMD:%s;", f->model, f->formats);
    snprintf(p->picture, sizeof(p->picture), "%s", f->picture);
    driver = model_for(f);
    if (driver != NULL)
    {
        snprintf(p->model, sizeof(p->model), "%s", driver);
        if (strcmp(driver, "ps2") == 0)
            gpprofile_set(p, "PPDFile", "GUTENPRINT:ppd/Generic.ppd");
    }
    paper = gpfind_page_size(f->paper);
    if (paper != NULL)
        gpprofile_set(p, "PageSize", paper);
    return driver;
}

/* Sets the current printer (or a new one) up for the chosen network printer. */
static void use_found(int i)
{
    struct gp_found *f = i >= 0 && i < g.nfound ? &g.found[i] : NULL;
    struct gp_profile *p;
    const char *driver;
    char name[64], text[300];
    int u, k;

    if (f == NULL)
        return;
    if (cur() == NULL)
    {
        if (g.nprofiles == GP_MAX_PROFILES)
            return;
        memset(&g.profile[g.nprofiles], 0, sizeof(g.profile[0]));
        snprintf(g.profile[g.nprofiles].name, sizeof(g.profile[0].name), "Printer");
        g.current = g.nprofiles++;
    }
    p = cur();
    /* A printer still called by its default name takes the network printer's name. */
    if (strncmp(p->name, "Printer", 7) == 0)
    {
        snprintf(name, sizeof(name), "%s", f->model[0] ? f->model : f->name);
        for (k = 0; k < GP_UNITS; k++)
            if (strcmp(g.units[k], p->name) == 0)
                snprintf(g.units[k], sizeof(g.units[k]), "%s", name);
        snprintf(p->name, sizeof(p->name), "%s", name);
    }
    driver = adopt_found(p, f);
    u = unit_of(p->name);
    for (k = 0; u < 0 && k < GP_UNITS; k++)
        if (g.units[k][0] == '\0')
        {
            snprintf(g.units[k], sizeof(g.units[k]), "%s", p->name);
            u = k;
        }
    fill_profile_list();
    show_profile();
    push_rebuild();
    set(g.fwin, MUIA_Window_Open, FALSE);
    if (driver == NULL)
        snprintf(text, sizeof(text), "%s: choose its model; it takes none of PostScript and PCL.", p->name);
    else if (u < 0)
        snprintf(text, sizeof(text), "%s: bind it to a printer.device unit, then Use or Save.", p->name);
    else
        snprintf(text, sizeof(text), "%s on unit %d through the network: Use or Save to print to it.", p->name, u);
    set_status(text);
}

/* The Status window: what the printer at status_uri says about itself now, how the
   spooler and the last job are doing. */
static void refresh_status(void)
{
    static struct gp_found f;
    char why[160], reasons[200], text[900], queue[200] = "", last[200] = "";
    const char *paper;
    int answered;

    set(g.app, MUIA_Application_Sleep, TRUE);
    answered = gpfind_query(g.status_uri, &f, why, sizeof(why));
    set(g.app, MUIA_Application_Sleep, FALSE);
    if (answered < 0)
    {
        set(g.sinfo, MUIA_Text_Contents, (IPTR)why);
        return;
    }
    if (answered == 0)
    {
        snprintf(text, sizeof(text), "\33b%s\33n\n%s\n\nThe printer does not answer: it may be off, "
            "asleep or no longer at this address.", g.status_name, g.status_uri);
        set(g.sinfo, MUIA_Text_Contents, (IPTR)text);
        fill_supplies(g.ssupplies, NULL);
    }
    else
    {
        gpfind_reasons_text(f.reasons, reasons, sizeof(reasons));
        paper = gpfind_page_size(f.paper);
        snprintf(text, sizeof(text), "\33b%s\33n\n%s\nState: %s%s%s\nPaper loaded: %s%s%s",
            f.model[0] ? f.model : g.status_name, g.status_uri,
            strcmp(f.state, "idle") == 0 ? "ready" : f.state,
            reasons[0] ? "  -  " : "", reasons,
            paper != NULL ? paper : f.paper[0] ? f.paper : "-",
            f.message[0] ? "\n" : "", f.message);
        set(g.sinfo, MUIA_Text_Contents, (IPTR)text);
        if (f.picture[0])
            set_picture(g.spic_group, &g.spic, f.picture);
        fill_supplies(g.ssupplies, &f);
    }
    GetVar((STRPTR)"PrintSpool/Status", (STRPTR)queue, sizeof(queue), GVF_GLOBAL_ONLY);
    GetVar((STRPTR)"IPP/Status", (STRPTR)last, sizeof(last), GVF_GLOBAL_ONLY);
    snprintf(text, sizeof(text), "Print queue: %s\nLast job: %s",
        queue[0] ? queue : "-", last[0] ? last : "-");
    set(g.squeue, MUIA_Text_Contents, (IPTR)text);
}

static void open_status(const char *uri, const char *name, const char *picture)
{
    char title[160];

    if (uri == NULL || strncmp(uri, "ipp://", 6) != 0)
    {
        set_status("Status is for printers on the network (an ipp:// connection).");
        return;
    }
    snprintf(g.status_uri, sizeof(g.status_uri), "%s", uri);
    snprintf(g.status_name, sizeof(g.status_name), "%s", name);
    snprintf(title, sizeof(title), "Status: %s", name);
    set(g.swin, MUIA_Window_Title, (IPTR)strdup(title));
    set_picture(g.spic_group, &g.spic, picture);
    set(g.swin, MUIA_Window_Open, TRUE);
    refresh_status();
}

static void find_printers(void)
{
    char why[160];
    int i;

    set_status("Looking for printers on the network...");
    set(g.app, MUIA_Application_Sleep, TRUE);
    g.nfound = gpfind_scan(g.found, GPFIND_MAX, why, sizeof(why));
    set(g.app, MUIA_Application_Sleep, FALSE);
    if (g.nfound < 0)
    {
        set_status(why);
        return;
    }
    if (g.nfound == 0)
    {
        set_status("No printers found on the network.");
        return;
    }
    DoMethod(g.flist, MUIM_List_Clear);
    for (i = 0; i < g.nfound; i++)
        DoMethod(g.flist, MUIM_List_InsertSingle,
            (IPTR)(g.found[i].model[0] ? g.found[i].model : g.found[i].name), MUIV_List_Insert_Bottom);
    set(g.flist, MUIA_List_Active, 0);
    show_found(0);
    set_status(g.nfound == 1 ? "1 printer found." : "Printers found.");
    set(g.fwin, MUIA_Window_Open, TRUE);
}

AROS_UFH3(IPTR, misc_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2), AROS_UFHA(IPTR *, msg, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    int i;

    (void)hook; (void)obj;
    switch (msg[0])
    {
    case MISC_ADD:
    case MISC_COPY:
        if (g.nprofiles == GP_MAX_PROFILES || (msg[0] == MISC_COPY && p == NULL))
            break;
        if (msg[0] == MISC_COPY)
            g.profile[g.nprofiles] = *p;
        else
            memset(&g.profile[g.nprofiles], 0, sizeof(g.profile[0]));
        new_name(g.profile[g.nprofiles].name, sizeof(g.profile[0].name),
            msg[0] == MISC_COPY ? p->name : "Printer");
        g.current = g.nprofiles++;
        fill_profile_list();
        show_profile();
        push_rebuild();
        break;
    case MISC_REMOVE:
        if (p == NULL)
            break;
        for (i = 0; i < GP_UNITS; i++)
            if (strcmp(g.units[i], p->name) == 0)
                g.units[i][0] = '\0';
        if (g.ndeleted < GP_MAX_PROFILES)
            snprintf(g.deleted[g.ndeleted++], 64, "%s", p->name);
        memmove(&g.profile[g.current], &g.profile[g.current + 1],
            (g.nprofiles - g.current - 1) * sizeof(g.profile[0]));
        g.nprofiles--;
        if (g.current >= g.nprofiles)
            g.current = g.nprofiles - 1;
        fill_profile_list();
        show_profile();
        push_rebuild();
        break;
    case MISC_LEVEL:
        if (!g.quiet)
            push_rebuild();
        break;
    case MISC_DEFAULTS:
        if (p != NULL)
        {
            p->noptions = 0;
            push_rebuild();
        }
        break;
    case MISC_FIND:
        find_printers();
        break;
    case MISC_FOUND_SELECT:
    {
        IPTR a = 0;

        get(g.flist, MUIA_List_Active, &a);
        show_found((int)a);
        break;
    }
    case MISC_FOUND_USE:
    {
        IPTR a = 0;

        get(g.flist, MUIA_List_Active, &a);
        use_found((int)a);
        break;
    }
    case MISC_FOUND_CANCEL:
        set(g.fwin, MUIA_Window_Open, FALSE);
        break;
    case MISC_FOUND_STATUS:
    {
        IPTR a = 0;

        get(g.flist, MUIA_List_Active, &a);
        if ((int)a >= 0 && (int)a < g.nfound)
            open_status(g.found[a].uri, g.found[a].model[0] ? g.found[a].model : g.found[a].name,
                g.found[a].picture);
        break;
    }
    case MISC_STATUS:
        if (p != NULL)
            open_status(p->connection, p->name, p->picture);
        break;
    case MISC_STATUS_REFRESH:
        refresh_status();
        break;
    case MISC_STATUS_CLOSE:
        set(g.swin, MUIA_Window_Open, FALSE);
        break;
    case MISC_CONNECTION:
        if (p != NULL && !g.quiet)
        {
            char *s = NULL;

            get(g.connection, MUIA_String_Contents, &s);
            snprintf(p->connection, sizeof(p->connection), "%s", s != NULL ? s : "");
        }
        break;
    case MISC_PREVIEW:
        if (p != NULL && !g.quiet)
        {
            IPTR on = 0;

            get(g.preview_check, MUIA_Selected, &on);
            p->preview = on != 0;
        }
        break;
    }
    return 0;
    AROS_USERFUNC_EXIT
}

/* ---------------------------------------------------------------- ARexx */

/* Commands on the GUTENPRINTPREFS.<n> port, so the editor can be scripted:
   SELECT <printer>, MODEL <driver>, SET <option> <value>, PAGE <tab>,
   TEST (test page), FIND (network printers), CHOOSE <n> (the n-th one found,
   from 1), STATUS (of the current printer), USE, SAVE, QUIT. */
enum { RX_QUIT, RX_SELECT, RX_MODEL, RX_SET, RX_PAGE, RX_USE, RX_SAVE, RX_TEST, RX_FIND, RX_CHOOSE, RX_STATUS, RX_COUNT };

AROS_UFH3(IPTR, rexx_func, AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, app, A2), AROS_UFHA(IPTR *, args, A1))
{
    AROS_USERFUNC_INIT
    struct gp_profile *p = cur();
    int i;

    /* Zune calls the hook with empty arguments when they do not match the template. */
    switch ((IPTR)hook->h_Data)
    {
    case RX_SELECT:
    case RX_MODEL:
    case RX_PAGE:
    case RX_CHOOSE:
        if (args == NULL || args[0] == 0)
            return 10;
        break;
    case RX_SET:
        if (args == NULL || args[0] == 0 || args[1] == 0)
            return 10;
        break;
    }
    switch ((IPTR)hook->h_Data)
    {
    case RX_QUIT:
        DoMethod(app, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
        return 0;
    case RX_USE:
        DoMethod(app, MUIM_Application_ReturnID, 2);
        return 0;
    case RX_SAVE:
        DoMethod(app, MUIM_Application_ReturnID, 1);
        return 0;
    case RX_TEST:
        DoMethod(app, MUIM_Application_ReturnID, 3);
        return 0;
    case RX_FIND:
        find_printers();
        return g.nfound > 0 ? 0 : 5;
    case RX_STATUS:
        if (p == NULL || strncmp(p->connection, "ipp://", 6) != 0)
            return 10;
        open_status(p->connection, p->name, p->picture);
        return 0;
    case RX_CHOOSE:
        i = atoi((const char *)args[0]);
        if (i < 1 || i > g.nfound)
            return 10;
        set(g.flist, MUIA_List_Active, i - 1);
        use_found(i - 1);
        return 0;
    case RX_SELECT:
        for (i = 0; i < g.nprofiles; i++)
            if (strcmp(g.profile[i].name, (const char *)args[0]) == 0)
            {
                g.current = i;
                fill_profile_list();
                show_profile();
                push_rebuild();
                return 0;
            }
        return 10;
    case RX_MODEL:
        if (p == NULL || stp_get_printer_by_driver((const char *)args[0]) == NULL)
            return 10;
        snprintf(p->model, sizeof(p->model), "%s", (const char *)args[0]);
        show_profile();
        push_rebuild();
        return 0;
    case RX_SET:
        if (p == NULL || gpprofile_set(p, (const char *)args[0], (const char *)args[1]) != 0)
            return 10;
        push_rebuild();
        return 0;
    case RX_PAGE:
        for (i = 0; tab_titles[i] != NULL; i++)
            if (strcasecmp(tab_titles[i], (const char *)args[0]) == 0)
            {
                set(g.pages, MUIA_Group_ActivePage, i);
                return 0;
            }
        return 10;
    }
    return 20;
    AROS_USERFUNC_EXIT
}

static struct Hook rexx_hooks[RX_COUNT];
static struct MUI_Command rexx_commands[] = {
    { "QUIT", NULL, 0, &rexx_hooks[RX_QUIT], { 0 } },
    { "SELECT", "PRINTER/A/F", 1, &rexx_hooks[RX_SELECT], { 0 } },
    { "MODEL", "DRIVER/A", 1, &rexx_hooks[RX_MODEL], { 0 } },
    { "SET", "NAME/A,VALUE/A/F", 2, &rexx_hooks[RX_SET], { 0 } },
    { "PAGE", "NAME/A", 1, &rexx_hooks[RX_PAGE], { 0 } },
    { "USE", NULL, 0, &rexx_hooks[RX_USE], { 0 } },
    { "SAVE", NULL, 0, &rexx_hooks[RX_SAVE], { 0 } },
    { "TEST", NULL, 0, &rexx_hooks[RX_TEST], { 0 } },
    { "FIND", NULL, 0, &rexx_hooks[RX_FIND], { 0 } },
    { "CHOOSE", "NUMBER/A", 1, &rexx_hooks[RX_CHOOSE], { 0 } },
    { "STATUS", NULL, 0, &rexx_hooks[RX_STATUS], { 0 } },
    { NULL, NULL, 0, NULL, { 0 } }
};

/* ---------------------------------------------------------------- load / save */

static void load_all(void)
{
    char names[GP_MAX_PROFILES][64];
    int i, n = gpprofile_list(GP_ENV_DIR, names, GP_MAX_PROFILES);
    char why[160];

    gpunits_load(GP_ENV_DIR, g.units);
    for (i = 0; i < n; i++)
        if (gpprofile_load(GP_ENV_DIR, names[i], &g.profile[g.nprofiles]) == 0)
            g.nprofiles++;
    if (g.nprofiles == 0 && gpprofile_for_unit(0, &g.profile[0], why, sizeof(why)) == 0)
    {
        /* Settings from before profiles: they become the printer on unit 0. */
        snprintf(g.profile[0].name, sizeof(g.profile[0].name), "Printer");
        snprintf(g.units[0], sizeof(g.units[0]), "Printer");
        g.nprofiles = 1;
    }
    g.current = g.nprofiles > 0 ? 0 : -1;
}

static int save_to(const char *dir)
{
    int i;

    for (i = 0; i < g.ndeleted; i++)
        gpprofile_delete(dir, g.deleted[i]);
    for (i = 0; i < g.nprofiles; i++)
        if (gpprofile_save(dir, &g.profile[i]) != 0)
            return 1;
    return gpunits_save(dir, g.units);
}

/* ---------------------------------------------------------------- test page */

#define TEST_W 1200
#define TEST_H 800

/* Colour and grey ramps, primaries and the settings, through printer.device
   on the unit of the current printer, as an application would print. */
static int print_test_page(char *why, size_t why_size)
{
    struct gp_profile *p = cur();
    int unit = p != NULL ? unit_of(p->name) : -1;
    stp_vars_t *v = profile_vars(p);
    struct BitMap *bm;
    struct RastPort rp;
    struct MsgPort *port;
    struct IODRPReq *io;
    UBYTE *row;
    stp_dimension_t l, r, b, t;
    LONG x, y, err = -1;
    char text[160];

    if (p == NULL || v == NULL)
    {
        snprintf(why, why_size, "Choose a model first.");
        return 1;
    }
    if (unit < 0)
    {
        stp_vars_destroy(v);
        snprintf(why, why_size, "Bind the printer to a printer.device unit first.");
        return 1;
    }
    stp_get_imageable_area(v, &l, &r, &b, &t);
    stp_vars_destroy(v);
    bm = AllocBitMap(TEST_W, TEST_H, 24, BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_RGB24), NULL);
    row = malloc(TEST_W * 3);
    if (bm == NULL || row == NULL)
    {
        FreeBitMap(bm);
        free(row);
        snprintf(why, why_size, "Not enough memory for the test page.");
        return 1;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;
    for (y = 0; y < TEST_H; y++)
    {
        for (x = 0; x < TEST_W; x++)
        {
            UBYTE *px = row + x * 3;
            LONG band = y * 6 / TEST_H, v8 = x * 255 / (TEST_W - 1);

            switch (band)
            {
            case 0: px[0] = v8; px[1] = 0; px[2] = 0; break;         /* red ramp */
            case 1: px[0] = 0; px[1] = v8; px[2] = 0; break;         /* green ramp */
            case 2: px[0] = 0; px[1] = 0; px[2] = v8; break;         /* blue ramp */
            case 3: px[0] = px[1] = px[2] = v8; break;               /* grey ramp */
            case 4:                                                   /* hue sweep */
            {
                LONG h6 = x * 6 * 256 / TEST_W, f = h6 & 255, q = 255 - f;
                UBYTE c[6][3] = { {255, f, 0}, {q, 255, 0}, {0, 255, f}, {0, q, 255},
                    {f, 0, 255}, {255, 0, q} };
                memcpy(px, c[(h6 >> 8) % 6], 3);
                break;
            }
            default: px[0] = px[1] = px[2] = 255; break;
            }
        }
        WritePixelArray(row, 0, 0, TEST_W * 3, &rp, 0, y, TEST_W, 1, RECTFMT_RGB);
    }
    SetRPAttrs(&rp, RPTAG_FgColor, 0xFF000000, RPTAG_DrMd, JAM1, TAG_END);
    snprintf(text, sizeof(text), "Gutenprint test page - %s - %s", p->name, model_long_name(p->model));
    Move(&rp, 20, TEST_H * 5 / 6 + 30);
    Text(&rp, (CONST_STRPTR)text, strlen(text));
    snprintf(text, sizeof(text), "%d settings changed from the defaults; printer.device unit %d",
        p->noptions, unit);
    Move(&rp, 20, TEST_H * 5 / 6 + 60);
    Text(&rp, (CONST_STRPTR)text, strlen(text));

    port = CreateMsgPort();
    io = (struct IODRPReq *)CreateIORequest(port, sizeof(*io));
    if (io != NULL && OpenDevice((CONST_STRPTR)"printer.device", unit, (struct IORequest *)io, 0) == 0)
    {
        /* The width of the printable area, the picture's proportions, centred. */
        LONG cols = (LONG)((r - l) * 1000 / 72), rows = cols * TEST_H / TEST_W;
#ifdef PRD_SETJOBINFO
        char title[96];
        struct TagItem info[] = { { PJIA_Document, (IPTR)title }, { TAG_END, 0 } };

        snprintf(title, sizeof(title), "Test page - %s", p->name);
        io->io_Command = PRD_SETJOBINFO;
        ((struct IOStdReq *)io)->io_Data = info;
        DoIO((struct IORequest *)io);
#endif

        io->io_Command = PRD_DUMPRPORT;
        io->io_RastPort = &rp;
        io->io_ColorMap = NULL;
        io->io_Modes = INVALID_ID;
        io->io_SrcX = io->io_SrcY = 0;
        io->io_SrcWidth = TEST_W;
        io->io_SrcHeight = TEST_H;
        io->io_DestCols = cols;
        io->io_DestRows = rows;
        io->io_Special = SPECIAL_MILCOLS | SPECIAL_MILROWS | SPECIAL_CENTER;
        DoIO((struct IORequest *)io);
        err = io->io_Error;
        CloseDevice((struct IORequest *)io);
        if (err != 0)
            snprintf(why, why_size, "printer.device unit %d reported error %ld.", unit, (long)err);
    }
    else
        snprintf(why, why_size, "Cannot open printer.device unit %d.", unit);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    FreeBitMap(bm);
    free(row);
    printf("GutenprintPrefs: test page on unit %d, io_Error=%ld\n", unit, (long)err);
    return err == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------- window */

static Object *tab_page(int t)
{
    g.tab_group[t] = VGroup, End;
    return ScrollgroupObject,
        MUIA_Scrollgroup_FreeHoriz, FALSE,
        MUIA_Scrollgroup_Contents, (IPTR)(VirtgroupObject,
            Child, (IPTR)g.tab_group[t],
            Child, (IPTR)VSpace(0),
            End),
        End;
}

static Object *build_window(void)
{
    return WindowObject,
        MUIA_Window_Title, (IPTR)"Gutenprint",
        MUIA_Window_ID, MAKE_ID('G', 'P', 'R', 'F'),
        MUIA_Window_Width, MUIV_Window_Width_Screen(55),
        MUIA_Window_Height, MUIV_Window_Height_Screen(70),
        WindowContents, (IPTR)(VGroup,
            Child, (IPTR)(HGroup,
                Child, (IPTR)(VGroup, MUIA_Weight, 30,
                    Child, (IPTR)(ListviewObject, MUIA_Listview_List, (IPTR)(g.profiles = ListObject,
                        InputListFrame,
                        MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                        MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                        End), MUIA_CycleChain, TRUE, End),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)(g.add = SimpleButton("_Add")),
                        Child, (IPTR)(g.copy = SimpleButton("_Copy")),
                        Child, (IPTR)(g.remove = SimpleButton("_Remove")),
                        End),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)Label1("Show"),
                        Child, (IPTR)(g.level = CycleObject, MUIA_Cycle_Entries, (IPTR)level_names,
                            MUIA_CycleChain, TRUE, End),
                        End),
                    End),
                Child, (IPTR)(g.pages = RegisterGroup((IPTR)tab_titles), MUIA_Weight, 70,
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(g.pic_group = VGroup, MUIA_Weight, 0, End),
                            Child, (IPTR)(VGroup,
                                Child, (IPTR)(g.about = TextObject, MUIA_Text_SetVMax, FALSE, MUIA_Text_SetMin, FALSE, End),
                                Child, (IPTR)HVSpace,
                                Child, (IPTR)(HGroup,
                                    Child, (IPTR)HSpace(0),
                                    Child, (IPTR)(g.status_btn = SimpleButton("S_tatus")),
                                    Child, (IPTR)(g.find = SimpleButton("_Find network printers...")),
                                    End),
                                End),
                            End),
                        Child, (IPTR)(ColGroup(2),
                            Child, (IPTR)Label2("Name"),
                            Child, (IPTR)(g.name = StringObject, StringFrame, MUIA_CycleChain, TRUE,
                                MUIA_String_MaxLen, 63, End),
                            Child, (IPTR)Label1("printer.device"),
                            Child, (IPTR)(g.unit = CycleObject, MUIA_Cycle_Entries, (IPTR)unit_names,
                                MUIA_CycleChain, TRUE, End),
                            Child, (IPTR)Label2("Model"),
                            Child, (IPTR)(g.model_text = TextObject, TextFrame, End),
                            Child, (IPTR)Label2("Find model"),
                            Child, (IPTR)(g.search = StringObject, StringFrame, MUIA_CycleChain, TRUE, End),
                            End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(ListviewObject, MUIA_Weight, 35,
                                MUIA_Listview_List, (IPTR)(g.makers = ListObject,
                                InputListFrame,
                                MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                                MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                                End), MUIA_CycleChain, TRUE, End),
                            Child, (IPTR)(ListviewObject, MUIA_Weight, 65,
                                MUIA_Listview_List, (IPTR)(g.models = ListObject,
                                InputListFrame,
                                MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                                MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                                End), MUIA_CycleChain, TRUE, End),
                            End),
                        Child, (IPTR)(ColGroup(2),
                            Child, (IPTR)Label2("Connection"),
                            Child, (IPTR)(g.connection = StringObject, StringFrame, MUIA_CycleChain, TRUE,
                                MUIA_String_MaxLen, 255, End),
                            Child, (IPTR)Label2("Device ID"),
                            Child, (IPTR)(g.device_id = TextObject, TextFrame, MUIA_Text_SetMin, FALSE, End),
                            Child, (IPTR)Label1("Preview"),
                            Child, (IPTR)(HGroup,
                                Child, (IPTR)(g.preview_check = MUI_MakeObject(MUIO_Checkmark, (IPTR)NULL)),
                                Child, (IPTR)LLabel1("Show each page before it prints"),
                                Child, (IPTR)HSpace(0),
                                End),
                            End),
                        End),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)tab_page(TAB_PAPER),
                        Child, (IPTR)(g.preview = NewObject(gppreview_class->mcc_Class, NULL,
                            MUIA_Frame, MUIV_Frame_Virtual, MUIA_Weight, 60, TAG_END)),
                        End),
                    Child, (IPTR)tab_page(TAB_QUALITY),
                    Child, (IPTR)tab_page(TAB_COLOUR),
                    Child, (IPTR)tab_page(TAB_ADVANCED),
                    End),
                End),
            Child, (IPTR)(g.status = TextObject, TextFrame, MUIA_Background, MUII_TextBack, End),
            Child, (IPTR)(HGroup,
                Child, (IPTR)(g.save = SimpleButton("_Save")),
                Child, (IPTR)(g.use = SimpleButton("_Use")),
                Child, (IPTR)(g.defaults = SimpleButton("_Defaults")),
                Child, (IPTR)(g.test = SimpleButton("_Test page")),
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(g.cancel = SimpleButton("Cancel")),
                End),
            End),
        End;
}

/* The printers found on the network: pick one, see its picture and supplies, use it. */
static Object *build_find_window(void)
{
    return WindowObject,
        MUIA_Window_Title, (IPTR)"Network printers",
        MUIA_Window_ID, MAKE_ID('G', 'P', 'F', 'N'),
        WindowContents, (IPTR)(VGroup,
            Child, (IPTR)(HGroup,
                Child, (IPTR)(ListviewObject, MUIA_Weight, 30,
                    MUIA_Listview_List, (IPTR)(g.flist = ListObject,
                    MUIA_List_MinLineHeight, 0, MUIA_List_AdjustWidth, TRUE,
                    InputListFrame,
                    MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                    MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                    End), MUIA_CycleChain, TRUE, End),
                Child, (IPTR)(VGroup, MUIA_Weight, 60,
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)(g.fpic_group = VGroup, MUIA_Weight, 0, End),
                        Child, (IPTR)(g.finfo = TextObject, TextFrame, MUIA_Background, MUII_TextBack, End),
                        End),
                    Child, (IPTR)(g.fgauges = VGroup, GroupFrameT("Supplies"), End),
                    End),
                End),
            Child, (IPTR)(HGroup,
                Child, (IPTR)(g.fuse = SimpleButton("_Use this printer")),
                Child, (IPTR)(g.fstatus = SimpleButton("_Status")),
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(g.fcancel = SimpleButton("_Cancel")),
                End),
            End),
        End;
}

/* A printer's status: its picture, state, paper and supplies, the queue and the last job. */
static Object *build_status_window(void)
{
    return WindowObject,
        MUIA_Window_Title, (IPTR)"Status",
        MUIA_Window_ID, MAKE_ID('G', 'P', 'S', 'T'),
        WindowContents, (IPTR)(VGroup,
            Child, (IPTR)(HGroup,
                Child, (IPTR)(g.spic_group = VGroup, MUIA_Weight, 0, End),
                Child, (IPTR)(g.sinfo = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                    MUIA_Text_Contents, (IPTR)"\n\n\n\n", End),
                End),
            Child, (IPTR)(g.ssupplies = VGroup, GroupFrameT("Supplies"), End),
            Child, (IPTR)(g.squeue = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                MUIA_Text_Contents, (IPTR)"Print queue: -\nLast job: -", End),
            Child, (IPTR)(HGroup,
                Child, (IPTR)(g.srefresh = SimpleButton("_Refresh")),
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(g.sclose = SimpleButton("_Close")),
                End),
            End),
        End;
}

static void notify_misc(Object *o, ULONG attr, IPTR value, IPTR what)
{
    DoMethod(o, MUIM_Notify, attr, value, (IPTR)g.app, 3, MUIM_CallHook, (IPTR)&misc_hook, what);
}

static void init_hook(struct Hook *h, APTR func)
{
    h->h_Entry = (HOOKFUNC)func;
    h->h_SubEntry = NULL;
    h->h_Data = NULL;
}

/* SETUP: sets a network printer up without opening a window, for PrinterWatch.
   0 on success, 5 when Gutenprint has no model for it, 10 on failure. */
static int setup_headless(const char *uri, const char *formats, int unit, int save)
{
    static struct gp_found f;
    struct gp_profile *p = NULL;
    const char *driver, *format;
    char why[200];
    int i, k;

    memset(&f, 0, sizeof(f));
    snprintf(f.uri, sizeof(f.uri), "%s", uri);
    if (gpfind_query(uri, &f, why, sizeof(why)) < 0)
    {
        printf("GutenprintPrefs: %s\n", why);
        return 10;
    }
    if (formats != NULL)
        snprintf(f.formats, sizeof(f.formats), "%s", formats);
    if (f.name[0] == '\0')
        snprintf(f.name, sizeof(f.name), "%s", f.model[0] ? f.model : "Network printer");

    /* The printer already set up for this connection, or a new one named after it. */
    for (i = 0; i < g.nprofiles; i++)
        if (strcmp(g.profile[i].connection, uri) == 0)
            p = &g.profile[i];
    if (p == NULL)
    {
        if (g.nprofiles == GP_MAX_PROFILES)
        {
            printf("GutenprintPrefs: no room for another printer\n");
            return 10;
        }
        p = &g.profile[g.nprofiles++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%.63s", f.model[0] ? f.model : f.name);
        for (i = 0; i < g.nprofiles - 1; i++)
            if (strcmp(g.profile[i].name, p->name) == 0)
            {
                size_t n = strlen(p->name);
                snprintf(p->name + (n > 56 ? 56 : n), sizeof(p->name) - (n > 56 ? 56 : n), " %d", g.nprofiles);
                break;
            }
    }
    if ((driver = adopt_found(p, &f)) == NULL)
    {
        printf("GutenprintPrefs: no Gutenprint model for %s; it takes none of PostScript and PCL\n", p->name);
        return 5;
    }

    for (k = 0; k < GP_UNITS; k++)
        if (strcmp(g.units[k], p->name) == 0)
            g.units[k][0] = '\0';
    if (unit < 0 || unit >= GP_UNITS)
        for (unit = 0; unit < GP_UNITS && g.units[unit][0]; unit++)
            ;
    if (unit >= GP_UNITS)
    {
        printf("GutenprintPrefs: no free printer unit\n");
        return 10;
    }
    snprintf(g.units[unit], sizeof(g.units[unit]), "%s", p->name);

    if (save_to(GP_ENV_DIR) != 0)
    {
        printf("GutenprintPrefs: cannot write the settings\n");
        return 10;
    }
    if (save)
        save_to(GP_ENVARC_DIR);     /* fails harmlessly on a read-only disk */
    format = strncmp(driver, "ps", 2) == 0 ? "application/postscript" : "application/octet-stream";
    if (gpfind_setup_unit(unit, uri, p->name, format, save, why, sizeof(why)) != 0
        && gpfind_setup_unit(unit, uri, p->name, format, 0, why, sizeof(why)) != 0)
    {
        printf("GutenprintPrefs: %s\n", why);
        return 10;
    }
    printf("GutenprintPrefs: %s is printer %d (%s)\n", p->name, unit, driver);
    return 0;
}

int main(void)
{
    SIPTR args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"UNIT/N,PAGE/K,SETUP/K,FORMATS/K,SAVE/S", args, NULL);
    int unit = rda != NULL && args[0] != 0 ? (int)*(LONG *)args[0] : -1;
    char setup_uri[256] = "", setup_formats[1300] = "";
    int setup_save = rda != NULL && args[4] != 0;
    int page = 0;
    struct DateStamp t0, t1, t2;
    ULONG sigs = 0;
    BOOL running = TRUE;
    int rc = 0, i;

    if (rda != NULL && args[1] != 0)
        for (i = 0; tab_titles[i] != NULL; i++)
            if (strcasecmp(tab_titles[i], (const char *)args[1]) == 0)
                page = i;
    if (rda != NULL && args[2] != 0)
        snprintf(setup_uri, sizeof(setup_uri), "%s", (const char *)args[2]);
    if (rda != NULL && args[3] != 0)
        snprintf(setup_formats, sizeof(setup_formats), "%s", (const char *)args[3]);
    if (rda != NULL)
        FreeArgs(rda);
    DateStamp(&t0);
    if (getenv("STP_DATA_PATH") == NULL)
        SetVar((CONST_STRPTR)"STP_DATA_PATH", (CONST_STRPTR)GP_DATA_PATH, -1, GVF_LOCAL_ONLY);
    gpjob_init();

    g.profile = calloc(GP_MAX_PROFILES, sizeof(*g.profile));
    g.nmodels = stp_printer_model_count();
    g.model_driver = calloc(g.nmodels, sizeof(*g.model_driver));
    g.model_name = calloc(g.nmodels, sizeof(*g.model_name));
    g.model_maker = calloc(g.nmodels, sizeof(*g.model_maker));
    g.sorted = calloc(g.nmodels, sizeof(int));
    g.filtered = calloc(g.nmodels, sizeof(int));
    g.maker = calloc(g.nmodels, sizeof(*g.maker));
    if (g.profile == NULL || g.model_driver == NULL || g.model_name == NULL ||
        g.model_maker == NULL || g.sorted == NULL || g.filtered == NULL || g.maker == NULL)
        return 20;
    for (i = 0; i < g.nmodels; i++)
    {
        const stp_printer_t *pr = stp_get_printer_by_index(i);

        snprintf(g.model_driver[i], sizeof(g.model_driver[i]), "%s", stp_printer_get_driver(pr));
        snprintf(g.model_name[i], sizeof(g.model_name[i]), "%s", stp_printer_get_long_name(pr));
        snprintf(g.model_maker[i], sizeof(g.model_maker[i]), "%s",
            stp_printer_get_manufacturer(pr)[0] != '\0' ? stp_printer_get_manufacturer(pr) : "Other");
    }
    sort_models();
    DateStamp(&t1);
    load_all();
    if (setup_uri[0])
        return setup_headless(setup_uri, setup_formats[0] ? setup_formats : NULL, unit, setup_save);
    for (i = 0; unit >= 0 && unit < GP_UNITS && i < g.nprofiles; i++)
        if (strcmp(g.profile[i].name, g.units[unit]) == 0)
            g.current = i;

    init_hook(&rebuild_hook, rebuild_func);
    init_hook(&param_hook, param_func);
    init_hook(&profile_hook, profile_func);
    init_hook(&search_hook, search_func);
    init_hook(&model_hook, model_func);
    init_hook(&name_hook, name_func);
    init_hook(&unit_hook, unit_func);
    init_hook(&misc_hook, misc_func);
    init_hook(&maker_hook, maker_func);
    for (i = 0; i < RX_COUNT; i++)
    {
        init_hook(&rexx_hooks[i], rexx_func);
        rexx_hooks[i].h_Data = (APTR)(IPTR)i;
    }

    gpslider_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Slider, NULL,
        sizeof(struct gpslider_data), gpslider_dispatcher);
    gppreview_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Area, NULL,
        sizeof(struct gppreview_data), gppreview_dispatcher);
    gpsupply_class = MUI_CreateCustomClass(NULL, (ClassID)MUIC_Area, NULL,
        sizeof(struct gpsupply_data), gpsupply_dispatcher);
    if (gpslider_class == NULL || gppreview_class == NULL || gpsupply_class == NULL)
        return 20;
    g.app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"GutenprintPrefs",
        MUIA_Application_Version, (IPTR)"$VER: GutenprintPrefs 0.1 (15.10.2026)",
        MUIA_Application_Description, (IPTR)"Gutenprint printer settings",
        MUIA_Application_Base, (IPTR)"GUTENPRINTPREFS",
        MUIA_Application_Commands, (IPTR)rexx_commands,
        SubWindow, (IPTR)(g.win = build_window()),
        SubWindow, (IPTR)(g.fwin = build_find_window()),
        SubWindow, (IPTR)(g.swin = build_status_window()),
        End;
    if (g.app == NULL)
    {
        printf("GutenprintPrefs: cannot create the window\n");
        return 20;
    }

    DoMethod(g.win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)g.app, 2,
        MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(g.cancel, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)g.app, 2,
        MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(g.save, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)g.app, 2, MUIM_Application_ReturnID, 1);
    DoMethod(g.use, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)g.app, 2, MUIM_Application_ReturnID, 2);
    DoMethod(g.test, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)g.app, 2, MUIM_Application_ReturnID, 3);
    DoMethod(g.profiles, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&profile_hook);
    DoMethod(g.search, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&search_hook);
    DoMethod(g.models, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&model_hook);
    DoMethod(g.makers, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&maker_hook);
    DoMethod(g.name, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&name_hook);
    DoMethod(g.unit, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)g.app, 2,
        MUIM_CallHook, (IPTR)&unit_hook);
    notify_misc(g.add, MUIA_Pressed, FALSE, MISC_ADD);
    notify_misc(g.copy, MUIA_Pressed, FALSE, MISC_COPY);
    notify_misc(g.remove, MUIA_Pressed, FALSE, MISC_REMOVE);
    notify_misc(g.level, MUIA_Cycle_Active, MUIV_EveryTime, MISC_LEVEL);
    notify_misc(g.defaults, MUIA_Pressed, FALSE, MISC_DEFAULTS);
    notify_misc(g.connection, MUIA_String_Contents, MUIV_EveryTime, MISC_CONNECTION);
    notify_misc(g.preview_check, MUIA_Selected, MUIV_EveryTime, MISC_PREVIEW);
    notify_misc(g.find, MUIA_Pressed, FALSE, MISC_FIND);
    notify_misc(g.flist, MUIA_List_Active, MUIV_EveryTime, MISC_FOUND_SELECT);
    notify_misc(g.fuse, MUIA_Pressed, FALSE, MISC_FOUND_USE);
    notify_misc(g.fcancel, MUIA_Pressed, FALSE, MISC_FOUND_CANCEL);
    notify_misc(g.fstatus, MUIA_Pressed, FALSE, MISC_FOUND_STATUS);
    notify_misc(g.status_btn, MUIA_Pressed, FALSE, MISC_STATUS);
    notify_misc(g.srefresh, MUIA_Pressed, FALSE, MISC_STATUS_REFRESH);
    notify_misc(g.sclose, MUIA_Pressed, FALSE, MISC_STATUS_CLOSE);
    DoMethod(g.swin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)g.swin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(g.fwin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)g.fwin, 3, MUIM_Set, MUIA_Window_Open, FALSE);

    g.quiet++;
    set(g.makers, MUIA_List_Quiet, TRUE);
    for (i = 0; i < g.nmakers; i++)
        DoMethod(g.makers, MUIM_List_InsertSingle, (IPTR)g.maker[i], MUIV_List_Insert_Bottom);
    set(g.makers, MUIA_List_Quiet, FALSE);
    g.quiet--;
    fill_profile_list();
    show_profile();
    rebuild_tabs();
    set(g.pages, MUIA_Group_ActivePage, page);
    set(g.win, MUIA_Window_Open, TRUE);
    {
        IPTR active = MUIV_List_Active_Off;

        get(g.models, MUIA_List_Active, &active);
        if ((LONG)active >= 0)
            DoMethod(g.models, MUIM_List_Jump, active);
    }
    DateStamp(&t2);
    printf("GutenprintPrefs: %d models, %d manufacturers; Gutenprint data %ld ms, window %ld ms\n",
        g.nmodels, g.nmakers,
        (long)((t1.ds_Minute - t0.ds_Minute) * 60000 + (t1.ds_Tick - t0.ds_Tick) * 20),
        (long)((t2.ds_Minute - t1.ds_Minute) * 60000 + (t2.ds_Tick - t1.ds_Tick) * 20));

    while (running)
    {
        IPTR id = DoMethod(g.app, MUIM_Application_NewInput, (IPTR)&sigs);

        switch (id)
        {
        case 3:
        {
            char why[160];

            /* The renderer reads the settings in use, so they are used first. */
            if (save_to(GP_ENV_DIR) != 0)
                set_status("Cannot write ENV:Gutenprint.");
            else if (print_test_page(why, sizeof(why)) != 0)
                set_status(why);
            else
                set_status("Test page sent to the printer.");
            break;
        }
        case MUIV_Application_ReturnID_Quit:
            running = FALSE;
            break;
        case 1:
        case 2:
            if (save_to(GP_ENV_DIR) != 0 || (id == 1 && save_to(GP_ENVARC_DIR) != 0))
            {
                set_status(id == 1 ? "Cannot save to ENVARC:Gutenprint (read-only disk?); "
                    "Use keeps the settings until reboot." : "Cannot write ENV:Gutenprint.");
                rc = 10;
                break;
            }
            {
                char why[160];
                int u, failed = 0;

                for (u = 0; u < GP_UNITS && !failed; u++)
                {
                    struct gp_profile *bound = NULL;
                    int k;

                    for (k = 0; k < g.nprofiles; k++)
                        if (g.units[u][0] && strcmp(g.profile[k].name, g.units[u]) == 0)
                            bound = &g.profile[k];
                    if (bound == NULL || !is_network(bound->connection))
                        continue;
                    if (gpfind_setup_unit(u, bound->connection, bound->name,
                            strncmp(bound->model, "ps", 2) == 0 ? "application/postscript" : "application/octet-stream",
                            id == 1, why, sizeof(why)) != 0)
                    {
                        set_status(why);
                        failed = 1;
                    }
                    else
                        printf("GutenprintPrefs: unit %d prints to %s through ipp.device\n", u, bound->connection);
                }
                if (failed)
                {
                    rc = 10;
                    break;
                }
            }
            printf("GutenprintPrefs: %s %d printer%s\n", id == 1 ? "saved" : "using",
                g.nprofiles, g.nprofiles == 1 ? "" : "s");
            rc = 0;
            running = FALSE;
            break;
        }
        if (running && sigs)
        {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C)
                running = FALSE;
        }
    }
    set(g.win, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(g.app);
    MUI_DeleteCustomClass(gppreview_class);
    MUI_DeleteCustomClass(gpsupply_class);
    MUI_DeleteCustomClass(gpslider_class);
    free_params();
    return rc;
}
