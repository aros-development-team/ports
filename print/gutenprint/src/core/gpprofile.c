/*
 * gpprofile - printer profiles and their printer.device unit bindings.
 * Copyright (C) 2026 terminills
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gpprofile.h"

/* Copies src, cut to fit size. */
static void copy(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);

    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Profile names may hold any character; file names may not hold ':' or '/'. */
static void profile_path(const char *dir, const char *name, char *path, size_t size)
{
    char file[64];
    size_t i;

    copy(file, sizeof(file), name);
    for (i = 0; file[i] != '\0'; i++)
        if (file[i] == ':' || file[i] == '/' || file[i] == '"')
            file[i] = '_';
    snprintf(path, size, "%s/Printers/%s", dir, file);
}

static void make_dirs(const char *dir)
{
    char sub[160];

    mkdir(dir, 0755);
    snprintf(sub, sizeof(sub), "%s/Printers", dir);
    mkdir(sub, 0755);
}

const char *gpprofile_get(const struct gp_profile *p, const char *name)
{
    int i;

    for (i = 0; i < p->noptions; i++)
        if (strcmp(p->opt_name[i], name) == 0)
            return p->opt_value[i];
    return NULL;
}

int gpprofile_set(struct gp_profile *p, const char *name, const char *value)
{
    int i;

    for (i = 0; i < p->noptions; i++)
        if (strcmp(p->opt_name[i], name) == 0)
            break;
    if (value == NULL)
    {
        if (i < p->noptions)
        {
            p->noptions--;
            memmove(p->opt_name[i], p->opt_name[i + 1], (p->noptions - i) * sizeof(p->opt_name[0]));
            memmove(p->opt_value[i], p->opt_value[i + 1], (p->noptions - i) * sizeof(p->opt_value[0]));
        }
        return 0;
    }
    if (i == p->noptions)
    {
        if (p->noptions == GP_MAX_OPTIONS)
            return 1;
        p->noptions++;
        copy(p->opt_name[i], sizeof(p->opt_name[i]), name);
    }
    copy(p->opt_value[i], sizeof(p->opt_value[i]), value);
    return 0;
}

static int load_file(const char *path, struct gp_profile *p)
{
    FILE *f = fopen(path, "r");
    char line[400];

    if (f == NULL)
        return 1;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *value = strchr(line, '=');

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || value == NULL)
            continue;
        *value++ = '\0';
        if (strcmp(line, "Version") == 0)
            continue;
        else if (strcmp(line, "Name") == 0)
            copy(p->name, sizeof(p->name), value);
        else if (strcmp(line, "Model") == 0 || strcmp(line, "Printer") == 0)
            copy(p->model, sizeof(p->model), value);
        else if (strcmp(line, "Connection") == 0)
            copy(p->connection, sizeof(p->connection), value);
        else if (strcmp(line, "DeviceID") == 0)
            copy(p->device_id, sizeof(p->device_id), value);
        else if (strcmp(line, "Picture") == 0)
            copy(p->picture, sizeof(p->picture), value);
        else if (strcmp(line, "Preview") == 0)
            p->preview = strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
        else if (strncmp(line, "Option.", 7) == 0)
            gpprofile_set(p, line + 7, value);
        else if (p->nextra < GP_MAX_EXTRA)
            snprintf(p->extra[p->nextra++], sizeof(p->extra[0]), "%.40s=%.110s", line, value);
    }
    fclose(f);
    return 0;
}

int gpprofile_load(const char *dir, const char *name, struct gp_profile *p)
{
    char path[200];

    memset(p, 0, sizeof(*p));
    copy(p->name, sizeof(p->name), name);
    profile_path(dir, name, path, sizeof(path));
    return load_file(path, p);
}

