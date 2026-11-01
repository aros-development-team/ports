/*
 * GutenprintRender - runs Gutenprint for the Gutenprint printer driver.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * The driver (printer.device's task) sends GPCMD_GEOMETRY, GPCMD_PRINT and
 * GPCMD_ENDJOB messages to the public port GUTENPRINT.RENDER; the printer data
 * comes back as GPCMD_DATA messages, each replied once the driver has written
 * it. The pages of one printer.device job (gm_JobID) are one Gutenprint job,
 * started by the first page and ended by GPCMD_ENDJOB. A profile with
 * Preview=yes shows each page in the preview window first (gppreview.c).
 */
#include <aros/debug.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/gpjob.h"
#include "../core/gpproto.h"
#include "../core/gpprofile.h"
#include "gppreview.h"

#define CHUNK_SIZE  65536

struct settings
{
    struct gp_profile profile;
    const char *options[GP_MAX_OPTIONS + 1];
    char buf[GP_MAX_OPTIONS][130];
};

struct print_state
{
    struct GPMsg *request;
    struct MsgPort *reply_port;
    struct GPMsg data_msg;
    UBYTE *chunk;
    LONG used;
    int failed;
};

struct open_job
{
    struct gpjob *job;
    ULONG id;
    struct settings s;
    LONG pages;             /* pages of the job so far */
    int print_all;          /* the preview was told to print the rest unasked */
};

static struct open_job jobs[GP_MAX_UNITS];

static int read_settings(LONG unit, struct settings *s, char *why, size_t why_size)
{
    if (gpprofile_for_unit(unit, &s->profile, why, why_size) != 0)
        return 1;
    gpprofile_options(&s->profile, s->options, s->buf);
    return 0;
}

static LONG to_dots(double points)
{
    return (LONG)(points * GP_LAYOUT_DPI / 72.0 + 0.5);
}

static double to_points(LONG dots)
{
    return dots * 72.0 / GP_LAYOUT_DPI;
}

/* The page and printable area of the settings, in dots: page width and
   height, area left, top, width and height. 0 on success. */
static int geometry_dots(const struct settings *s, LONG dots[6], char *why, size_t why_size)
{
    double pw, ph, l, t, w, h;

    if (gpjob_geometry(s->profile.model, s->options, &pw, &ph, &l, &t, &w, &h, why, why_size) != 0)
        return 1;
    if (w < 1 || h < 1)
    {
        /* Gutenprint's PostScript models have no page sizes without a PPD file. */
        snprintf(why, why_size, "%s has no printable area (a PostScript model needs its PPD file)",
            s->profile.model);
        return 1;
    }
    dots[0] = to_dots(pw);
    dots[1] = to_dots(ph);
    dots[2] = to_dots(l);
    dots[3] = to_dots(t);
    dots[4] = to_dots(w);
    dots[5] = to_dots(h);
    return 0;
}

static void do_geometry(struct GPMsg *m)
{
    static struct settings s;
    LONG dots[6];

    m->gm_Result = 1;
    if (read_settings(m->gm_Unit, &s, m->gm_Error, sizeof(m->gm_Error)) == 0 &&
        geometry_dots(&s, dots, m->gm_Error, sizeof(m->gm_Error)) == 0)
    {
        m->gm_PageWidth = dots[0];
        m->gm_PageHeight = dots[1];
        m->gm_AreaLeft = dots[2];
        m->gm_AreaTop = dots[3];
        m->gm_AreaWidth = dots[4];
        m->gm_AreaHeight = dots[5];
        m->gm_Result = 0;
    }
}

static const char *const adjust_names[4] = { "Brightness", "Contrast", "Saturation", "Gamma" };

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

/* Writes the profile with the preview's adjustments to ENV: and ENVARC:,
   for a unit bound to a profile; the old single Settings file is not
   rewritten. */
static void keep_adjustments(LONG unit, const struct gp_profile *p)
{
    char units[GP_UNITS][64];

    if (gpunits_load(GP_ENV_DIR, units) != 0 || unit < 0 || unit >= GP_UNITS || units[unit][0] == '\0')
    {
        bug("[GutenprintRender] unit %ld: adjustments not kept, it uses " GP_ENV_DIR "/Settings\n", (long)unit);
        return;
    }
    if (gpprofile_save(GP_ENV_DIR, p) != 0 || gpprofile_save(GP_ENVARC_DIR, p) != 0)
        bug("[GutenprintRender] unit %ld: cannot save the adjustments to printer '%s'\n", (long)unit, p->name);
}

