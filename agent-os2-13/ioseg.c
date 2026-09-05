/*
 * IOSEG.C / IOSEG.DLL - I/O-privileged (ring 2) port access for OS/2 1.x.
 *
 * Why this exists: OS/2 1.3 runs applications at ring 3 with IOPL 0, so a
 * plain IN/OUT in the agent general-protection-faults. The documented 1.x
 * escape hatch is an *I/O privilege segment*: a code segment marked IOPL
 * at link time, whose exported entry points OS/2 reaches through ring-2
 * call gates. Inside such a segment IN/OUT execute against real hardware.
 * Three things have to line up, and all three are here or in ioreset.c:
 *
 *   1. CONFIG.SYS has IOPL=YES (already true on the os2-13 box).
 *   2. This module's code segment is named IOSEG (wcc -nt=IOSEG) and the
 *      linker marks that one segment - and nothing else, not the C
 *      runtime - IOPL (wlink "segment 'IOSEG' iopl").
 *   3. Each entry below is exported with a parameter *byte count*, so the
 *      linker builds a call gate that copies that many bytes from the
 *      ring-3 stack to the ring-2 stack.
 *
 * (3) is why every entry is __pascal rather than Watcom's default
 * register-based convention: a call gate can only copy stack parameters,
 * and its callee-cleans-up "RETF n" is exactly what a gate return needs.
 * All parameters are USHORT so the byte counts stay even - the processor
 * copies words.
 *
 * The calling process must also have been granted each specific port with
 * DosPortAccess() first; the grant is per-process, so ioreset.c (or the
 * agent) does that before calling in here.
 *
 * Deliberately free of the C runtime, statics and OS/2 API calls: ring-2
 * code should touch nothing but the ports it was granted.
 */

typedef unsigned short USHORT;
typedef unsigned char  UCHAR;

extern UCHAR io_in8(USHORT port);
#pragma aux io_in8 =    \
    "in  al,dx"         \
    parm [dx]           \
    value [al]          \
    modify exact [al];

extern void io_out8(USHORT port, UCHAR val);
#pragma aux io_out8 =   \
    "out dx,al"         \
    parm [dx] [al]      \
    modify exact [];

/* Settling delay between back-to-back port writes. Meaningless against
 * emulated hardware, kept because the 8042 sequences below are the real
 * ISA-era ones and cost nothing. */
extern void io_wait(void);
#pragma aux io_wait = "nop" "nop" "nop" "nop" modify exact [];

#define IOAPI __far __pascal

/* i8042 status port bits (read from 0x64). */
#define KBD_STAT_OBF  0x01      /* output buffer full - byte waiting for us */
#define KBD_STAT_IBF  0x02      /* input buffer full - controller still busy */

/* Spin until the 8042 is ready for another command/data byte. Returns 0
 * if it never drained. static, so it stays inside IOSEG and is only ever
 * called from ring 2. */
static USHORT kbd_wait_ready(void)
{
    USHORT i;

    for (i = 0; i < 20000; i++) {
        if ((io_in8(0x64) & KBD_STAT_IBF) == 0)
            return 1;
        io_wait();
    }
    return 0;
}

/*
 * Raw single-port primitives, for probing from ring 3 without committing
 * to any particular reset sequence.
 */
USHORT IOAPI IOIN8(USHORT port)
{
    return (USHORT)io_in8(port);
}

USHORT IOAPI IOOUT8(USHORT port, USHORT val)
{
    io_out8(port, (UCHAR)val);
    return 0;
}

/*
 * Reset method 1: pulse the 8042's reset line (command 0xFE to port
 * 0x64). This is the mechanism a PC/AT actually uses for Ctrl-Alt-Del,
 * and what OS/2's own keyboard driver ends up doing.
 *
 * Every routine from here down returns 1 if it is still executing
 * afterwards, i.e. the reset did NOT take - a working reset never
 * returns at all.
 */
USHORT IOAPI IOKBDRESET(void)
{
    USHORT i;

    kbd_wait_ready();
    io_out8(0x64, 0xFE);
    for (i = 0; i < 30000; i++)
        io_wait();
    return 1;
}

/*
 * Reset method 2: System Control Port A (0x92), bit 0 = fast CPU reset.
 * The PS/2-era alternative to going through the 8042 at all, and one
 * every PC emulator implements. Read-modify-write leaving bit 1 (fast
 * A20) alone, since clearing A20 mid-flight would be its own disaster if
 * the reset then didn't happen.
 */
USHORT IOAPI IOPORT92RESET(void)
{
    UCHAR v;
    USHORT i;

    v = io_in8(0x92);
    io_out8(0x92, (UCHAR)((v & ~1) | 1));
    for (i = 0; i < 30000; i++)
        io_wait();
    return 1;
}

/*
 * Reset method 3: chipset reset control register (0xCF9), the ICH/PIIX
 * "RST_CNT" that Linux and SeaBIOS use. Arm with SYS_RST (0x02), then
 * trigger on RST_CPU's 0->1 edge (0x06). Predates nothing OS/2 1.3 would
 * ever use itself, but a hypervisor emulating a standard PC chipset
 * generally implements it faithfully regardless of guest.
 */
USHORT IOAPI IOCF9RESET(void)
{
    USHORT i;

    io_out8(0x0CF9, 0x02);
    io_wait();
    io_out8(0x0CF9, 0x06);
    for (i = 0; i < 30000; i++)
        io_wait();
    return 1;
}

/*
 * Reset method 4: synthesise a real Ctrl-Alt-Del. Command 0xD2 tells the
 * 8042 to push the next byte written to 0x60 into its *output* buffer, so
 * OS/2's keyboard interrupt handler reads it as a genuine keystroke off
 * the wire. Make codes only (Ctrl 0x1D, Alt 0x38, Del = extended 0xE0
 * 0x53); real hardware fires the moment Del's make code lands with Ctrl
 * and Alt already down, and never needs the break codes.
 *
 * Unlike the three above this doesn't reset anything itself - it asks
 * OS/2 to do the same orderly thing it does when a person presses the
 * keys, which is the behaviour that already works from the VMware
 * console.
 */
static void kbd_inject(UCHAR scancode)
{
    kbd_wait_ready();
    io_out8(0x64, 0xD2);
    kbd_wait_ready();
    io_out8(0x60, scancode);
}

USHORT IOAPI IOCAD(void)
{
    USHORT i;

    kbd_inject(0x1D);   /* Ctrl down */
    kbd_inject(0x38);   /* Alt down */
    kbd_inject(0xE0);   /* Del (extended prefix) */
    kbd_inject(0x53);   /* Del */
    for (i = 0; i < 30000; i++)
        io_wait();
    return 1;
}
