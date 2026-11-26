#!/usr/bin/env python3
"""Build an uncompressed CPython/Django import archive for the maiOs ext4."""

from __future__ import annotations

import argparse
import gzip
import pathlib
import zipfile


EXCLUDED_PARTS = {"__pycache__", "test", "tests", "idlelib", "tkinter", "turtledemo"}


def archive_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o100644 << 16
    return info


def add_stdlib(archive: zipfile.ZipFile, root: pathlib.Path, overlay: pathlib.Path | None) -> None:
    for path in sorted(root.rglob("*")):
        if not path.is_file() or any(part in EXCLUDED_PARTS for part in path.relative_to(root).parts):
            continue
        if overlay is not None and path.relative_to(root).as_posix() == "socket.py":
            continue
        if path.suffix in {".pyc", ".pyo"}:
            continue
        archive.writestr(archive_info(path.relative_to(root).as_posix()), path.read_bytes())


def add_overlay(archive: zipfile.ZipFile, root: pathlib.Path) -> None:
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix not in {".pyc", ".pyo"}:
            archive.writestr(archive_info(path.relative_to(root).as_posix()), path.read_bytes())


def add_wheel(archive: zipfile.ZipFile, wheel: pathlib.Path) -> None:
    with zipfile.ZipFile(wheel) as source:
        for name in sorted(source.namelist()):
            if name.endswith("/") or ".dist-info/" in name or "__pycache__" in name:
                continue
            if name.endswith((".pyc", ".pyo")):
                continue
            contents = source.read(name)
            archive.writestr(archive_info(name), contents)
            if name == "django/contrib/auth/common-passwords.txt.gz":
                archive.writestr(
                    archive_info("django/contrib/auth/common-passwords.txt"),
                    gzip.decompress(contents),
                )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("stdlib", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("wheels", nargs="+", type=pathlib.Path)
    parser.add_argument("--overlay", type=pathlib.Path)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", allowZip64=True) as archive:
        add_stdlib(archive, args.stdlib, args.overlay)
        if args.overlay:
            add_overlay(archive, args.overlay)
        for wheel in args.wheels:
            add_wheel(archive, wheel)


if __name__ == "__main__":
    main()
