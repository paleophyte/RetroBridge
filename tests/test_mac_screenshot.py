"""Run production Mac screenshot capture/encoding against synthetic PixMaps."""
from pathlib import Path
import re
import unittest

from test_session_timeouts import compile_run
from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "agent-mac-system7/llm_agent.c").read_text(encoding="utf-8")
CODE = "\n".join(re.findall(r"^#define SCREENSHOT_.*$", SOURCE, re.M)) + "\n"
CODE += "\n".join(function(SOURCE, signature) for signature in (
    "static void PutLE32(", "static void PutLE16(", "static void GetPixelRGB(",
    "static int ScreenshotLayout(", "static int ScreenshotPixelsValid(",
    "static int SendScreenshotBMP(", "static void HandleScreenshot("))

STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { unsigned short red,green,blue; } RGBColor;
typedef struct { RGBColor rgb; } ColorSpec;
typedef struct { short ctSize; ColorSpec ctTable[256]; } ColorTable;
typedef ColorTable **CTabHandle;
typedef struct { short top,left,bottom,right; } Rect;
typedef struct { Rect bounds; unsigned short rowBytes; short pixelSize;
                 CTabHandle pmTable; char *baseAddr; } PixMap;
typedef PixMap **PixMapHandle;
typedef struct { PixMapHandle gdPMap; } Device;
typedef Device **GDHandle;
typedef void *GWorldPtr;
typedef void *CGrafPtr;
typedef int QDErr;
typedef PixMap BitMap;
#define noErr 0
#define srcCopy 0
static ColorTable colors, *colorsPtr=&colors;
static PixMap screen, pixels, *screenPtr=&screen, *pixelsPtr=&pixels;
static Device device={&screenPtr}, *devicePtr=&device;
static unsigned char sourceBytes[400000], wire[400000];
static long used, fail_at=-1, largest;
static int calls_after_failure, failed, fault, created, disposed, locked, unlocked, copied, port_changes;
static int session_failed;
static void net_fail(void) { session_failed=1; }
static int SendAll(const char *p,long n) {
    long take=n;
    if (failed) { calls_after_failure++; return -1; }
    if (n>largest) largest=n;
    if (fail_at>=0 && used+n>fail_at) take=fail_at-used;
    assert(take>=0 && used+take<(long)sizeof(wire));
    memcpy(wire+used,p,(size_t)take);used+=take;
    if (take!=n) { failed=1; return -1; }
    return 0;
}
static int SendCStr(const char *s) { return SendAll(s,(long)strlen(s)); }
static GDHandle GetMainDevice(void) { return fault==1?NULL:&devicePtr; }
static void GetGWorld(CGrafPtr *p,GDHandle *d) { *p=(void *)1;*d=&devicePtr; }
static QDErr NewGWorld(GWorldPtr *g,int depth,Rect *r,void *t,GDHandle d,int flags) {
    (void)depth;(void)t;(void)d;(void)flags;created++;
    if(fault==2)return -108;
    *g=(void *)2;pixels.bounds=*r;
    if(fault==9)pixels.bounds.right--;
    return 0;
}
static PixMapHandle GetGWorldPixMap(GWorldPtr p) { (void)p;return fault==3?NULL:&pixelsPtr; }
static int LockPixels(PixMapHandle p) { (void)p;if(fault==4)return 0;locked++;return 1; }
static void UnlockPixels(PixMapHandle p) { (void)p;assert(locked==1 && !unlocked);unlocked++; }
static void DisposeGWorld(GWorldPtr p) { assert(p==(void *)2 && created==1 && !disposed);disposed++; }
static void SetGWorld(CGrafPtr p,GDHandle d) {
    (void)d;port_changes++;assert(p==(port_changes==1?(void *)2:(void *)1));
}
static void CopyBits(BitMap *a,BitMap *b,Rect *r,Rect *s,int mode,void *mask) {
    (void)a;(void)b;(void)r;(void)s;(void)mode;(void)mask;
    assert(locked && !unlocked);copied++;
}
static unsigned long le32(const unsigned char *p) {
    return (unsigned long)p[0]|((unsigned long)p[1]<<8)|((unsigned long)p[2]<<16)|((unsigned long)p[3]<<24);
}
static void reset(int width,int height,int depth) {
    int i;used=largest=0;failed=calls_after_failure=session_failed=0;fail_at=-1;
    created=disposed=locked=unlocked=copied=port_changes=0;
    memset(&pixels,0,sizeof(pixels));memset(sourceBytes,0xcc,sizeof(sourceBytes));
    screen.bounds.left=7;screen.bounds.top=-3;
    screen.bounds.right=(short)(7+width);screen.bounds.bottom=(short)(-3+height);
    pixels.pixelSize=(short)depth;pixels.rowBytes=(unsigned short)(((width*depth+7)/8+3)&~3);
    pixels.baseAddr=(char *)sourceBytes;pixels.pmTable=&colorsPtr;colors.ctSize=255;
    for(i=0;i<256;i++){
        colors.ctTable[i].rgb.red=(unsigned short)(i<<8);
        colors.ctTable[i].rgb.green=(unsigned short)((255-i)<<8);
        colors.ctTable[i].rgb.blue=(unsigned short)((i^85)<<8);
    }
}
static void seed(int width,int height,int depth) {
    int x,y;
    for(y=0;y<height;y++)for(x=0;x<width;x++) {
        unsigned char *row=sourceBytes+(long)y*pixels.rowBytes;
        if(depth<=8) {
            int mask=(1<<depth)-1,shift=8-depth-((x*depth)%8);
            row[(x*depth)/8]=(unsigned char)((row[(x*depth)/8]&~(mask<<shift))|(((x+y)&mask)<<shift));
        } else if(depth==16) {
            unsigned v=((x&31)<<10)|((y&31)<<5)|((x+y)&31);
            row[x*2]=(unsigned char)(v>>8);row[x*2+1]=(unsigned char)v;
        } else {
            row[x*4]=0;row[x*4+1]=(unsigned char)x;row[x*4+2]=(unsigned char)y;row[x*4+3]=(unsigned char)(x+y);
        }
    }
}
static void verify(int width,int height,int depth) {
    unsigned char *bmp=(unsigned char *)memchr(wire,'\n',(size_t)used)+1;
    unsigned char *data=bmp+54;
    long advertised=0, stride=(width*3+3)&~3L, x,y;
    assert(sscanf((char *)wire,"SIZE:%ld",&advertised)==1);
    assert(advertised==54+stride*height && used-(bmp-wire)==advertised);
    assert(bmp[0]=='B' && bmp[1]=='M' && le32(bmp+2)==(unsigned long)advertised);
    assert(le32(bmp+10)==54 && le32(bmp+14)==40 && le32(bmp+18)==(unsigned)width);
    assert(le32(bmp+22)==(unsigned)height && bmp[26]==1 && bmp[28]==24);
    assert(le32(bmp+34)==(unsigned long)(stride*height));
    for(y=0;y<height;y++) {
        long srcy=height-1-y;
        for(x=0;x<width;x++) {
            unsigned char r,g,b;
            if(depth<=8) { int v=(int)((x+srcy)&((1<<depth)-1));r=(unsigned char)v;g=(unsigned char)(255-v);b=(unsigned char)(v^85); }
            else if(depth==16) { r=(unsigned char)((x&31)*255/31);g=(unsigned char)((srcy&31)*255/31);b=(unsigned char)(((x+srcy)&31)*255/31); }
            else { r=(unsigned char)x;g=(unsigned char)srcy;b=(unsigned char)(x+srcy); }
            assert(data[y*stride+x*3]==b && data[y*stride+x*3+1]==g && data[y*stride+x*3+2]==r);
        }
        for(x=width*3;x<stride;x++)assert(data[y*stride+x]==0);
    }
}
'''


class MacScreenshotTests(unittest.TestCase):
    def test_wide_images_depths_padding_and_persistent_framing(self):
        compile_run(STUBS + CODE + r'''
