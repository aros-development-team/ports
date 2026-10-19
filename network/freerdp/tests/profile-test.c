/* SPDX-License-Identifier: Apache-2.0 */
#include "profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static unsigned rename_calls, fail_rename;
int __real_rename(const char *,const char *);
int __wrap_rename(const char *a,const char *b) { if (++rename_calls==fail_rename) { errno=EIO; return -1; } return __real_rename(a,b); }
int main(void) {
 char path[]="/tmp/adt-rdp-profile-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd);
 struct adt_profile p={0}, q={0}; strcpy(p.host,"192.0.2.10:3389"); strcpy(p.user,"fixture");
 strcpy(p.options.directory,"RAM:Shared"); p.options.documents=1; p.options.sound=1; p.options.resize=1;
 assert(adt_profile_name_valid("Build_server-1")); assert(!adt_profile_name_valid("../secret"));
 assert(!adt_profile_name_valid("SYS:Prefs")); assert(!adt_profile_name_valid(""));
 assert(adt_profile_save(path,&p)); assert(adt_profile_load(path,&q)); assert(!memcmp(&p,&q,sizeof(p)));
 p.options.printer=1; assert(!adt_profile_save(path,&p)); p.options.printer=0;
 strcpy(p.domain,"domain"); assert(adt_profile_save(path,&p)); assert(adt_profile_load(path,&q));
 assert(!strcmp(q.domain,"domain"));
 strcpy(p.domain,"new-domain"); rename_calls=0; fail_rename=2;
 assert(!adt_profile_save(path,&p)); fail_rename=0;
 assert(adt_profile_load(path,&q)); assert(!strcmp(q.domain,"domain"));
 FILE *f=fopen(path,"ab"); assert(f); fputs("password=secret\n",f); fclose(f);
 struct adt_profile before=q; assert(!adt_profile_load(path,&q)); assert(!memcmp(&before,&q,sizeof(q)));
 strcpy(p.host,"server\npassword=secret"); assert(!adt_profile_save(path,&p));
 strcpy(p.host,"server"); strcpy(p.options.fingerprint,"invalid"); assert(!adt_profile_save(path,&p));
 memset(p.options.fingerprint,'a',64); p.options.fingerprint[64]=0; assert(adt_profile_save(path,&p));
 f=fopen(path,"ab"); assert(f); fputs("host=duplicate\n",f); fclose(f); assert(!adt_profile_load(path,&q));
 unlink(path); char backup[128]; snprintf(backup,sizeof(backup),"%s.bak",path); unlink(backup); puts("profile: PASS (no secrets, strict keys, atomic load, save rollback, bounds)"); return 0;
}
