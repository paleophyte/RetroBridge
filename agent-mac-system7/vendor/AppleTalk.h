/* Minimal stub -- MacTCP.h only needs the AddrBlock type from the real
 * AppleTalk.h (for the nbp_entry ARP-cache-adjacent struct it defines but
 * that this agent never uses). We don't have Apple's real AppleTalk.h and
 * don't need actual AppleTalk/EtherTalk support, so this just satisfies
 * the type dependency.
 */
#ifndef __APPLETALK__
#define __APPLETALK__

#ifndef __TYPES__
#include <Types.h>
#endif

typedef UInt16 NetNumber;
typedef UInt8 NodeID;
typedef UInt8 SocketNumber;

struct AddrBlock {
    NetNumber aNet;
    NodeID aNode;
    SocketNumber aSocket;
};
typedef struct AddrBlock AddrBlock;

#endif /* __APPLETALK__ */
