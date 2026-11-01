/*
 * gpcolor - Gutenprint's brightness, contrast, saturation and gamma
 * adjustments on 8-bit RGB, for the print preview.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * The curves are the ones Gutenprint applies to a page (print-color.c,
 * color-conversions.c), in the same order: contrast per channel, then
 * brightness on the lightness and saturation in HSL, then the composite
 * gamma. The printer's fixed linearisation (its screen gamma curve) is left
 * out, so with every value at 1.0 the preview shows the image as the
 * application sent it, and a change shows the same curve the print gets.
 */
#ifndef GPCOLOR_H
#define GPCOLOR_H

#include <stddef.h>

struct gpcolor_adjust
{
    double brightness;      /* 0..2, 1 = unchanged */
    double contrast;        /* 0..4 */
    double saturation;      /* 0..9 */
    double gamma;           /* 0.1..4 */
    int linear_contrast;    /* Gutenprint's LinearContrast */
};

struct gpcolor_lut
{
    unsigned char contrast[256];
    unsigned char gamma[256];
    double brightness[257];     /* lightness curve, by lightness * 256 */
    double ssat, isat;
    int do_hsl, split_saturation;
};

/* The Gutenprint defaults: everything 1.0. */
void gpcolor_defaults(struct gpcolor_adjust *a);
void gpcolor_prepare(struct gpcolor_lut *lut, const struct gpcolor_adjust *a);
/* out = in adjusted; in and out may be the same buffer. */
void gpcolor_apply(const struct gpcolor_lut *lut, const unsigned char *in, unsigned char *out,
    size_t pixels);

#endif
