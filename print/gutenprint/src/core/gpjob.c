/*
 * gpjob - pages through Gutenprint, one job at a time.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gutenprint/gutenprint.h>

#include "gpjob.h"
#include "gpprofile.h"
#ifdef __AROS__
#include <proto/dos.h>
#endif

struct gpjob_image
{
    const struct gpjob_page *page;
    int failed;
};

static void image_init(stp_image_t *image) { (void)image; }
static void image_reset(stp_image_t *image) { (void)image; }
static void image_conclude(stp_image_t *image) { (void)image; }
static const char *image_appname(stp_image_t *image) { (void)image; return "Gutenprint for AROS"; }

static int image_width(stp_image_t *image)
{
    return ((struct gpjob_image *)image->rep)->page->width;
}

static int image_height(stp_image_t *image)
{
    return ((struct gpjob_image *)image->rep)->page->height;
}

static stp_image_status_t image_get_row(stp_image_t *image, unsigned char *data,
    size_t byte_limit, int row)
{
    struct gpjob_image *job = image->rep;

    if (byte_limit < (size_t)job->page->width * 3 ||
        job->page->row(job->page->user, row, data) != 0)
    {
        job->failed = 1;
        return STP_IMAGE_STATUS_ABORT;
    }
    return STP_IMAGE_STATUS_OK;
}

static void out_func(void *data, const char *buffer, size_t bytes)
{
    const struct gpjob_page *page = data;

    page->out(page->user, buffer, bytes);
}

static void discard_func(void *data, const char *buffer, size_t bytes)
{
    (void)data;
    (void)buffer;
    (void)bytes;
}

static void err_func(void *data, const char *buffer, size_t bytes)
{
    const struct gpjob_page *page = data;

    if (page->err != NULL)
        page->err(page->user, buffer, bytes);
}

/* Sets "Name=value" with the type Gutenprint declares for Name. */
int gpjob_set_option(void *vars, const char *option)
{
    stp_parameter_t desc;
    const char *equals = strchr(option, '=');
    stp_vars_t *v = vars;
    char name[64];
    const char *value;
    int ok = 1;

    if (equals == NULL || (size_t)(equals - option) >= sizeof(name))
        return 0;
    memcpy(name, option, equals - option);
    name[equals - option] = '\0';
    value = equals + 1;
    stp_describe_parameter(v, name, &desc);
    switch (desc.p_type)
    {
    case STP_PARAMETER_TYPE_STRING_LIST:
    case STP_PARAMETER_TYPE_RAW:
        stp_set_string_parameter(v, name, value);
        break;
    case STP_PARAMETER_TYPE_INT:
        stp_set_int_parameter(v, name, atoi(value));
        break;
    case STP_PARAMETER_TYPE_BOOLEAN:
        stp_set_boolean_parameter(v, name, strcmp(value, "true") == 0 ||
            strcmp(value, "1") == 0);
        break;
    case STP_PARAMETER_TYPE_DOUBLE:
        stp_set_float_parameter(v, name, atof(value));
        break;
    case STP_PARAMETER_TYPE_DIMENSION:
        stp_set_dimension_parameter(v, name, atof(value));
        break;
    case STP_PARAMETER_TYPE_FILE:
        stp_set_file_parameter(v, name, value);
        break;
    default:
        ok = 0;
        break;
    }
    stp_parameter_description_destroy(&desc);
    return ok;
}

void gpjob_init(void)
{
#ifdef __AROS__
    /* The user's data overlay, then the installed data. */
    if (getenv("STP_DATA_PATH") == NULL)
        SetVar((CONST_STRPTR)"STP_DATA_PATH", (CONST_STRPTR)GP_DATA_PATH, -1, GVF_LOCAL_ONLY);
#endif
    stp_init();
}

/* A vars object for driver with options applied, or NULL with why set. */
static stp_vars_t *make_vars(const char *driver, const char *const *options,
    char *why, size_t why_size)
{
    const stp_printer_t *printer = stp_get_printer_by_driver(driver);
    stp_vars_t *v;
    int i;

    if (printer == NULL)
    {
        snprintf(why, why_size, "unknown printer driver '%s'", driver);
        return NULL;
    }
    v = stp_vars_create();
    stp_set_printer_defaults(v, printer);
    stp_set_string_parameter(v, "InputImageType", "RGB");
    stp_set_string_parameter(v, "ChannelBitDepth", "8");
    for (i = 0; options != NULL && options[i] != NULL; i++)
        if (!gpjob_set_option(v, options[i]))
        {
            snprintf(why, why_size, "bad option '%s'", options[i]);
            stp_vars_destroy(v);
            return NULL;
        }
    stp_set_printer_defaults_soft(v, printer);
    return v;
}

