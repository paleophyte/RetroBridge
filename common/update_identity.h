/* Startup-only executable identity for update verification. C89 / 16-bit safe.
 * This fingerprints the file at startup, not a live memory image or signature.
 * Failure leaves the hash empty; the bridge must not verify an unknown image.
 */
#ifndef RETRO_UPDATE_IDENTITY_H
#define RETRO_UPDATE_IDENTITY_H
#include <stdio.h>
#include <string.h>

#include "update_sha256.h"

static int update_file_sha256(const char *path, char *hex) {
    UpdateSHA256 state;
    unsigned char block[64];
    size_t n;
    int failed;
    FILE *f;
    hex[0]='\0';
    f=fopen(path,"rb");
    if (!f) return -1;
    update_sha_init(&state);
    while ((n=fread(block,1,sizeof(block),f))!=0) {
        if(update_sha_add(&state,block,(unsigned)n)!=0) {fclose(f);return -1;}
    }
    failed=ferror(f);
    if(fclose(f)!=0 || failed)return -1;
    update_sha_finish(&state,hex);
    return 0;
}

static char g_update_exe[260];
static char g_update_sha256[65];
static char g_update_started[48];
static void update_identity_init(const char *path, unsigned long ticks, unsigned long process) {
    if (!path || !path[0] || strlen(path)>=sizeof(g_update_exe)) return;
    strcpy(g_update_exe,path);
    /* Compared only within a quiet, >=10-second update. Collisions fail
       closed. This is an instance marker, not a secret or authentication. */
    update_hex32(g_update_started,ticks);
    g_update_started[8]='-';
    update_hex32(g_update_started+9,process);
    update_file_sha256(path,g_update_sha256);
}
#endif