/* Shows the page in the preview window when the profile asks for one. 0 to
   print it (with the adjustments put into the job's settings), 1 to cancel
   the job with m->gm_Error set. */
static int preview_page(struct GPMsg *m, struct open_job *oj)
{
    struct gppreview_page pv;
    struct gpcolor_adjust before;
    LONG dots[6];
    int i, choice;

    if (!oj->s.profile.preview || oj->print_all)
        return 0;
    if (geometry_dots(&oj->s, dots, m->gm_Error, sizeof(m->gm_Error)) != 0)
        return 1;
    memset(&pv, 0, sizeof(pv));
    pv.printer = oj->s.profile.name;
    pv.model = gpjob_model_name(oj->s.profile.model);
    pv.unit = m->gm_Unit;
    pv.page_number = oj->pages;
    pv.page_width = dots[0];
    pv.page_height = dots[1];
    pv.area_left = dots[2];
    pv.area_top = dots[3];
    pv.area_width = dots[4];
    pv.area_height = dots[5];
    pv.pixels = m->gm_Pixels;
    pv.image_width = m->gm_ImageWidth;
    pv.image_height = m->gm_ImageHeight;
    pv.place_left = m->gm_PlaceLeft;
    pv.place_top = m->gm_PlaceTop;
    pv.place_width = m->gm_PlaceWidth;
    pv.place_height = m->gm_PlaceHeight;
    gpcolor_defaults(&pv.adjust);
    for (i = 0; i < 4; i++)
    {
        const char *v = gpprofile_get(&oj->s.profile, adjust_names[i]);

        if (v != NULL)
            *adjust_field(&pv.adjust, i) = atof(v);
        pv.offers[i] = gpjob_option_active(oj->s.profile.model, oj->s.options, adjust_names[i]);
    }
    {
        const char *v = gpprofile_get(&oj->s.profile, "LinearContrast");

        pv.adjust.linear_contrast = v != NULL && (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    before = pv.adjust;
    choice = gppreview_show(&pv);
    if (choice == GPPREVIEW_CANCEL)
    {
        snprintf(m->gm_Error, sizeof(m->gm_Error), "cancelled in the print preview");
        return 1;
    }
    if (choice == GPPREVIEW_PRINT_ALL)
        oj->print_all = 1;
    if (choice == GPPREVIEW_FAILED)
        return 0;
    /* A moved slider becomes the job's setting; one left alone leaves the
       profile's setting, or its absence, as it was. */
    for (i = 0; i < 4; i++)
    {
        double value = *adjust_field(&pv.adjust, i);
        char text[32];

        if (!pv.offers[i] || value == *adjust_field(&before, i))
            continue;
        snprintf(text, sizeof(text), "%.3f", value);
        gpprofile_set(&oj->s.profile, adjust_names[i], text);
        if (oj->job != NULL)
        {
            char option[64];

            snprintf(option, sizeof(option), "%s=%s", adjust_names[i], text);
            gpjob_set(oj->job, option);
        }
    }
    gpprofile_options(&oj->s.profile, oj->s.options, oj->s.buf);
    if (pv.keep)
        keep_adjustments(m->gm_Unit, &oj->s.profile);
    return 0;
}

static void flush_chunk(struct print_state *ps)
{
    if (ps->used == 0 || ps->failed)
        return;
    ps->data_msg.gm_Msg.mn_ReplyPort = ps->reply_port;
    ps->data_msg.gm_Msg.mn_Length = sizeof(ps->data_msg);
    ps->data_msg.gm_Command = GPCMD_DATA;
    ps->data_msg.gm_Data = ps->chunk;
    ps->data_msg.gm_Length = ps->used;
    ps->data_msg.gm_Result = 0;
    PutMsg(ps->request->gm_DataPort, &ps->data_msg.gm_Msg);
    WaitPort(ps->reply_port);
    GetMsg(ps->reply_port);
    if (ps->data_msg.gm_Result != 0)
        ps->failed = 1;
    ps->used = 0;
}

static void put_data(void *user, const char *data, size_t bytes)
{
    struct print_state *ps = user;

    while (bytes > 0 && !ps->failed)
    {
        size_t n = CHUNK_SIZE - ps->used;

        if (n > bytes)
            n = bytes;
        memcpy(ps->chunk + ps->used, data, n);
        ps->used += n;
        data += n;
        bytes -= n;
        if (ps->used == CHUNK_SIZE)
            flush_chunk(ps);
    }
}

static int get_row(void *user, int row, unsigned char *rgb)
{
    struct print_state *ps = user;
    struct GPMsg *m = ps->request;

    if (row < 0 || row >= m->gm_ImageHeight || ps->failed)
        return 1;
    memcpy(rgb, m->gm_Pixels + (size_t)row * m->gm_ImageWidth * 3, (size_t)m->gm_ImageWidth * 3);
    return 0;
}

static void put_error(void *user, const char *text, size_t bytes)
{
    (void)user;
    fwrite(text, 1, bytes, stderr);
}

static int begin_state(struct print_state *ps, struct GPMsg *m, struct MsgPort *reply_port)
{
    memset(ps, 0, sizeof(*ps));
    ps->request = m;
    ps->reply_port = reply_port;
    ps->chunk = AllocVec(CHUNK_SIZE, MEMF_PUBLIC);
    if (ps->chunk == NULL)
    {
        snprintf(m->gm_Error, sizeof(m->gm_Error), "out of memory");
        return 1;
    }
    return 0;
}

/* Sends what is left of the state's output; 0 when the driver took it all. */
static int end_state(struct print_state *ps)
{
    flush_chunk(ps);
    FreeVec(ps->chunk);
    if (ps->failed)
    {
        snprintf(ps->request->gm_Error, sizeof(ps->request->gm_Error),
            "the printer stopped taking data");
        return 1;
    }
    return 0;
}

static void init_page(struct gpjob_page *page, struct print_state *ps)
{
    memset(page, 0, sizeof(*page));
    page->row = get_row;
    page->out = put_data;
    page->err = put_error;
    page->user = ps;
}

static struct open_job *job_for(struct GPMsg *m)
{
    if (m->gm_Unit < 0 || m->gm_Unit >= GP_MAX_UNITS)
    {
        snprintf(m->gm_Error, sizeof(m->gm_Error), "no printer unit %ld", (long)m->gm_Unit);
        return NULL;
    }
    return &jobs[m->gm_Unit];
}

static void do_print(struct GPMsg *m, struct MsgPort *reply_port)
{
    struct open_job *oj = job_for(m);
    struct print_state ps;
    struct gpjob_page page;
    int failed;

    m->gm_Result = 1;
    if (oj == NULL)
        return;
    if (oj->job != NULL && oj->id != m->gm_JobID)
    {
        /* Its driver went away without ending it. */
        gpjob_end(oj->job, NULL);
        oj->job = NULL;
    }
    if (oj->job == NULL)
    {
        if (read_settings(m->gm_Unit, &oj->s, m->gm_Error, sizeof(m->gm_Error)) != 0)
            return;
        oj->pages = 0;
        oj->print_all = 0;
    }
    oj->pages++;
    if (preview_page(m, oj) != 0)
    {
        if (oj->job != NULL)
        {
            gpjob_end(oj->job, NULL);
            oj->job = NULL;
        }
        return;
    }
    if (begin_state(&ps, m, reply_port) != 0)
        return;
    init_page(&page, &ps);
    page.driver = oj->s.profile.model;
    page.options = oj->s.options;
    page.width = m->gm_ImageWidth;
    page.height = m->gm_ImageHeight;
    page.place_left = to_points(m->gm_PlaceLeft);
    page.place_top = to_points(m->gm_PlaceTop);
    page.place_width = to_points(m->gm_PlaceWidth);
    page.place_height = to_points(m->gm_PlaceHeight);
    if (page.place_width > 0 && (page.place_width < 1 || page.place_height < 1))
    {
        snprintf(m->gm_Error, sizeof(m->gm_Error), "the image is placed smaller than one point");
        FreeVec(ps.chunk);
        return;
    }
    if (oj->job == NULL)
    {
        oj->job = gpjob_begin(&page, m->gm_Error, sizeof(m->gm_Error));
        oj->id = m->gm_JobID;
    }
    failed = oj->job == NULL ||
        gpjob_page(oj->job, &page, m->gm_Error, sizeof(m->gm_Error)) != 0;
    if (end_state(&ps) != 0)
        failed = 1;
    if (failed && oj->job != NULL)
    {
        gpjob_end(oj->job, NULL);
        oj->job = NULL;
    }
    if (!failed)
        m->gm_Result = 0;
    printf("GutenprintRender: unit %ld job %lu %s (%s) %ldx%ld at %ld,%ld,%ld,%ld -> %s\n",
        (long)m->gm_Unit, (unsigned long)m->gm_JobID, oj->s.profile.name, oj->s.profile.model,
        (long)m->gm_ImageWidth, (long)m->gm_ImageHeight, (long)m->gm_PlaceLeft,
        (long)m->gm_PlaceTop, (long)m->gm_PlaceWidth, (long)m->gm_PlaceHeight,
        m->gm_Result == 0 ? "printed" : m->gm_Error);
}

static void do_endjob(struct GPMsg *m, struct MsgPort *reply_port)
{
    struct open_job *oj = job_for(m);
    struct print_state ps;
    struct gpjob_page page;

    m->gm_Result = 1;
    if (oj == NULL)
        return;
    if (oj->job == NULL || oj->id != m->gm_JobID)
    {
        m->gm_Result = 0;
        return;
    }
    if (begin_state(&ps, m, reply_port) != 0)
        return;
    init_page(&page, &ps);
    gpjob_end(oj->job, &page);
    oj->job = NULL;
    if (end_state(&ps) == 0)
        m->gm_Result = 0;
    printf("GutenprintRender: unit %ld job %lu ended\n", (long)m->gm_Unit,
        (unsigned long)m->gm_JobID);
}

int main(void)
{
    struct MsgPort *port, *reply_port;
    int running = 1;

    Forbid();
    if (FindPort((CONST_STRPTR)GP_PORTNAME) != NULL)
    {
        Permit();
        printf("GutenprintRender: already running\n");
        return 5;
    }
    port = CreateMsgPort();
    if (port != NULL)
    {
        port->mp_Node.ln_Name = (char *)GP_PORTNAME;
        port->mp_Node.ln_Pri = 0;
        AddPort(port);
    }
    Permit();
    reply_port = CreateMsgPort();
    if (port == NULL || reply_port == NULL)
    {
        if (port != NULL)
        {
            RemPort(port);
            DeleteMsgPort(port);
        }
        DeleteMsgPort(reply_port);
        return 20;
    }
    /* The user's data overlay, then the installed data. */
    if (getenv("STP_DATA_PATH") == NULL)
        SetVar((CONST_STRPTR)"STP_DATA_PATH", (CONST_STRPTR)GP_DATA_PATH, -1, GVF_LOCAL_ONLY);
    gpjob_init();
    printf("GutenprintRender: ready\n");
    while (running)
    {
        struct GPMsg *m;

        WaitPort(port);
        while ((m = (struct GPMsg *)GetMsg(port)) != NULL)
        {
            m->gm_Error[0] = '\0';
            switch (m->gm_Command)
            {
            case GPCMD_GEOMETRY:
                do_geometry(m);
                break;
            case GPCMD_PRINT:
                do_print(m, reply_port);
                break;
            case GPCMD_ENDJOB:
                do_endjob(m, reply_port);
                break;
            case GPCMD_QUIT:
                running = 0;
                m->gm_Result = 0;
                break;
            default:
                m->gm_Result = 1;
                snprintf(m->gm_Error, sizeof(m->gm_Error), "unknown command %lu",
                    (unsigned long)m->gm_Command);
                break;
            }
            ReplyMsg(&m->gm_Msg);
        }
    }
    Forbid();
    RemPort(port);
    while (GetMsg(port) != NULL)
        ;
    Permit();
    DeleteMsgPort(port);
    DeleteMsgPort(reply_port);
    return 0;
}
