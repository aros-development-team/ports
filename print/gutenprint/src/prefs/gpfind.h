/*
 * gpfind - network printers for GutenprintPrefs.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Finds the printers on the network with AROS's C:FindPrinters (DNS-SD, and
 * IPP for each printer's model, state, paper, supplies and picture), and sets
 * a printer.device unit up to print to one: ipp.device for the connection,
 * printspool.device in front of it, and the unit's printer prefs pointing at
 * the spooler and the Gutenprint driver.
 */
#ifndef GPFIND_H
#define GPFIND_H

#include <stddef.h>

#define GPFIND_MAX       16
#define GPFIND_SUPPLIES  8

struct gp_found
{
    char name[128];         /* the DNS-SD instance name */
    char uri[256];          /* ipp://... or socket://... */
    char formats[512];      /* document formats it takes */
    char model[128];        /* printer-make-and-model */
    char state[16];
    char reasons[160];      /* printer-state-reasons, comma separated */
    char message[128];      /* printer-state-message */
    char paper[96];         /* media-ready, PWG names */
    char picture[256];      /* its picture, a PNG in T: */
    int nsupplies;
    int level[GPFIND_SUPPLIES];         /* percent, -1 unknown */
    char colour[GPFIND_SUPPLIES][16];   /* #RRGGBB */
    char supply[GPFIND_SUPPLIES][48];
};

/* Looks for printers (a few seconds). Returns how many were found, -1 with a reason in why. */
int gpfind_scan(struct gp_found *found, int max, char *why, size_t why_size);

/* Asks one IPP printer for its state, paper, supplies and picture. 1 when it answered,
   0 when it did not, -1 with a reason in why. */
int gpfind_query(const char *uri, struct gp_found *found, char *why, size_t why_size);

/* printer-state-reasons as words for people: "toner-low-report,media-empty" ->
   "Toner low, out of paper"; "" for none. */
void gpfind_reasons_text(const char *reasons, char *out, size_t size);

/* The Gutenprint page size for a PWG media name ("na_letter_8.5x11in" -> "Letter"), or NULL. */
const char *gpfind_page_size(const char *pwg);

/* Sets printer.device unit to print to uri through printspool.device and ipp.device unit
   <unit>, as document format (application/postscript, ...). In ENV:, and ENVARC: too with
   save. 0 on success; otherwise a reason in why. */
int gpfind_setup_unit(int unit, const char *uri, const char *name, const char *format, int save,
    char *why, size_t why_size);

#endif
