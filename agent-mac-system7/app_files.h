/* Application-relative files for System 7. No volume or launch-directory names. */
#ifndef MAC_APP_FILES_H
#define MAC_APP_FILES_H

static FSSpec gApplicationSpec;
static int gApplicationLocated;

static OSErr MacLocateApplication(void)
{
    ProcessSerialNumber self;
    ProcessInfoRec info;
    OSErr err;
    gApplicationLocated = 0;
    memset(&gApplicationSpec, 0, sizeof(gApplicationSpec));
    memset(&info, 0, sizeof(info));
    err = GetCurrentProcess(&self);
    if (err != noErr) return err;
    info.processInfoLength = sizeof(info);
    info.processAppSpec = &gApplicationSpec;
    err = GetProcessInformation(&self, &info);
    if (err != noErr) return err;
    if (!gApplicationSpec.vRefNum || gApplicationSpec.parID <= 0 ||
        !gApplicationSpec.name[0] || gApplicationSpec.name[0] > 63) return paramErr;
    gApplicationLocated = 1;
    return noErr;
}

static OSErr MacSelectApplicationDirectory(void)
{
    WDPBRec pb;
    if (!gApplicationLocated) return paramErr;
    memset(&pb, 0, sizeof(pb));
    pb.ioVRefNum = gApplicationSpec.vRefNum;
    pb.ioWDDirID = gApplicationSpec.parID;
    return PBHSetVolSync(&pb);
}

static OSErr MacApplicationFile(const char *name, FSSpec *spec)
{
    unsigned char pname[32];
    size_t len = strlen(name);
    if (!gApplicationLocated || !len || len > 31 || strchr(name, ':')) return paramErr;
    pname[0] = (unsigned char)len;
    memcpy(pname + 1, name, len);
    return FSMakeFSSpec(gApplicationSpec.vRefNum, gApplicationSpec.parID, pname, spec);
}

static int MacApplicationHasName(const char *name)
{
    size_t i, len = strlen(name);
    if (!gApplicationLocated || len != gApplicationSpec.name[0]) return 0;
    for (i = 0; i < len; i++) {
        unsigned char a = gApplicationSpec.name[i + 1], b = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

/* Best-effort diagnostics beside the executable; never log configuration values. */
static void MacAppendLog(const char *name, const char *message)
{
    FSSpec spec;
    short ref;
    long count;
    OSErr err = MacApplicationFile(name, &spec);
    if (err == fnfErr) err = FSpCreate(&spec, 0x3F3F3F3FUL, 0x54455854UL, smSystemScript);
    if (err != noErr || FSpOpenDF(&spec, fsWrPerm, &ref) != noErr) return;
    if (SetFPos(ref, fsFromLEOF, 0) == noErr) {
        count = (long)strlen(message);
        if (FSWrite(ref, &count, message) == noErr) {
            count = 1;
            FSWrite(ref, &count, "\r");
        }
    }
    FSClose(ref);
    FlushVol(NULL, spec.vRefNum);
}
#endif
