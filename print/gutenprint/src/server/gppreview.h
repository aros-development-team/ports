/*
 * gppreview - the print preview window of GutenprintRender.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#ifndef GPPREVIEW_H
#define GPPREVIEW_H

#include <exec/types.h>

#include "../core/gpcolor.h"

struct gppreview_page
{
    const char *printer;            /* profile name */
    const char *model;              /* Gutenprint model's long name */
    LONG unit, page_number;         /* page_number from 1 within the job */
    /* the page and printable area, in dots at GP_LAYOUT_DPI */
    LONG page_width, page_height, area_left, area_top, area_width, area_height;
    /* the page image and where it goes in the printable area; place_width 0
       fills the printable area */
    const UBYTE *pixels;
    LONG image_width, image_height;
    LONG place_left, place_top, place_width, place_height;
    struct gpcolor_adjust adjust;   /* in: the profile's values; out: the user's */
    int offers[4];                  /* brightness, contrast, saturation, gamma offered by the model */
    int keep;                       /* out: keep the adjustments in the profile */
};

enum
{
    GPPREVIEW_PRINT,        /* print this page */
    GPPREVIEW_PRINT_ALL,    /* print this and the job's remaining pages unasked */
    GPPREVIEW_CANCEL,       /* cancel the job */
    GPPREVIEW_FAILED        /* no window could be opened: print */
};

/* Shows the page and returns what the user chose; p->adjust and p->keep
   hold the adjustments on return. */
int gppreview_show(struct gppreview_page *p);

#endif
