/*
 * gp-render - print a PPM image with a Gutenprint driver into a file.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 *   gp-render -l                                  list the printer models
 *   gp-render -g DRIVER [Name=value ...]          page and printable area, in dots at 300 dpi
 *   gp-render DRIVER IN.ppm[,IN2.ppm...] OUT [--place=L,T,W,H] [Name=value ...]
 *                                                 print IN.ppm (binary P6, maxval 255), placed
 *                                                 at L,T,W,H dots (300 dpi) in the printable area;
 *                                                 several images print as the pages of one job
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gutenprint/gutenprint.h>

#include "../core/gpjob.h"

struct ppm
{
    int width, height;
    unsigned char *pixels;
};

static FILE *out;
static size_t written;
static unsigned long long hash = 0xcbf29ce484222325ULL;

static int read_ppm(const char *path, struct ppm *ppm)
{
    FILE *f = fopen(path, "rb");
    int maxval;
    size_t size;

    if (f == NULL)
        return 1;
    if (fscanf(f, "P6 %d %d %d", &ppm->width, &ppm->height, &maxval) != 3 ||
        maxval != 255 || fgetc(f) == EOF || ppm->width <= 0 || ppm->height <= 0)
    {
        fclose(f);
        return 1;
    }
    size = (size_t)ppm->width * ppm->height * 3;
    ppm->pixels = malloc(size);
    if (ppm->pixels == NULL || fread(ppm->pixels, 1, size, f) != size)
    {
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

static int get_row(void *user, int row, unsigned char *rgb)
{
    struct ppm *ppm = user;

    if (row < 0 || row >= ppm->height)
        return 1;
    memcpy(rgb, ppm->pixels + (size_t)row * ppm->width * 3, (size_t)ppm->width * 3);
    return 0;
}

static void put_data(void *user, const char *data, size_t bytes)
{
    size_t i;

    (void)user;
    /* FNV-1a over the printer data, so outputs compare without moving them. */
    for (i = 0; i < bytes; i++)
        hash = (hash ^ (unsigned char)data[i]) * 0x100000001b3ULL;
    written += fwrite(data, 1, bytes, out);
}

static void put_error(void *user, const char *text, size_t bytes)
{
    (void)user;
    fwrite(text, 1, bytes, stderr);
}

int main(int argc, char **argv)
{
    struct ppm ppm[64];
    struct gpjob_page page[64];
    struct gpjob *job;
    char why[128], *names, *name;
    int i, pages = 0, rc = 0;

    gpjob_init();
    if (argc == 2 && strcmp(argv[1], "-l") == 0)
    {
        for (i = 0; i < stp_printer_model_count(); i++)
        {
            const stp_printer_t *p = stp_get_printer_by_index(i);

            printf("%s\t%s\n", stp_printer_get_driver(p), stp_printer_get_long_name(p));
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "-g") == 0)
    {
        double pw, ph, l, t, w, h;

        if (gpjob_geometry(argv[2], (const char *const *)&argv[3], &pw, &ph, &l, &t, &w, &h,
                why, sizeof(why)) != 0)
        {
            fprintf(stderr, "gp-render: %s\n", why);
            return 20;
        }
        printf("page %ld %ld area %ld %ld %ld %ld\n", (long)(pw * 300 / 72.0 + 0.5),
            (long)(ph * 300 / 72.0 + 0.5), (long)(l * 300 / 72.0 + 0.5), (long)(t * 300 / 72.0 + 0.5),
            (long)(w * 300 / 72.0 + 0.5), (long)(h * 300 / 72.0 + 0.5));
        return 0;
    }
    if (argc < 4)
    {
        fprintf(stderr, "usage: gp-render -l | DRIVER IN.ppm[,IN2.ppm...] OUT [Name=value ...]\n");
        return 20;
    }
    names = strdup(argv[2]);
    for (name = strtok(names, ","); name != NULL; name = strtok(NULL, ","))
    {
        if (pages == 64 || read_ppm(name, &ppm[pages]))
        {
            fprintf(stderr, "gp-render: cannot read %s as a binary PPM\n", name);
            return 20;
        }
        pages++;
    }
    out = fopen(argv[3], "wb");
    if (out == NULL)
    {
        fprintf(stderr, "gp-render: cannot create %s\n", argv[3]);
        return 20;
    }
    for (i = 0; i < pages; i++)
    {
        page[i].driver = argv[1];
        page[i].options = (const char *const *)&argv[4];
        page[i].width = ppm[i].width;
        page[i].height = ppm[i].height;
        page[i].place_left = page[i].place_top = page[i].place_width = page[i].place_height = 0;
        if (argc > 4 && strncmp(argv[4], "--place=", 8) == 0)
        {
            long l, t, w, h;

            if (sscanf(argv[4] + 8, "%ld,%ld,%ld,%ld", &l, &t, &w, &h) != 4)
            {
                fprintf(stderr, "gp-render: bad %s\n", argv[4]);
                return 20;
            }
            page[i].place_left = l * 72.0 / 300;
            page[i].place_top = t * 72.0 / 300;
            page[i].place_width = w * 72.0 / 300;
            page[i].place_height = h * 72.0 / 300;
            page[i].options = (const char *const *)&argv[5];
        }
        page[i].row = get_row;
        page[i].out = put_data;
        page[i].err = put_error;
        page[i].user = &ppm[i];
    }
    job = gpjob_begin(&page[0], why, sizeof(why));
    for (i = 0; job != NULL && i < pages && rc == 0; i++)
        rc = gpjob_page(job, &page[i], why, sizeof(why));
    if (job != NULL)
        gpjob_end(job, &page[pages - 1]);
    if (job == NULL || rc != 0)
    {
        fprintf(stderr, "gp-render: %s\n", why);
        fclose(out);
        return 20;
    }
    fclose(out);
    printf("gp-render: %s %d page(s) -> %s %lu bytes fnv1a64=%016llx\n", argv[1], pages,
        argv[3], (unsigned long)written, hash);
    return 0;
}
