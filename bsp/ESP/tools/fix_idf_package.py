#!/usr/bin/env python3
#
# File      : fix_idf_package.py
# This file is part of RT-Thread RTOS
#
# SPDX-License-Identifier: Apache-2.0
#
# Re-apply the local patches this BSP needs on top of the downloaded
# ESP-IDF package.  Run it after `pkgs --update` (or any time the
# packages/ESP-IDF-latest tree has been refreshed):
#
#     python3 bsp/ESP/tools/fix_idf_package.py
#
# The script is idempotent: patches that are already in place are skipped,
# and a patch whose anchor cannot be found (because the package moved on)
# is reported as an error so it does not silently regress.
#

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Patch definitions
# ---------------------------------------------------------------------------
#
# Each entry:
#   file    : path relative to the ESP-IDF package root
#   probe   : text that, when present, means "already patched"
#   apply   : callable(text) -> text, raising ValueError when the anchor is gone
#   why     : short reason printed to the user
#

def _patch_vectors_no_gp_restore(text):
    """ESP-IDF's RT-Thread interrupt vector restores x3/gp from the stack
    frame. RT-Thread's RISC-V port never saves gp there (rt_hw_context_switch
    leaves slot 3 untouched and rt_hw_stack_init fills it with 0xdeadbeef), so
    a thread resumed from an ISR would continue with a garbage global pointer.
    Keep the current gp instead."""
    pattern = re.compile(
        r'^([ \t]*)LOAD[ \t]+x3,[ \t]*3[ \t]*\*[ \t]*REGBYTES\(sp\)[^\n]*\n',
        re.MULTILINE,
    )
    match = pattern.search(text)
    if match is None:
        raise ValueError("'LOAD x3, 3 * REGBYTES(sp)' not found")

    indent = match.group(1)
    # Drop any trailing /* ... */ so the replacement does not nest comments.
    instruction = match.group(0).split("/*")[0].strip()
    replacement = (
        "%s/*\n"
        "%s * gp (x3) is not part of RT-Thread's context frame: the port's\n"
        "%s * rt_hw_context_switch() never writes slot 3 and rt_hw_stack_init()\n"
        "%s * leaves it as 0xdeadbeef. Restoring it here hands a garbage global\n"
        "%s * pointer to every thread resumed from an interrupt, so keep the\n"
        "%s * current gp instead.\n"
        "%s */\n"
        "%s/* %s */\n"
    ) % ((indent,) * 7 + (indent, instruction))
    return text[:match.start()] + replacement + text[match.end():]


PATCHES = [
    {
        "file": "components/riscv/vectors_rtthread.S",
        "probe": "is not part of RT-Thread's context frame",
        "apply": _patch_vectors_no_gp_restore,
        "why": "do not restore gp from RT-Thread context frames",
    },
]

# Debug prints accidentally left in the package by earlier debugging sessions.
# They are not fatal, only noisy; report them instead of editing silently.
LEFTOVER_DEBUG = [
    ("components/spi_flash/cache_utils.c", r"\[dbg\]"),
    ("components/spi_flash/esp_flash_api.c", r"\[dbg\]"),
    ("components/spi_flash/esp_flash_spi_init.c", r"\[dbg\]"),
    ("components/spi_flash/spi_flash_os_func_app.c", r"\[dbg\]"),
    ("components/esp_hal_mspi/spi_flash_hal_common.inc", r"\[dbg\]"),
]


def default_package_path():
    here = os.path.dirname(os.path.abspath(__file__))
    # bsp/ESP/tools -> bsp/ESP
    esp_dir = os.path.dirname(here)
    return os.path.join(esp_dir, "ESP32_C3", "packages", "ESP-IDF-latest")


def main():
    parser = argparse.ArgumentParser(
        description="Re-apply the local ESP-IDF package patches for ESP32-C3.")
    parser.add_argument("--package", default=None,
                        help="path to the ESP-IDF package (default: "
                             "bsp/ESP/ESP32_C3/packages/ESP-IDF-latest)")
    parser.add_argument("--check", action="store_true",
                        help="only report status, do not modify files")
    args = parser.parse_args()

    package = args.package or default_package_path()
    if not os.path.isdir(package):
        print("error: package directory not found: %s" % package)
        print("       run `pkgs --update` in the BSP directory first")
        return 1

    print("package: %s" % package)

    failures = 0
    changes = 0

    for patch in PATCHES:
        path = os.path.join(package, patch["file"])
        if not os.path.isfile(path):
            print("  [FAIL] %s: file missing" % patch["file"])
            failures += 1
            continue

        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()

        if patch["probe"] in text:
            print("  [ ok ] %s: already patched" % patch["file"])
            continue

        if args.check:
            print("  [todo] %s: needs patch (%s)" % (patch["file"], patch["why"]))
            continue

        try:
            patched = patch["apply"](text)
        except ValueError as exc:
            print("  [FAIL] %s: %s" % (patch["file"], exc))
            print("         the package changed upstream, update this script")
            failures += 1
            continue

        with open(path, "w", encoding="utf-8") as handle:
            handle.write(patched)
        print("  [done] %s: %s" % (patch["file"], patch["why"]))
        changes += 1

    for rel, pattern in LEFTOVER_DEBUG:
        path = os.path.join(package, rel)
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            hits = len(re.findall(pattern, handle.read()))
        if hits:
            print("  [warn] %s: %d leftover debug print(s)" % (rel, hits))

    print("")
    if failures:
        print("%d patch(es) failed" % failures)
        return 1

    if changes:
        print("%d file(s) patched, rebuild with: scons -j8" % changes)
    else:
        print("nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
