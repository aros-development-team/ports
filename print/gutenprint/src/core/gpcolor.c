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
 * The curve maths follows Gutenprint 5.3.5 src/main/print-color.c
 * (compute_user_correction, compute_a_curve_full) and
 * color-conversions.c (update_saturation, the RGB/HSL conversions from
 * the GIMP), under the same license.
 */
#include <math.h>
#include <string.h>

#include "gpcolor.h"

void gpcolor_defaults(struct gpcolor_adjust *a)
{
    a->brightness = 1.0;
    a->contrast = 1.0;
    a->saturation = 1.0;
    a->gamma = 1.0;
    a->linear_contrast = 0;
}

/* compute_user_correction's contrast step. */
static double contrast_curve(double pixel, double contrast, int linear)
{
    double temp_pixel = pixel >= .5 ? 1.0 - pixel : pixel;

    if (contrast > 3.99999)
        temp_pixel = temp_pixel < .5 ? 0 : 1;
    if (temp_pixel <= .000001 && contrast <= .0001)
        temp_pixel = .5;
    else if (temp_pixel > 1)
        temp_pixel = .5 * pow(2 * temp_pixel, contrast);
    else if (temp_pixel < 1)
    {
        if (linear)
            temp_pixel = 0.5 - ((0.5 - .5 * pow(2 * temp_pixel, contrast)) * contrast);
        else
            temp_pixel = 0.5 - ((0.5 - .5 * pow(2 * temp_pixel, contrast)));
    }
    if (temp_pixel > .5)
        temp_pixel = .5;
    else if (temp_pixel < 0)
        temp_pixel = 0;
    return pixel < .5 ? temp_pixel : 1 - temp_pixel;
}

/* compute_user_correction's brightness step. */
static double brightness_curve(double pixel, double brightness)
{
    if (brightness < .001)
        brightness = 1000;
    else if (brightness > 1.999)
        brightness = .001;
    else if (brightness > 1)
        brightness = 2.0 - brightness;
    else
        brightness = 1.0 / brightness;
    if (brightness < 1)
        return pow(pixel, brightness);
    return 1.0 - pow(1.0 - pixel, 1.0 / brightness);
}

/* compute_a_curve_full's print gamma step, on the amount of ink. */
static double gamma_curve(double ink, double gamma)
{
    double print_gamma = 1.0 + 9.0 * (gamma - 1.0);
    double pivot = .75, ipivot = .25;
    double xgamma = pow(pivot, print_gamma);

    if (fabs(1.0 - xgamma) < 1e-9)
        return ink;
    return (1.0 / (1.0 - xgamma)) * (pow(pivot + ipivot * ink, print_gamma) - xgamma);
}

static unsigned char to_byte(double v)
{
    if (v <= 0)
        return 0;
    if (v >= 1)
        return 255;
    return (unsigned char)floor(v * 255 + .5);
}

void gpcolor_prepare(struct gpcolor_lut *lut, const struct gpcolor_adjust *a)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        double pixel = i / 255.0;

        lut->contrast[i] = to_byte(contrast_curve(pixel, a->contrast, a->linear_contrast));
        lut->gamma[i] = to_byte(1.0 - gamma_curve(1.0 - pixel, a->gamma));
    }
    for (i = 0; i <= 256; i++)
        lut->brightness[i] = brightness_curve(i / 256.0, a->brightness);
    lut->ssat = a->saturation;
    lut->isat = 1.0;
    lut->split_saturation = lut->ssat > 1.4;
    if (lut->split_saturation)
        lut->ssat = sqrt(lut->ssat);
    if (lut->ssat > 1)
        lut->isat = 1.0 / lut->ssat;
    lut->do_hsl = a->brightness != 1.0 || a->saturation <= .99999 || a->saturation >= 1.00001;
}

static double fmin2(double a, double b)
{
    return a < b ? a : b;
}

