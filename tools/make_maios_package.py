#!/usr/bin/env python3
"""Create a single-file maiOS package archive for pacman -U."""

import argparse
import re
from pathlib import Path


COMPONENT = re.compile(r"[A-Za-z0-9_.-]{1,63}\Z")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("version")
    parser.add_argument("payload", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    for label, value in (("name", args.name), ("version", args.version),
                         ("payload filename", args.payload.name)):
        if not COMPONENT.fullmatch(value):
            parser.error(f"{label} must be 1 to 63 letters, digits, '.', '_' or '-'")
    header = f"MAIOSPKG1 {args.name} {args.version} {args.payload.name}\n".encode("ascii")
    with args.output.open("wb") as package:
        package.write(header)
        with args.payload.open("rb") as source:
            while chunk := source.read(65536):
                package.write(chunk)


if __name__ == "__main__":
    main()