int main(void) {
    int widths[]={1,2,3,4,511,512,513,2047,2048,2049,2053,4093};
    int depths[]={1,2,4,8,16,32};unsigned i,j;long end;
    fault=0;
    for(i=0;i<sizeof(widths)/sizeof(widths[0]);i++)for(j=0;j<sizeof(depths)/sizeof(depths[0]);j++) {
        reset(widths[i],3,depths[j]);seed(widths[i],3,depths[j]);
        HandleScreenshot();verify(widths[i],3,depths[j]);
        assert(disposed==1 && unlocked==1 && copied==1 && port_changes==2);
        assert(largest<=SCREENSHOT_CHUNK_PIXELS*3 && !session_failed);
        end=used;assert(SendCStr("PONG\n")==0);assert(!memcmp(wire+end,"PONG\n",5));
    }
    return 0;
}
''')

    def test_capture_failures_and_size_limits_before_size_header(self):
        compile_run(STUBS + CODE + r'''
int main(void) {
    int i;long stride,size;
    for(i=1;i<=9;i++) {
        fault=i;reset(2053,3,8);
        if(i==5)pixels.baseAddr=NULL;
        if(i==6)pixels.pixelSize=24;
        if(i==7)pixels.rowBytes=1;
        if(i==8)pixels.pmTable=NULL;
        HandleScreenshot();assert(used>4 && !memcmp(wire,"ERR:",4));
        assert(!copied && !port_changes);
        assert(disposed==(i>=3) && unlocked==(i>=5));
    }
    assert(ScreenshotLayout(0,1,&stride,&size)<0);
    assert(ScreenshotLayout(1,-1,&stride,&size)<0);
    assert(ScreenshotLayout(65535,1,&stride,&size)<0);
    assert(ScreenshotLayout(32767,32767,&stride,&size)<0);
    assert(ScreenshotLayout(1,(64L*1024*1024-54)/4,&stride,&size)<0);
    assert(ScreenshotLayout(4096,100,&stride,&size)==0 && size==54+4096L*3*100);
    fault=0;reset(1,1,8);screen.bounds.left=-32768;screen.bounds.right=32767;
    HandleScreenshot();assert(!created && !memcmp(wire,"ERR:",4));
    return 0;
}
''')

    def test_disconnect_stops_output_and_releases_capture(self):
        compile_run(STUBS + CODE + r'''
int main(void) {
    long limit,total;
    fault=0;reset(2053,2,32);seed(2053,2,32);HandleScreenshot();total=used;
    for(limit=0;limit<total;limit+=137) {
        reset(2053,2,32);seed(2053,2,32);fail_at=limit;
        HandleScreenshot();assert(failed && session_failed && used==limit && !calls_after_failure);
        assert(disposed==1 && unlocked==1 && port_changes==2);
    }
    return 0;
}
''')
