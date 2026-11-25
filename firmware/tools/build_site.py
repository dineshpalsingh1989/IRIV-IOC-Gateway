"""Assembles the GitHub Pages site: dashboard + installer + firmware.

Usage (after `pio run -d firmware`):  python firmware/tools/build_site.py [output_dir]
Writes output_dir/firmware/iriv-ioc-gateway.uf2 and manifest.json (version, size, SHA-256),
which the dashboard's Install page reads.
"""
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
UF2 = os.path.join(ROOT, "firmware", ".pio", "build", "cytron_iriv_io_controller", "firmware.uf2")
SITE_FILES = ["index.html", "app.js", "install.js", "styles.css"]
FIRMWARE_NAME = "iriv-ioc-gateway.uf2"


def firmware_version():
    with open(os.path.join(ROOT, "firmware", "src", "register_map.h"), encoding="utf-8") as f:
        header = f.read()
    text = re.search(r'#define GATEWAY_VERSION_TEXT "([^"]+)"', header).group(1)
    parts = [re.search(rf"#define GATEWAY_VERSION_{p} (\d+)", header).group(1) for p in ("MAJOR", "MINOR", "PATCH")]
    if text != ".".join(parts):
        sys.exit(f"register_map.h: GATEWAY_VERSION_TEXT {text} does not match {'.'.join(parts)}")
    return text


# UF2 family IDs (from the UF2 spec / pico-sdk). The bootloader rejects a file for another chip.
UF2_FAMILIES = {0xE48BFF56: "RP2040", 0xE48BFF59: "RP2350", 0xE48BFF5A: "RP2350", 0xE48BFF5B: "RP2350"}


def uf2_chip(data):
    """Chip the .uf2 is built for, from its family ID; every block must agree."""
    families = set()
    for offset in range(0, len(data), 512):
        magic0, magic1, flags = struct.unpack_from("<3I", data, offset)
        if (magic0, magic1) != (0x0A324655, 0x9E5D5157):
            sys.exit(f"Not a UF2 file (bad block at {offset})")
        if flags & 0x2000:  # family ID present
            families.add(struct.unpack_from("<I", data, offset + 28)[0])
    chips = {UF2_FAMILIES.get(f, f"unknown 0x{f:08x}") for f in families}
    if len(chips) != 1 or next(iter(chips)).startswith("unknown"):
        sys.exit(f"UF2 has unexpected families: {sorted(hex(f) for f in families)}")
    return next(iter(chips)), sorted(families)


def commit():
    if os.environ.get("GITHUB_SHA"):
        return os.environ["GITHUB_SHA"][:12]
    try:
        return subprocess.check_output(["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "local"


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "site"))
    if not os.path.isfile(UF2):
        sys.exit(f"{UF2} not found: run `pio run -d firmware` first")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "firmware"))
    for name in SITE_FILES:
        shutil.copy2(os.path.join(ROOT, name), out)
    shutil.copytree(os.path.join(ROOT, "docs"), os.path.join(out, "docs"))
    open(os.path.join(out, ".nojekyll"), "w").close()  # serve files as they are

    with open(UF2, "rb") as f:
        data = f.read()
    with open(os.path.join(out, "firmware", FIRMWARE_NAME), "wb") as f:
        f.write(data)
    chip, families = uf2_chip(data)
    build = {
        "chip": chip,                 # what the install-mode drive reports (INFO_UF2.TXT)
        "variant": "RP2350A",         # what the running firmware reports (package register)
        "board": "cytron_iriv_io_controller",
        "boardName": "Cytron IRIV IO Controller",
        "uf2Families": [f"0x{f:08x}" for f in families],
        "file": FIRMWARE_NAME,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    manifest = {
        "product": "iriv-ioc-gateway",
        "version": firmware_version(),
        "commit": commit(),
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        # One entry per processor/board; the installer picks the one matching the connected device.
        "builds": [build],
        # Single-build fields kept for older installer pages.
        "board": build["board"], "file": build["file"], "size": build["size"], "sha256": build["sha256"],
    }
    with open(os.path.join(out, "firmware", "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"Site in {out}: firmware {manifest['version']}, {len(data)} bytes, sha256 {manifest['sha256'][:16]}...")


if __name__ == "__main__":
    main()