static int copy_file(const char *from, const char *to)
{
    char buf[4096];
    FILE *in = fopen(from, "rb"), *out;
    size_t n;
    int err = 0;

    if (in == NULL)
        return 1;
    out = fopen(to, "wb");
    if (out == NULL)
    {
        fclose(in);
        return 1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            err = 1;
    fclose(in);
    if (fclose(out) != 0)
        err = 1;
    return err;
}

int gpprofile_save(const char *dir, const struct gp_profile *p)
{
    char path[200], tmp[210];
    FILE *f;
    int i;

    make_dirs(dir);
    profile_path(dir, p->name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    f = fopen(tmp, "w");
    if (f == NULL)
        return 1;
    fprintf(f, "Version=1\nName=%s\nModel=%s\n", p->name, p->model);
    if (p->picture[0] != '\0')
    {
        char pic[210];

        snprintf(pic, sizeof(pic), "%s.png", path);
        if (strcmp(pic, p->picture) == 0 || copy_file(p->picture, pic) == 0)
            fprintf(f, "Picture=%s\n", pic);
    }
    if (p->connection[0] != '\0')
        fprintf(f, "Connection=%s\n", p->connection);
    if (p->device_id[0] != '\0')
        fprintf(f, "DeviceID=%s\n", p->device_id);
    if (p->preview)
        fprintf(f, "Preview=yes\n");
    for (i = 0; i < p->noptions; i++)
        fprintf(f, "Option.%s=%s\n", p->opt_name[i], p->opt_value[i]);
    for (i = 0; i < p->nextra; i++)
        fprintf(f, "%s\n", p->extra[i]);
    if (fclose(f) != 0)
    {
        remove(tmp);
        return 1;
    }
    remove(path);
    return rename(tmp, path) != 0;
}

int gpprofile_delete(const char *dir, const char *name)
{
    char path[200];

    profile_path(dir, name, path, sizeof(path));
    strcat(path, ".png");
    remove(path);
    path[strlen(path) - 4] = '\0';
    return remove(path) != 0;
}

int gpprofile_list(const char *dir, char names[][64], int max)
{
    char sub[160];
    DIR *d;
    struct dirent *e;
    int n = 0;

    snprintf(sub, sizeof(sub), "%s/Printers", dir);
    d = opendir(sub);
    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL && n < max)
    {
        struct gp_profile *p;
        char path[sizeof(sub) + sizeof(e->d_name) + 2];

        if (e->d_name[0] == '.' || strstr(e->d_name, ".new") != NULL || strstr(e->d_name, ".png") != NULL)
            continue;
        p = calloc(1, sizeof(*p));
        if (p == NULL)
            break;
        snprintf(path, sizeof(path), "%s/%s", sub, e->d_name);
        if (load_file(path, p) == 0)
            copy(names[n++], 64, p->name[0] != '\0' ? p->name : e->d_name);
        free(p);
    }
    closedir(d);
    return n;
}

int gpunits_load(const char *dir, char units[GP_UNITS][64])
{
    char path[160], line[128];
    FILE *f;
    int i;

    for (i = 0; i < GP_UNITS; i++)
        units[i][0] = '\0';
    snprintf(path, sizeof(path), "%s/Units", dir);
    f = fopen(path, "r");
    if (f == NULL)
        return 1;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] >= '0' && line[0] <= '9' && line[1] == '=')
            copy(units[line[0] - '0'], 64, line + 2);
    }
    fclose(f);
    return 0;
}

int gpunits_save(const char *dir, char units[GP_UNITS][64])
{
    char path[160];
    FILE *f;
    int i;

    make_dirs(dir);
    snprintf(path, sizeof(path), "%s/Units", dir);
    f = fopen(path, "w");
    if (f == NULL)
        return 1;
    for (i = 0; i < GP_UNITS; i++)
        if (units[i][0] != '\0')
            fprintf(f, "%d=%s\n", i, units[i]);
    return fclose(f) != 0;
}

int gpprofile_for_unit(int unit, struct gp_profile *p, char *why, size_t why_size)
{
    char units[GP_UNITS][64];

    if (unit < 0 || unit >= GP_UNITS)
    {
        snprintf(why, why_size, "printer.device unit %d is out of range", unit);
        return 1;
    }
    if (gpunits_load(GP_ENV_DIR, units) != 0)
    {
        /* Before profiles: one Settings file, for unit 0. */
        memset(p, 0, sizeof(*p));
        copy(p->name, sizeof(p->name), "Settings");
        if (unit == 0 && load_file(GP_ENV_DIR "/Settings", p) == 0 && p->model[0] != '\0')
        {
            /* Its options were plain Name=value lines. */
            int i;

            for (i = 0; i < p->nextra; i++)
            {
                char *value = strchr(p->extra[i], '=');

                *value++ = '\0';
                gpprofile_set(p, p->extra[i], value);
            }
            p->nextra = 0;
            return 0;
        }
        snprintf(why, why_size, "no printer is set up for unit %d (" GP_ENV_DIR "/Units)", unit);
        return 1;
    }
    if (units[unit][0] == '\0')
    {
        snprintf(why, why_size, "no printer is bound to unit %d in " GP_ENV_DIR "/Units", unit);
        return 1;
    }
    if (gpprofile_load(GP_ENV_DIR, units[unit], p) != 0 || p->model[0] == '\0')
    {
        snprintf(why, why_size, "printer '%s' for unit %d has no model", units[unit], unit);
        return 1;
    }
    return 0;
}

void gpprofile_options(const struct gp_profile *p, const char *opts[GP_MAX_OPTIONS + 1],
    char buf[GP_MAX_OPTIONS][130])
{
    int i;

    for (i = 0; i < p->noptions; i++)
    {
        snprintf(buf[i], sizeof(buf[i]), "%s=%s", p->opt_name[i], p->opt_value[i]);
        opts[i] = buf[i];
    }
    opts[i] = NULL;
}
