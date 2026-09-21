# Agent screenshot gallery

Captured on September 21, 2026 from 13 distinct running OS/version combinations.
Every image came from the native agent's authenticated `SCREENSHOT` command,
using the same Python client as the MCP bridge. The returned BMPs were converted
losslessly to PNG. No hypervisor screenshots, generated scenes, crops or
retouching are included. These are running lab VMs; this gallery is not evidence
of testing the same installations on physical hardware.

[Capture metadata and image hashes](screenshots/manifest.json) record dimensions,
times and available agent build identities. Private inventory files and tokens
are excluded. Actual non-secret lab names/addresses can appear in the images.

The NetWare capture and Win16 input bugs found during this session were fixed
and tested before this gallery was published; see the
[publication audit](PUBLICATION_AUDIT.md#follow-up-agent-screenshot-gallery-and-win16-input-2026-09-21).
Capability differences and remaining limitations are documented in
[MCP coverage](MCP_COVERAGE.md) and the platform READMEs.

The Ubuntu/QEMU host is inventory-only, so it is not included as a supported
agent platform. The MS-DOS 6.22/WFW dual-boot machine was running WFW; no separate
bare-DOS 6.22 capture was taken. Other advertised Windows versions without a
running guest in this inventory are not pictured.

Screenshots depict third-party operating systems and applications; their names,
logos and interfaces are not part of RetroBridge's MIT-licensed source code.

## FreeDOS 1.4

The DOS agent renders the live text console into a bitmap; this is not a GUI framebuffer.

![FreeDOS 1.4 captured through the native agent](screenshots/freedos-1.4.png)

## Windows for Workgroups 3.11

Notepad text was entered through the corrected Win16 agent. The guest reports Windows API version 3.10; this installation is Windows for Workgroups 3.11.

![Windows for Workgroups 3.11 captured through the native agent](screenshots/windows-for-workgroups-3.11.png)

## Windows 95

One of the two running Windows 95 guests; duplicate OS installations are represented once.

![Windows 95 captured through the native agent](screenshots/windows-95.png)

## Windows NT 4.0

Windows NT 4.0 SP6 with a legacy Selsius administration page open.

![Windows NT 4.0 captured through the native agent](screenshots/windows-nt-4.png)

## Windows 2000 Server

The console was locked. Capturing it does not imply the agent can bypass Windows logon.

![Windows 2000 Server captured through the native agent](screenshots/windows-2000-server.png)

## Windows XP

Windows XP SP2 with a legacy softphone application open.

![Windows XP captured through the native agent](screenshots/windows-xp.png)

## Windows 7

Windows 7 SP1, with Notepad launched and text entered through the interactive agent.

![Windows 7 captured through the native agent](screenshots/windows-7.png)

## OS/2 1.3

The 16-bit OS/2 agent captures Presentation Manager.

![OS/2 1.3 captured through the native agent](screenshots/os2-1.3.png)

## OS/2 2.11

OS/2 2.11; the command interpreter identifies itself as version 2.1.

![OS/2 2.11 captured through the native agent](screenshots/os2-2.11.png)

## OS/2 4.50

The guest reports kernel version 4.50, despite an older inventory nickname.

![OS/2 4.50 captured through the native agent](screenshots/os2-4.50.png)

## Mac OS System 7.5.3

Finder on System 7.5.3, captured by the 68k MacTCP agent.

![Mac OS System 7.5.3 captured through the native agent](screenshots/system-7.5.3.png)

## NetWare 3.12

Live text-console cells rendered by the fixed NetWare 3.12 agent, including its verified update log.

![NetWare 3.12 captured through the native agent](screenshots/netware-3.12.png)

## NetWare 4.11

Live text-console cells rendered by the fixed NetWare 4.11 agent, including its verified update log.

![NetWare 4.11 captured through the native agent](screenshots/netware-4.11.png)
