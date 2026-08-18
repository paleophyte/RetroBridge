"""Loads named legacy-machine configs (host/ports/token) from an ini file.

Lets one bridge process serve multiple legacy boxes - each MCP tool call
names which configured machine to target instead of the bridge being
wired to a single machine via environment variables.
"""

from __future__ import annotations

import configparser
from dataclasses import dataclass
from pathlib import Path


class MachineConfigError(RuntimeError):
    pass


@dataclass
class MachineConfig:
    name: str
    host: str
    exec_port: int = 2222
    exec_token: str = ""
    vnc_port: int = 5900
    vnc_password: str | None = None


def load_machines(path: Path) -> dict[str, MachineConfig]:
    if not path.exists():
        raise MachineConfigError(
            f"machine config file not found: {path}\n"
            f"Copy machines.ini.example to {path.name} and fill in your machines."
        )

    parser = configparser.ConfigParser()
    parser.read(path, encoding="utf-8")

    machines: dict[str, MachineConfig] = {}
    for name in parser.sections():
        section = parser[name]
        host = section.get("host")
        if not host:
            raise MachineConfigError(f"[{name}] in {path} is missing 'host'")
        token = section.get("exec_token", "")
        if not token:
            raise MachineConfigError(f"[{name}] in {path} is missing 'exec_token'")
        machines[name] = MachineConfig(
            name=name,
            host=host,
            exec_port=section.getint("exec_port", fallback=2222),
            exec_token=token,
            vnc_port=section.getint("vnc_port", fallback=5900),
            vnc_password=section.get("vnc_password") or None,
        )
    return machines
