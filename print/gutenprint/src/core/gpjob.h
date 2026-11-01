/*
 * gpjob - one page through Gutenprint.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#ifndef GPJOB_H
#define GPJOB_H

#include <stddef.h>

/* Fills row (0 = top) of the page image: width * 3 bytes of 8-bit RGB. */
typedef int (*gpjob_row_fn)(void *user, int row, unsigned char *rgb);
/* Receives the printer data in order. */
typedef void (*gpjob_out_fn)(void *user, const char *data, size_t bytes);
/* Receives Gutenprint's messages. */
typedef void (*gpjob_err_fn)(void *user, const char *text, size_t bytes);

struct gpjob_page
{
    const char *driver;         /* Gutenprint driver name, e.g. "escp2-r2400" */
    const char *const *options; /* "Name=value" strings, NULL-terminated */
    int width, height;          /* image size in pixels */
    /* Where the image goes, in points from the printable area's top left;
       place_width 0 fills the printable area. */
    double place_left, place_top, place_width, place_height;
    gpjob_row_fn row;
    gpjob_out_fn out;
    gpjob_err_fn err;
    void *user;
};

/* Sets "Name=value" on a stp_vars_t with the type Gutenprint declares for
   Name; 0 when Name is not a parameter or its type cannot be set. */
int gpjob_set_option(void *vars, const char *option);
/* Call once before anything else. */
void gpjob_init(void);
/* 0 on success; otherwise a short reason is copied to why. */
struct gpjob;
struct gpjob *gpjob_begin(const struct gpjob_page *first, char *why, size_t why_size);
int gpjob_page(struct gpjob *job, const struct gpjob_page *page, char *why, size_t why_size);
/* Changes a "Name=value" option of a started job for its next pages;
   0 when Name is not a parameter. */
int gpjob_set(struct gpjob *job, const char *option);
/* Whether Gutenprint offers the parameter name on this driver with these
   options (it is active, e.g. Saturation only when printing in colour). */
int gpjob_option_active(const char *driver, const char *const *options, const char *name);
/* The model's long name, or driver when it is unknown. */
const char *gpjob_model_name(const char *driver);
void gpjob_end(struct gpjob *job, const struct gpjob_page *last);
int gpjob_print(const struct gpjob_page *page, char *why, size_t why_size);
/* Page size and printable area in points for a driver and its options. */
int gpjob_geometry(const char *driver, const char *const *options, double *page_width,
    double *page_height, double *left, double *top, double *width, double *height,
    char *why, size_t why_size);

#endif
