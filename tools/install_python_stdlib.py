#!/usr/bin/env python3
"""Install the generated Python archive into an ext4 image with debugfs."""

from __future__ import annotations

import argparse
import subprocess


def run(*command: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=check)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("archive")
    parser.add_argument("--demo", help="install the Django demo entry point as /django")
    args = parser.parse_args()

    existing = run("debugfs", "-R", "stat /python311.zip", args.image, check=False)
    if existing.returncode == 0:
        run("debugfs", "-w", "-R", "rm /python311.zip", args.image)
    run("debugfs", "-w", "-R", f"write {args.archive} /python311.zip", args.image)
    if args.demo:
        existing = run("debugfs", "-R", "stat /django", args.image, check=False)
        if existing.returncode == 0:
            run("debugfs", "-w", "-R", "rm /django", args.image)
        run("debugfs", "-w", "-R", f"write {args.demo} /django", args.image)


if __name__ == "__main__":
    main()
