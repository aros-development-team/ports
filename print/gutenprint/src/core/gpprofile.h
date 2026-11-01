/*
 * gpprofile - printer profiles and their printer.device unit bindings.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * A profile is one printer: Gutenprint model, connection and options, kept in
 * <dir>/Printers/<name> as Key=value lines. <dir>/Units binds printer.device
 * units to profiles ("0=Photo R2400"). <dir> is ENV:Gutenprint for the
 * settings in use and ENVARC:Gutenprint for the saved ones. A profile's
 * picture is kept beside it as <dir>/Printers/<name>.png.
 */
#ifndef GPPROFILE_H
#define GPPROFILE_H

#include <stddef.h>

#define GP_ENV_DIR       "ENV:Gutenprint"
#define GP_ENVARC_DIR    "ENVARC:Gutenprint"
#define GP_DATA_PATH     "ENVARC:Gutenprint/xml;GUTENPRINT:xml"
#define GP_MAX_OPTIONS   160
#define GP_MAX_EXTRA     32
#define GP_MAX_PROFILES  32
#define GP_UNITS         10

struct gp_profile
{
    char name[64];
    char model[64];
    char connection[256];
    char device_id[256];
    char picture[256];              /* the printer's picture of itself (PNG), or "" */
    int preview;                    /* show each page before it prints (Preview=yes) */
    int noptions;
    char opt_name[GP_MAX_OPTIONS][48];
    char opt_value[GP_MAX_OPTIONS][80];
    int nextra;                     /* keys this version does not know, kept on save */
    char extra[GP_MAX_EXTRA][160];
};

/* Option value or NULL. */
const char *gpprofile_get(const struct gp_profile *p, const char *name);
/* Sets (value non-NULL) or removes (NULL) an option; 0 on success. */
int gpprofile_set(struct gp_profile *p, const char *name, const char *value);

int gpprofile_load(const char *dir, const char *name, struct gp_profile *p);
int gpprofile_save(const char *dir, const struct gp_profile *p);
int gpprofile_delete(const char *dir, const char *name);
/* Profile names in dir, up to max; returns the count. */
int gpprofile_list(const char *dir, char names[][64], int max);

/* units[i] is the profile name bound to unit i, "" when none. */
int gpunits_load(const char *dir, char units[GP_UNITS][64]);
int gpunits_save(const char *dir, char units[GP_UNITS][64]);

/* The profile for a printer.device unit in ENV:. Settings written before
   profiles existed (ENV:Gutenprint/Settings) are used for unit 0 when there
   is no Units file. 0 on success; otherwise a reason in why. */
int gpprofile_for_unit(int unit, struct gp_profile *p, char *why, size_t why_size);

/* "Name=value" pointers into p for gpjob, NULL-terminated; buf holds the strings. */
void gpprofile_options(const struct gp_profile *p, const char *opts[GP_MAX_OPTIONS + 1],
    char buf[GP_MAX_OPTIONS][130]);

#endif
