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
    manifest = {
        "product": "iriv-ioc-gateway",
        "board": "cytron_iriv_io_controller",
        "version": firmware_version(),
        "file": FIRMWARE_NAME,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "commit": commit(),
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    with open(os.path.join(out, "firmware", "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"Site in {out}: firmware {manifest['version']}, {len(data)} bytes, sha256 {manifest['sha256'][:16]}...")


if __name__ == "__main__":
    main()
