/*
 * Minimal NetWare CLIB BSD-sockets + a few server APIs for llm_agent.
 * Avoids pulling the full NDK include tree into the compile.
 */
#ifndef NWSOCK_H
#define NWSOCK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  LONG;

#define AF_INET      2
#define SOCK_STREAM  1
#define INADDR_ANY   0UL
#define SOL_SOCKET   0xffff
#define SO_REUSEADDR 0x0004
#define FIONBIO      2  /* set/clear nonblocking I/O (sys/filio.h) */

struct in_addr {
    LONG s_addr;
};

struct sockaddr {
    WORD sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    WORD           sin_family;
    WORD           sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

#ifndef htons
#define htons(x) ((WORD)((((WORD)(x) & 0x00ffU) << 8) | \
                         (((WORD)(x) & 0xff00U) >> 8)))
#endif
#ifndef ntohs
#define ntohs(x) htons(x)
#endif

#pragma pack(push, 1)
typedef struct {
    char serverName[48];
    BYTE netwareVersion;
    BYTE netwareSubVersion;
    WORD maxConnectionsSupported;
    WORD connectionsInUse;
    WORD maxVolumesSupported;
    BYTE revisionLevel;
    BYTE SFTLevel;
    BYTE TTSLevel;
    WORD peakConnectionsUsed;
    BYTE accountingVersion;
    BYTE VAPversion;
    BYTE queingVersion;
    BYTE printServerVersion;
    BYTE virtualConsoleVersion;
    BYTE securityRestrictionLevel;
    BYTE internetBridgeSupport;
    BYTE reserved[60];
    BYTE CLibMajorVersion;
    BYTE CLibMinorVersion;
    BYTE CLibRevision;
} FILE_SERV_INFO;
#pragma pack(pop)

/* BSD sockets (CLIB exports; TCP/IP stack must be loaded). */
int socket(int domain, int type, int protocol);
int bind(int s, struct sockaddr *name, int namelen);
int listen(int s, int backlog);
int accept(int s, struct sockaddr *addr, int *addrlen);
int connect(int s, struct sockaddr *name, int namelen);
int send(int s, char *msg, int len, int flags);
int recv(int s, char *msg, int len, int flags);
int setsockopt(int s, int level, int name, char *val, int len);
int close(int fd);
int shutdown(int s, int how);
long GetCurrentTicks(void); /* CLIB uptime, approximately 18 ticks/second */
int ioctl(int fd, int command, ...);

/* Yield so we do not starve the NetWare OS. Blocking socket calls do not. */
void ThreadSwitch(void);
void ThreadSwitchWithDelay(void);

/* Console output (also used by printf under CLIB). */
void ConsolePrintf(const char *format, ...);

int system(const char *command);
int spawnlp(int mode, const char *path, const char *arg0, ...);
void delay(unsigned int milliseconds);
int rename(const char *old, const char *newname);
int remove(const char *path);
void AtUnload(void (*func)(void));

/* Hidden screen + console capture (never call DisplayScreen). */
int CreateScreen(const char *screenName, BYTE attr);
int DestroyScreen(int screenHandle);
void CopyFromScreenMemory(WORD height, WORD width, BYTE *Rect,
                          WORD beg_x, WORD beg_y);
int GetCurrentScreen(void);
int GetSizeOfScreen(WORD *heightP, WORD *widthP);
int GetScreenInfo(int handle, char *name, LONG *attr);
int SetCurrentScreen(int screenHandle);
int ScanScreens(int lastScreenID, char *name, LONG *attr);
int DisplayScreen(int screenHandle);
int CheckIfScreenDisplayed(int screenHandle, long waitFlag);
int ungetch(int charToPushBack);
void exit(int status);
void *memcpy(void *dest, const void *src, unsigned n);

/* Server control */
int DownFileServer(int forceFlag);

int GetServerInformation(int returnSize, FILE_SERV_INFO *serverInfo);
int GetVolumeInfoWithNumber(
    BYTE volumeNumber,
    char *volumeName,
    WORD *totalBlocks,
    WORD *sectorsPerBlock,
    WORD *availableBlocks,
    WORD *totalDirectorySlots,
    WORD *availableDirectorySlots,
    WORD *volumeIsRemovable
);

#ifdef __cplusplus
}
#endif

#endif /* NWSOCK_H */
