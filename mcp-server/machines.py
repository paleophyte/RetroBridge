"""Loads named legacy-machine configs (host/ports/token) from an ini file.

Lets one MCP server process serve multiple legacy boxes - each MCP tool call
names which configured machine to target instead of the server being
wired to a single machine via environment variables.
"""

from __future__ import annotations

import configparser
from dataclasses import dataclass
from pathlib import Path
from text_codec import normalize_encoding


class MachineConfigError(RuntimeError):
    pass


@dataclass
class MachineConfig:
    name: str
    host: str
    exec_port: int = 2222
    exec_token: str = ""
    vm_name: str | None = None
    agent_enabled: bool = True
    max_command_bytes: int = 510
    max_response_bytes: int = 64 * 1024 * 1024
    text_encoding: str = "ascii"
    exec_encoding: str | None = None
    exec_command_encoding: str | None = None
    file_encoding: str | None = None


def load_machines(path: Path) -> dict[str, MachineConfig]:
    if not path.exists():
        raise MachineConfigError(
            f"machine config file not found: {path}\n"
            f"Copy machines.ini.example to {path.name} and fill in your machines."
        )

    # Tokens and ancillary SSH settings are literal, not interpolation templates.
    parser = configparser.ConfigParser(interpolation=None)
    try:
        with path.open(encoding="utf-8-sig") as config:
            parser.read_file(config)
    except (configparser.Error, UnicodeError, OSError) as e:
        # Parser errors may embed entire lines containing credentials.
        raise MachineConfigError(f"cannot read machine config {path} ({type(e).__name__})") from None

    machines: dict[str, MachineConfig] = {}
    for name in parser.sections():
        section = parser[name]
        host = section.get("host")
        if not host:
            raise MachineConfigError(f"[{name}] in {path} is missing 'host'")
        try:
            agent_enabled = section.getboolean("agent_enabled", fallback=True)
        except ValueError:
            raise MachineConfigError(f"[{name}] in {path} has invalid 'agent_enabled' (use true or false)") from None
        token = ""
        port = 2222
        limits = {"max_command_bytes": 510, "max_response_bytes": 64 * 1024 * 1024}
        encodings = {"text_encoding": "ascii", "exec_encoding": "ascii", "exec_command_encoding": "ascii", "file_encoding": "ascii"}
        if agent_enabled:
            token = section.get("exec_token", "")
            if not token:
                raise MachineConfigError(f"[{name}] in {path} is missing 'exec_token'")
            try:
                port = section.getint("exec_port", fallback=2222)
            except ValueError:
                raise MachineConfigError(f"[{name}] in {path} has invalid 'exec_port' (use 1-65535)") from None
            if not 1 <= port <= 65535:
                raise MachineConfigError(f"[{name}] in {path} has invalid 'exec_port' (use 1-65535)")
            for key, maximum in (("max_command_bytes", 4094), ("max_response_bytes", 2147483647)):
                try:
                    value = section.getint(key, fallback=limits[key])
                except ValueError:
                    raise MachineConfigError(f"[{name}] in {path} has invalid '{key}'") from None
                if not 1 <= value <= maximum:
                    raise MachineConfigError(f"[{name}] in {path} has invalid '{key}' (use 1-{maximum})")
                limits[key] = value
            for key in encodings:
                try:
                    encodings[key] = normalize_encoding(
                        section.get(key, encodings["text_encoding"]), output=key == "exec_encoding")
                except ValueError:
                    raise MachineConfigError(f"[{name}] in {path} has invalid '{key}'; see docs/TEXT_ENCODINGS.md") from None
        machines[name] = MachineConfig(
            name=name,
            host=host,
            exec_port=port,
            exec_token=token,
            vm_name=section.get("vm_name"),
            agent_enabled=agent_enabled,
            **limits,
            **encodings,
        )
    return machines
