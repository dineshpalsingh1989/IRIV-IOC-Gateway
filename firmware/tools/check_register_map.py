"""Checks that the dashboard (app.js) and the firmware (src/register_map.h) use the same
Modbus TCP address map. Run from anywhere:  python firmware/tools/check_register_map.py"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, "..", "src", "register_map.h")
DASHBOARD = os.path.join(HERE, "..", "..", "app.js")


def main():
    with open(FIRMWARE, encoding="utf-8") as f:
        header = f.read()
    with open(DASHBOARD, encoding="utf-8") as f:
        js = f.read()
    firmware = {k: int(v, 0) for k, v in re.findall(r"const uint16_t (\w+) = (0x[0-9A-Fa-f]+|\d+);", header)}
    block = re.search(r"const MB = \{(.*?)\};", js, re.S)
    if not block:
        sys.exit("app.js: `const MB = {...}` not found")
    dashboard = {k: int(v, 16) for k, v in re.findall(r"(\w+): (0x[0-9A-Fa-f]+)", block.group(1))}
    dashboard["RS485_REGS"] = int(re.search(r"const RS485_REGS = (\d+);", js).group(1))

    problems = [f"{k}: firmware {firmware.get(k)!r} vs dashboard {dashboard.get(k)!r}"
                for k in sorted(set(firmware) | set(dashboard)) if firmware.get(k) != dashboard.get(k)]
    if problems:
        print("Register maps differ:\n  " + "\n  ".join(problems))
        sys.exit(1)
    print(f"Register maps match ({len(firmware)} entries)")


if __name__ == "__main__":
    main()
