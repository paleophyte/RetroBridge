# Documentation guide

Start with the [project README](../README.md) for installation, private machine
inventory, and MCP client setup. [Screenshots](SCREENSHOTS.md) show live captures
from the agents. Platform support differs; use the appropriate platform guide
before deploying a binary or calling a tool.

## Platform guides

| Target | Build, deployment, and platform limits |
| --- | --- |
| Windows 95/98/ME and NT-family Windows | [Root Win32 walkthrough](../README.md#1-build-a-target-agent-win32-walkthrough), [architecture](ARCHITECTURE.md) |
| DOS / FreeDOS | [DOS agent](../agent-dos/README.md) |
| Windows for Workgroups 3.11 | [Win16 agent](../agent-win16/README.md) |
| OS/2 2.x and later tested kernels | [32-bit OS/2 agent](../agent-os2/README.md) |
| OS/2 1.3 | [16-bit OS/2 agent](../agent-os2-13/README.md) |
| NetWare 3.12 / 4.11 | [NetWare agent and recovery](../agent-netware/README.md), [4.x build](../agent-netware/nw4/README.md) |
| Classic Mac OS System 7 | [68k Mac agent](../agent-mac-system7/README.md) |

## Shared behavior and development

- [All-agent deployment ISO](BUILD_ISO.md): compile every production agent and
  helper into local CD media, including NetWare NLM loading instructions.
- [Architecture and protocol](ARCHITECTURE.md): components, Win32 design,
  desktop sessions, trust model, file transfers, and update verification.
- [MCP coverage](MCP_COVERAGE.md): platform extensions and tool limitations.
- [Text encodings](TEXT_ENCODINGS.md): code-page configuration and byte limits.
- [Command framing](COMMAND_FRAMING.md) and [network deadlines](NETWORK_TIMEOUTS.md):
  raw-client requirements and stalled-connection behavior.
- [Long-running commands](LONG_RUNNING_COMMANDS.md): synchronous execution,
  tracked jobs, cancellation, output retention, and restart behavior.
- [DOS resident experiment](../agent-dos/experimental/README.md): deferred TSR
  concept, reproducible probe, failed live tests, and possible next work.
- [Validation and remaining issues](PUBLICATION_AUDIT.md): bounded test evidence
  and open work. Historical session narratives remain available in Git history.

## Publication and provenance

- [Licensing and dependency setup](../THIRD_PARTY.md): MIT scope, locally
  supplied SDKs, input manifests, and implementation provenance.
- [Source publication](PUBLICATION.md): checks for a source commit or archive.
- [Binary release review](BINARY_RELEASE.md): artifact-specific requirements,
  [recorded toolchain inventory](binary-provenance.json), and
  [collected third-party notices](binary-notices/README.md).

Build paths such as `C:\src\RetroBridge` are examples. Real tokens, machine
inventories, generated media, SDKs, and recovery files belong outside Git.
