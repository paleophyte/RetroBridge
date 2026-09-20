/* Startup-only executable identity for update verification. C89 / 16-bit safe.
 * This fingerprints the file at startup, not a live memory image or signature.
 * Failure leaves the hash empty; the bridge must not verify an unknown image.
 */
#ifndef RETRO_UPDATE_IDENTITY_H
#define RETRO_UPDATE_IDENTITY_H
#include <stdio.h>
#include <string.h>

#define UI32(x) ((x) & 0xffffffffUL)
#define UIR(x,n) UI32(((x) >> (n)) | ((x) << (32-(n))))
/* Avoid CRT formatted I/O: MinGW's prebuilt formatter requires newer CPUs
   than the Windows 95 agent supports. */
static void update_hex32(char *out, unsigned long value) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i=7;i>=0;i--) { out[i]=digits[value & 15]; value >>= 4; }
    out[8]='\0';
}
static void update_sha_block(unsigned long *h, const unsigned char *p) {
    static const unsigned long k[64] = {
        0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,
        0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
        0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,
        0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
        0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,
        0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
        0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,
        0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
        0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,
        0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
        0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,
        0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
        0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,
        0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
        0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
        0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
    };
    unsigned long w[64], a,b,c,d,e,f,g,z,t1,t2,s0,s1;
    int i;
    for (i=0;i<16;i++) w[i]=((unsigned long)p[i*4]<<24) |
        ((unsigned long)p[i*4+1]<<16) | ((unsigned long)p[i*4+2]<<8) | p[i*4+3];
    for (i=16;i<64;i++) {
        s0=UIR(w[i-15],7)^UIR(w[i-15],18)^(w[i-15]>>3);
        s1=UIR(w[i-2],17)^UIR(w[i-2],19)^(w[i-2]>>10);
        w[i]=UI32(w[i-16]+s0+w[i-7]+s1);
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; z=h[7];
    for (i=0;i<64;i++) {
        s1=UIR(e,6)^UIR(e,11)^UIR(e,25);
        t1=UI32(z+s1+((e&f)^((~e)&g))+k[i]+w[i]);
        s0=UIR(a,2)^UIR(a,13)^UIR(a,22);
        t2=UI32(s0+((a&b)^(a&c)^(b&c)));
        z=g; g=f; f=e; e=UI32(d+t1); d=c; c=b; b=a; a=UI32(t1+t2);
    }
    h[0]=UI32(h[0]+a); h[1]=UI32(h[1]+b); h[2]=UI32(h[2]+c); h[3]=UI32(h[3]+d);
    h[4]=UI32(h[4]+e); h[5]=UI32(h[5]+f); h[6]=UI32(h[6]+g); h[7]=UI32(h[7]+z);
}

static int update_file_sha256(const char *path, char *hex) {
    unsigned long h[8] = {0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,
        0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL};
    unsigned long bytes=0, low, high;
    unsigned char block[64];
    size_t n;
    int i, failed;
    FILE *f;
    hex[0]='\0';
    f=fopen(path,"rb");
    if (!f) return -1;
    while ((n=fread(block,1,sizeof(block),f))==sizeof(block)) {
        /* Agent uploads are limited to 64 MiB; reject counter overflow. */
        if (bytes>0xffffffbfUL) { fclose(f); return -1; }
        bytes+=64; update_sha_block(h,block);
    }
    failed=ferror(f);
    if (fclose(f)!=0 || failed || bytes>0xffffffffUL-(unsigned long)n) return -1;
    bytes+=(unsigned long)n;
    block[n++]=0x80;
    if (n>56) { memset(block+n,0,64-n); update_sha_block(h,block); n=0; }
    memset(block+n,0,56-n);
    high=bytes>>29; low=UI32(bytes<<3);
    for (i=0;i<4;i++) { block[59-i]=(unsigned char)(high>>(i*8)); block[63-i]=(unsigned char)(low>>(i*8)); }
    update_sha_block(h,block);
    for (i=0;i<8;i++) update_hex32(hex+i*8,h[i]);
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
#undef UI32
#undef UIR
#endif