int gpjob_geometry(const char *driver, const char *const *options, double *page_width,
    double *page_height, double *left, double *top, double *width, double *height,
    char *why, size_t why_size)
{
    stp_vars_t *v = make_vars(driver, options, why, why_size);
    stp_dimension_t l, r, b, t, pw, ph;

    if (v == NULL)
        return 1;
    stp_get_media_size(v, &pw, &ph);
    stp_get_imageable_area(v, &l, &r, &b, &t);
    *page_width = pw;
    *page_height = ph;
    *left = l;
    *top = t;
    *width = r - l;
    *height = b - t;
    stp_vars_destroy(v);
    return 0;
}

struct gpjob
{
    stp_vars_t *v;
    struct gpjob_image image;
    stp_image_t stp_image;
    stp_dimension_t left, right, bottom, top;
};

/* Places page on the printable area of job's vars and makes it the image
   and output target of the next Gutenprint call. */
static void use_page(struct gpjob *job, const struct gpjob_page *page)
{
    job->image.page = page;
    job->image.failed = 0;
    stp_set_outdata(job->v, (void *)page);
    stp_set_errdata(job->v, (void *)page);
    if (page->place_width > 0)
    {
        /* A placement converted from device dots can overshoot the
           printable area by a rounding error, which Gutenprint rejects. */
        double width = job->right - job->left - page->place_left;
        double height = job->bottom - job->top - page->place_top;

        stp_set_left(job->v, job->left + page->place_left);
        stp_set_top(job->v, job->top + page->place_top);
        stp_set_width(job->v, page->place_width < width ? page->place_width : width);
        stp_set_height(job->v, page->place_height < height ? page->place_height : height);
    }
    else
    {
        /* The page image fills the printable area. */
        stp_set_left(job->v, job->left);
        stp_set_top(job->v, job->top);
        stp_set_width(job->v, job->right - job->left);
        stp_set_height(job->v, job->bottom - job->top);
    }
}

/* Starts a job for the printer and options of first, whose output (the
   printer's job set-up) goes to first's out function. NULL with why set. */
struct gpjob *gpjob_begin(const struct gpjob_page *first, char *why, size_t why_size)
{
    struct gpjob *job = calloc(1, sizeof(*job));

    if (job == NULL)
    {
        snprintf(why, why_size, "out of memory");
        return NULL;
    }
    job->v = make_vars(first->driver, first->options, why, why_size);
    if (job->v == NULL)
    {
        free(job);
        return NULL;
    }
    job->stp_image = (stp_image_t) {
        image_init, image_reset, image_width, image_height, image_get_row,
        image_appname, image_conclude, &job->image
    };
    stp_set_outfunc(job->v, out_func);
    stp_set_errfunc(job->v, err_func);
    stp_get_imageable_area(job->v, &job->left, &job->right, &job->bottom, &job->top);
    use_page(job, first);
    if (!stp_verify(job->v))
    {
        snprintf(why, why_size, "Gutenprint rejected the settings");
        stp_vars_destroy(job->v);
        free(job);
        return NULL;
    }
    stp_start_job(job->v, &job->stp_image);
    return job;
}

/* Prints one page of job. */
int gpjob_page(struct gpjob *job, const struct gpjob_page *page, char *why, size_t why_size)
{
    use_page(job, page);
    if (stp_print(job->v, &job->stp_image) != 1 || job->image.failed)
    {
        snprintf(why, why_size, "printing failed");
        return 1;
    }
    return 0;
}

int gpjob_set(struct gpjob *job, const char *option)
{
    return gpjob_set_option(job->v, option);
}

const char *gpjob_model_name(const char *driver)
{
    const stp_printer_t *printer = stp_get_printer_by_driver(driver);

    return printer != NULL ? stp_printer_get_long_name(printer) : driver;
}

int gpjob_option_active(const char *driver, const char *const *options, const char *name)
{
    char why[80];
    stp_vars_t *v = make_vars(driver, options, why, sizeof(why));
    stp_parameter_t desc;
    int active;

    if (v == NULL)
        return 0;
    stp_describe_parameter(v, name, &desc);
    active = desc.p_type != STP_PARAMETER_TYPE_INVALID && desc.is_active;
    stp_parameter_description_destroy(&desc);
    stp_vars_destroy(v);
    return active;
}

/* Ends job; the printer's end-of-job data goes to last's out function, or
   is dropped when last is NULL (a job abandoned by its client). */
void gpjob_end(struct gpjob *job, const struct gpjob_page *last)
{
    static const struct gpjob_page discard;

    if (last == NULL)
    {
        stp_set_outfunc(job->v, discard_func);
        last = &discard;
    }
    use_page(job, last);
    stp_end_job(job->v, &job->stp_image);
    stp_vars_destroy(job->v);
    free(job);
}

int gpjob_print(const struct gpjob_page *page, char *why, size_t why_size)
{
    struct gpjob *job = gpjob_begin(page, why, why_size);
    int rc;

    if (job == NULL)
        return 1;
    rc = gpjob_page(job, page, why, why_size);
    gpjob_end(job, page);
    return rc;
}
