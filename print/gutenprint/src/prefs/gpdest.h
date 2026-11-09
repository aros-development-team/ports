/*
 * gpdest - where a printer.device unit prints, for GutenprintPrefs.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * The same settings Printer prefs shows: the output device in the unit's
 * printer preferences (SYS/printer.prefs, SYS/printer<n>.prefs), and that
 * device's own settings (PRD_GETDESTINATION): where a print spooler queue
 * sends its jobs, and which network printer an ipp.device unit prints to.
 */
#ifndef GPDEST_H
#define GPDEST_H

#include <stddef.h>

struct gpdest
{
    char port[64];          /* the output device, without ".device" (printer.prefs) */
    int port_unit;
    int has_sends;          /* the port is a spooler: where its queue sends the jobs */
    char sends[64];
    int sends_unit;
    int has_printer;        /* a unit with a network printer: the port's own, or the spooler's target */
    char printer_dev[64];
    int printer_unit;
    char uri[256];
    char name[256];
    char uuid[64];
    char format[256];
};

/* Reads unit's settings (ENV:, then ENVARC:); a unit with no preferences prints to parallel 0. */
void gpdest_load(int unit, struct gpdest *d);

/* Re-reads the settings of the port device (and of the device its queue sends to) for d's port. */
void gpdest_follow(struct gpdest *d);

/* Re-reads the network printer of the device d's queue sends to. */
void gpdest_follow_sends(struct gpdest *d);

/* Writes what differs from was (everything with save, ENVARC: too). 0 on success;
   otherwise a reason in why. */
int gpdest_store(int unit, const struct gpdest *now, const struct gpdest *was, int save,
    char *why, size_t why_size);

/* The usual chain for a network printer: printer.device unit to printspool.device unit, whose
   queue sends to ipp.device unit, which prints to uri. */
void gpdest_network(struct gpdest *d, int unit, const char *uri, const char *name, const char *uuid,
    const char *format);

/* The driver unit's preferences name, "" when there are none. */
void gpdest_driver(int unit, char *driver, size_t size);

#endif