static void rgb_to_hsl(const double *rgb, double *hue, double *sat, double *lightness)
{
    double red = rgb[0], green = rgb[1], blue = rgb[2];
    double h, s, l, min, max, delta;
    int maxval;

    if (red > green)
    {
        if (red > blue)
        {
            max = red;
            maxval = 0;
        }
        else
        {
            max = blue;
            maxval = 2;
        }
        min = fmin2(green, blue);
    }
    else
    {
        if (green > blue)
        {
            max = green;
            maxval = 1;
        }
        else
        {
            max = blue;
            maxval = 2;
        }
        min = fmin2(red, blue);
    }
    l = (max + min) / 2.0;
    delta = max - min;
    if (delta < .000001)
    {
        s = 0.0;
        h = 0.0;
    }
    else
    {
        if (l <= .5)
            s = delta / (max + min);
        else
            s = delta / (2 - max - min);
        if (maxval == 0)
            h = (green - blue) / delta;
        else if (maxval == 1)
            h = 2 + (blue - red) / delta;
        else
            h = 4 + (red - green) / delta;
        if (h < 0.0)
            h += 6.0;
        else if (h > 6.0)
            h -= 6.0;
    }
    *hue = h;
    *sat = s;
    *lightness = l;
}

static double hsl_value(double n1, double n2, double hue)
{
    if (hue < 0)
        hue += 6.0;
    else if (hue > 6)
        hue -= 6.0;
    if (hue < 1.0)
        return n1 + (n2 - n1) * hue;
    if (hue < 3.0)
        return n2;
    if (hue < 4.0)
        return n1 + (n2 - n1) * (4.0 - hue);
    return n1;
}

static void hsl_to_rgb(double *rgb, double h, double s, double l)
{
    if (s < .0000001)
    {
        if (l > 1)
            l = 1;
        else if (l < 0)
            l = 0;
        rgb[0] = rgb[1] = rgb[2] = l;
    }
    else
    {
        double m1, m2;

        if (l < .5)
            m2 = l * (1 + s);
        else
            m2 = l + s - (l * s);
        m1 = (l * 2) - m2;
        rgb[0] = hsl_value(m1, m2, h + 2);
        rgb[1] = hsl_value(m1, m2, h);
        rgb[2] = hsl_value(m1, m2, h - 2);
    }
}

static double update_saturation(double sat, double adjust, double isat)
{
    if (adjust < 1)
        sat *= adjust;
    else if (adjust > 1)
    {
        double s1 = sat * adjust;
        double s2 = 1.0 - ((1.0 - sat) * isat);

        sat = fmin2(s1, s2);
    }
    if (sat > 1)
        sat = 1.0;
    return sat;
}

/* The lightness curve, interpolated. */
static double lightness(const struct gpcolor_lut *lut, double l)
{
    double pos = l * 256;
    int i = (int)pos;

    if (i >= 256)
        return lut->brightness[256];
    if (i < 0)
        return lut->brightness[0];
    return lut->brightness[i] + (lut->brightness[i + 1] - lut->brightness[i]) * (pos - i);
}

void gpcolor_apply(const struct gpcolor_lut *lut, const unsigned char *in, unsigned char *out,
    size_t pixels)
{
    unsigned char last_in[3] = { 0, 0, 0 }, last_out[3] = { 0, 0, 0 };
    int have_last = 0;

    while (pixels-- > 0)
    {
        unsigned char c[3];
        int i;

        if (have_last && in[0] == last_in[0] && in[1] == last_in[1] && in[2] == last_in[2])
        {
            memcpy(out, last_out, 3);
            in += 3;
            out += 3;
            continue;
        }
        memcpy(last_in, in, 3);
        for (i = 0; i < 3; i++)
            c[i] = lut->contrast[in[i]];
        if (lut->do_hsl)
        {
            double rgb[3], h, s, l;

            for (i = 0; i < 3; i++)
                rgb[i] = c[i] / 255.0;
            rgb_to_hsl(rgb, &h, &s, &l);
            {
                double nl = lightness(lut, l);

                if (nl < l && l < 1.0)
                    s = s * (1.0 - l) / (1.0 - nl);
                l = nl;
            }
            s = update_saturation(s, lut->ssat, lut->isat);
            if (lut->split_saturation && s > 0)
                s = update_saturation(s, lut->ssat, lut->isat);
            hsl_to_rgb(rgb, h, s, l);
            for (i = 0; i < 3; i++)
                c[i] = to_byte(rgb[i]);
        }
        for (i = 0; i < 3; i++)
            out[i] = lut->gamma[c[i]];
        memcpy(last_out, out, 3);
        have_last = 1;
        in += 3;
        out += 3;
    }
}
