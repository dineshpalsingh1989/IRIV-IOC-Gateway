"""Checks the controller's web page and API (firmware 0.3.2+). Usage: webtest.py <ip> <web password> [firmware.bin]
Switches DO0-DO3 briefly (output test). With a .bin it also sends an update with a wrong MD5 (must be refused),
then the real one (the controller restarts with it)."""
import gzip, hashlib, sys, time
import requests
from requests.auth import HTTPDigestAuth

IP, PASSWORD = sys.argv[1], sys.argv[2]
BASE, AUTH, results = f"http://{IP}", HTTPDigestAuth("admin", PASSWORD), []

def check(name, ok, detail=""):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}  {detail}", flush=True)

for path, marker in [("/", b"IRIV"), ("/app.js", b"connectDevice"), ("/styles.css", b".mode-pill"), ("/install.js", b"startWebInstall")]:
    r = requests.get(BASE + path, timeout=5)
    check(f"GET {path}", r.ok and marker in r.content, f"{r.status_code}, {len(r.content)} bytes, gzip={r.headers.get('Content-Encoding') == 'gzip'}")
s = requests.get(BASE + "/api/status", timeout=5).json()
check("GET /api/status", s.get("type") == "status" and len(s["io"]["di"]) == 11 and len(s["io"]["do"]) == 4,
      f"v{s['version']} ip {s['ip']} di {''.join(str(int(x)) for x in s['io']['di'])} an {s['io']['an']} webPassword={s['webPassword']}")
t = requests.get(BASE + "/api/selftest", timeout=5).json()
check("GET /api/selftest", t.get("type") == "selftest" and t["ethernetChip"], f"rtc {t['rtc']['state']}, {t['temperature']:.1f} °C")
c = requests.get(BASE + "/api/config", timeout=5)
cfg = c.json()
check("GET /api/config hides passwords", c.ok and not cfg.get("mqtt", {}).get("pass") and "web" not in cfg, f"mqtt {cfg['mqtt']['host']}:{cfg['mqtt']['port']}")
check("No password -> 401", requests.post(BASE + "/api/testoutputs", timeout=5).status_code == 401)
check("Wrong password -> 401", requests.get(BASE + "/api/login", auth=HTTPDigestAuth("admin", "wrong"), timeout=5).status_code == 401)
check("Right password -> login ok", requests.get(BASE + "/api/login", auth=AUTH, timeout=5).ok)
r = requests.post(BASE + "/api/testoutputs", auth=AUTH, timeout=5)
check("Output test (DO0-DO3 running light)", r.ok, r.text)
time.sleep(3.5)
check("Outputs back off after the test", requests.get(BASE + "/api/status", timeout=5).json()["io"]["do"] == [False] * 4)

if len(sys.argv) > 3:
    data = open(sys.argv[3], "rb").read()
    files = {"firmware": ("fw.bin", data, "application/octet-stream")}
    r = requests.post(f"{BASE}/api/update?size={len(data)}&md5={'0' * 32}", files=files, auth=AUTH, timeout=60)
    check("Update with wrong MD5 refused", r.status_code == 400, r.text)
    check("Still running after the refusal", requests.get(BASE + "/api/status", timeout=5).ok)
    r = requests.post(f"{BASE}/api/update?size={len(data)}&md5={hashlib.md5(data).hexdigest()}", files=files, auth=AUTH, timeout=60)
    check("Update accepted", r.ok, r.text)
    for _ in range(30):
        time.sleep(2)
        try: s = requests.get(BASE + "/api/status", timeout=3).json(); break
        except Exception: s = None
    check("Back online after the update", s is not None, f"v{s['version']}, up {s['uptime']} s" if s else "")
print(f"\n{sum(results)}/{len(results)} passed")
