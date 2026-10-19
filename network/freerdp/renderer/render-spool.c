/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "render-spool.h"
#include <stdio.h>
#include <string.h>
/* Only the exact generated RAM: paths are eligible for automatic cleanup. */
int render_spool_complete(const char *path, int result)
{
    const char *prefix="RAM:RemoteDesktopJobs/";
    if (strncmp(path,prefix,strlen(prefix))) return 0;
    const char *name=path+strlen(prefix);
    size_t n=strlen(name);
    if (n<5 || strcmp(name+n-4,".xps")) return 0;
    for (size_t i=0;i<n-4;i++)
        if (!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f') || name[i]=='-')) return 0;
    /* A terminal status is published before releasing the queue slot. */
    char status[256];
    if (snprintf(status,sizeof(status),"%s.status",path)>=(int)sizeof(status)) return 0;
    FILE *f=fopen(status,"w"); if (!f) return 0;
    int ok=fprintf(f,"result=%d\n",result)>0;
    if (fclose(f)) ok=0;
    if (ok) remove(path);
    return ok;
}
