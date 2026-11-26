# The controller's own web page

From firmware 0.3.2 the controller serves the same dashboard itself. Type its IP address in a browser on the LAN, for example `http://192.168.0.206/`.

The top bar turns green: **Live · IRIV-IOC_886257 · 192.168.0.206 · v0.3.3**.

## What it does

| Page | On the controller |
| --- | --- |
| Overview | Live DI0–DI10 and DO0–DO3, AN0/AN1 in your units, buzzer, MQTT and Modbus state, every 2 s |
| Channels | The controller's own settings, with live states |
| Connections | Edit MQTT, Ethernet, Modbus, RS485, then **Save to controller** (top right). It restarts with the new settings. |
| RS485 points | **Test read** a sensor over the LAN; the table shows live values or "no reply" |
| Install | The controller check (live inputs), **Test outputs**, **Beep and blink**, **Update from GitHub** |

**Update from GitHub** downloads the newest firmware in your browser and sends it to the controller, which checks it (MD5) and installs it. It never offers an older version than the one running.

## Web password

- **Looking** is open to anyone on the LAN.
- **Changing** anything (Save to controller, Update from GitHub, Test outputs, Beep and blink) needs the **web password**. The user name is `admin`.
- Set it in the dashboard under **Connections → Web page**, then send the settings **over USB once** with **Include the passwords** ticked.
- Without a password the page is read-only and says so.
- Saved passwords (web and MQTT) are never sent to the page. On the controller's page, leave a password box empty to keep the saved one.

The page is plain `http`. The password itself is not sent in the clear (HTTP digest), but the traffic is not encrypted: use it on a trusted LAN.

## Limits

- The first install on a new controller still needs USB from the website.
- Outputs are not switched from this page: use SCADA (Modbus) or MQTT.
- Browsers only allow USB on secure (`https`) pages, so USB features live on the website, not here.

## For integrators: the API

| Request | Password | Does |
| --- | --- | --- |
| `GET /api/status` | no | Status plus live `io` (`di`, `do`, `an`, `count`, `buzzer`) |
| `GET /api/selftest` | no | Hardware self-test |
| `GET /api/config` | no | Saved settings, passwords removed |
| `GET /api/rs485/read?slave=&fn=&reg=&count=&baud=&parity=&stop=&timeout=` | no | Reads one RS485 register once |
| `GET /api/login` | yes | Checks the password |
| `POST /api/config` | yes | Saves settings, then restarts |
| `POST /api/update?size=&md5=` | yes | Firmware `.bin` as a multipart file |
| `POST /api/testoutputs`, `POST /api/identify` | yes | Output running light, beep and blink |
