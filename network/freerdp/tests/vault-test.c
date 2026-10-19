/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vault.h"
int main(void)
{
    char dir[]="/tmp/adt-vault-test-XXXXXX",path[128],backup[140];assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/profile.conf",dir);snprintf(backup,sizeof(backup),"%s.bak",path);
    struct adt_profile p={0},q={0};strcpy(p.host,"fixture:3392");strcpy(p.user,"fixture");
    char password[512],first[1200];
    assert(adt_password_seal(&p,"owned test master","fixture-password",p.password_blob));
    strcpy(first,p.password_blob);assert(!strstr(first,"fixture-password"));
    assert(adt_password_open(&p,"owned test master",password));assert(!strcmp(password,"fixture-password"));
    assert(!adt_password_open(&p,"wrong master",password) && !*password);
    assert(adt_password_seal(&p,"owned test master","fixture-password",p.password_blob));assert(strcmp(first,p.password_blob));
    q=p;strcpy(q.host,"different-server");assert(!adt_password_open(&q,"owned test master",password));
    q=p;strcpy(q.user,"different-user");assert(!adt_password_open(&q,"owned test master",password));
    q=p;q.password_blob[70]=q.password_blob[70]=='a'?'b':'a';assert(!adt_password_open(&q,"owned test master",password));
    q=p;q.password_blob[98]=0;assert(!adt_password_open(&q,"owned test master",password));
    assert(adt_profile_save(path,&p));assert(adt_profile_load(path,&q));
    assert(adt_password_open(&q,"owned test master",password) && !strcmp(password,"fixture-password"));
    memset(password,'z',511);password[511]=0;
    assert(adt_password_seal(&p,"owned test master",password,p.password_blob));
    assert(adt_profile_save(path,&p));assert(adt_profile_load(path,&q));
    assert(adt_password_open(&q,"owned test master",password) && strlen(password)==511);
    assert(adt_password_seal(&p,"owned test master","",p.password_blob));
    assert(adt_password_open(&p,"owned test master",password) && !*password);
    q.password_blob[0]=0;assert(adt_profile_save(path,&q));assert(access(backup,F_OK));
    assert(!adt_password_seal(&p,"","password",p.password_blob));
    remove(path);remove(backup);rmdir(dir);
    puts("PASS encrypted profile roundtrip, wrong key, tampering, endpoint binding, random nonces, max/empty password, missing master");
}
