"""Send USB setup commands to the IOC and print the @IOC replies. Usage: usb.py COM11 HELLO SELFTEST ...
CONFIG:<file> sends a settings file (the controller restarts after saving)."""
import hashlib, json, sys, time, serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.2)
time.sleep(0.5); port.reset_input_buffer()

def reply(want, wait=4.0):
    end = time.time() + wait
    while time.time() < end:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith("@IOC "):
            msg = json.loads(line[5:])
            if msg.get("type") in want or msg.get("type") == "error": return msg
        elif line: print("  log:", line)
    return None

for cmd in sys.argv[2:]:
    if cmd.startswith("CONFIG:"):
        data = open(cmd[7:], "rb").read()
        port.write(f"CONFIG {len(data)}\n".encode()); time.sleep(0.1); port.write(data)
        print("CONFIG ->", json.dumps(reply({"saved"}, 6)))
    elif cmd.startswith("UPDATE:"):  # UPDATE:<firmware.bin>[:badmd5] installs firmware over USB (0.3.1+)
        bad = cmd.endswith(":badmd5"); path = cmd[7:].removesuffix(":badmd5")
        data = open(path, "rb").read()
        md5 = "0" * 32 if bad else hashlib.md5(data).hexdigest()
        port.write(f"UPDATE {len(data)} {md5}\n".encode()); time.sleep(0.1)
        for i in range(0, len(data), 512): port.write(data[i:i + 512])
        print("UPDATE ->", json.dumps(reply({"updated"}, 60)))
    elif cmd.startswith("TIME"):
        port.write(f"TIME {int(time.time())}\n".encode()); print("TIME ->", json.dumps(reply({"time"})))
    else:
        port.write((cmd + "\n").encode())
        print(cmd, "->", json.dumps(reply({cmd.lower()})))
port.close()
