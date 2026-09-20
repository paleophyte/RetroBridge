"""Find user-supplied hex signatures in a local RAM dump.

Usage: python findsig.py ram.bin signatures.json
The JSON file maps descriptive names to hex byte strings. No guest memory,
ROM, or proprietary application signatures are supplied by this project.
"""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("signatures", type=Path)
    args = parser.parse_args()
    ram = args.dump.read_bytes()
    signatures = json.loads(args.signatures.read_text(encoding="utf-8"))
    for name, value in signatures.items():
        signature = bytes.fromhex(value)
        if not signature:
            parser.error("Empty signature: " + name)
        start = 0
        while True:
            hit = ram.find(signature, start)
            if hit < 0:
                break
            print(f"{name}: 0x{hit:08X}")
            start = hit + 1


if __name__ == "__main__":
    main()
