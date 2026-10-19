/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef ADT_RENDER_NATIVE_H
#define ADT_RENDER_NATIVE_H
struct render_printer;
struct render_printer *render_printer_open(void);
int render_printer_page(struct render_printer *, const unsigned char *, int, int, int, int);
void render_printer_close(struct render_printer *);
void render_printer_error(const char *);
#endif
