// Browser installer for the IRIV IOC gateway (Install page).
// Firmware: a running gateway 0.3.1+ gets the .bin over USB (UPDATE command). Anything else: put the RP2350
// in install mode, then write the .uf2 onto its USB drive.
// Settings: send ioc_config.json over USB (Web Serial) using the firmware's line protocol:
//   HELLO | STATUS | CONFIG <bytes> + JSON | TIME <unix> | REBOOT | BOOTLOADER  ->  "@IOC {json}" replies.
// Uses app.js globals: $, icon, escapeHtml, toast, config, points, buildConfigFile, num.

const FIRMWARE_DIR = 'firmware/';
const hasSerial = 'serial' in navigator;
const hasDirectoryPicker = 'showDirectoryPicker' in window;
let manifest = null;   // firmware/manifest.json: version + one build per processor
let customFile = null; // { name, bytes } picked by the user
let link = null;       // open IocLink
let statusTimer = null;
let check = null;      // last controller check: { result (SELFTEST reply), at } or { unsupported: version }
const detected = {};   // what we learned about the connected device: usb, drive, hello, other (non-gateway firmware)

// Chip a .uf2 is built for, by its UF2 family ID. The bootloader ignores files for another chip.
const UF2_FAMILIES = { 0xe48bff56: 'RP2040', 0xe48bff59: 'RP2350', 0xe48bff5a: 'RP2350', 0xe48bff5b: 'RP2350' };
// USB vendor IDs seen on a serial port, to recognise what was plugged in.
const USB_VENDORS = {
  0x2e8a: { name: 'Raspberry Pi RP2040/RP2350', rp: true },
  0x239a: { name: 'Adafruit / CircuitPython', rp: true },
  0x303a: { name: 'Espressif ESP32' },
  0x10c4: { name: 'Silicon Labs CP210x USB-serial (often an ESP32 board)' },
  0x1a86: { name: 'WCH CH340 USB-serial (often an ESP32 or Arduino board)' },
  0x0403: { name: 'FTDI USB-serial adapter' },
  0x2341: { name: 'Arduino' }
};
const hex4 = n => n.toString(16).toUpperCase().padStart(4, '0');
// Compares "major.minor.patch" texts: negative when a is older than b.
const cmpVersion = (a, b) => { const x = String(a).split('.').map(Number), y = String(b).split('.').map(Number); for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) - (y[i] || 0); return 0; };

function usbInfo(port) {
  const { usbVendorId: vid, usbProductId: pid } = port.getInfo?.() || {};
  if (vid === undefined) return null; // e.g. a Bluetooth serial port
  return { vid, pid, ...(USB_VENDORS[vid] || { name: 'Unknown USB device' }) };
}

// Reads the chip from a .uf2 file: every block carries the same family ID.
function uf2Chip(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 512 || view.getUint32(0, true) !== 0x0a324655 || view.getUint32(4, true) !== 0x9e5d5157) return null;
  const flags = view.getUint32(8, true);
  return flags & 0x2000 ? UF2_FAMILIES[view.getUint32(28, true)] || `unknown family 0x${view.getUint32(28, true).toString(16)}` : 'no family ID';
}

// INFO_UF2.TXT on an install-mode drive names the chip, e.g. "Model: Raspberry Pi RP2350 / Board-ID: RP2350".
function parseDriveInfo(text) {
  const model = /Model:\s*(.+)/i.exec(text)?.[1].trim() || '';
  const boardId = /Board-ID:\s*(.+)/i.exec(text)?.[1].trim() || '';
  const both = `${model} ${boardId}`;
  const chip = /RP2350/i.test(both) ? 'RP2350' : /RPI-RP2|RP2040|\bRP2\b/i.test(both) ? 'RP2040' : null;
  return { model, boardId, chip };
}

function buildFor(chip) {
  return manifest?.builds?.find(b => b.chip === chip) || null;
}

// Installed gateway version against the one this page installs.
function installedRow(version) {
  const latest = manifest?.version, v = escapeHtml(version), diff = latest ? cmpVersion(version, latest) : 0;
  if (!latest || diff === 0) return [`IRIV IOC Gateway ${v}${latest ? ' · up to date' : ''}`, 'ok'];
  return diff < 0 ? [`IRIV IOC Gateway ${v} · <b>update available: ${escapeHtml(latest)}</b> (step 1)`, 'bad'] : [`IRIV IOC Gateway ${v} · newer than this installer (${escapeHtml(latest)})`, 'ok'];
}

function clockText(rtc) {
  if (rtc.state === 'missing') return 'RTC chip not answering';
  if (rtc.state === 'lost') return 'Time lost: the clock stopped while unpowered (CR2032 flat or missing, or never set)';
  return `${new Date(rtc.time * 1000).toLocaleString()}${rtc.synced ? ` · set from this computer${rtc.wasLost ? ': it had lost its time, check the CR2032' : ''}` : ''}`;
}

function renderVersion() {
  const tag = $('#top-version'), device = detected.hello?.version, latest = manifest?.version;
  tag.hidden = !device && !latest;
  tag.textContent = device ? `controller v${device}${latest && cmpVersion(device, latest) < 0 ? ` · v${latest} available` : ''}` : `firmware v${latest}`;
  tag.title = latest ? `This page installs firmware ${latest} (commit ${manifest.commit}, built ${manifest.built.slice(0, 10)})` : '';
}

function renderDetected() {
  const rows = [];
  const { usb, drive, hello, other } = detected;
  if (usb) rows.push(['USB', `${escapeHtml(usb.name)} <code>${hex4(usb.vid)}:${hex4(usb.pid)}</code>`, usb.rp ? 'ok' : 'bad']);
  if (drive) rows.push(['Install drive', `${escapeHtml(drive.model || 'unknown model')} <small class="muted">Board-ID ${escapeHtml(drive.boardId || '?')}</small>`, drive.chip ? 'ok' : 'bad']);
  if (hello) {
    const match = hello.chip === hello.builtFor;
    rows.push(['Processor', `${escapeHtml(hello.chip || '?')}${hello.chipRevision !== undefined ? ` <small class="muted">revision ${hello.chipRevision}</small>` : ''}`, match ? 'ok' : 'bad']);
    rows.push(['Board', `${escapeHtml(hello.board || '?')} <small class="muted">${escapeHtml(hello.host)}</small>`, 'ok']);
    rows.push(['Installed', ...installedRow(hello.version)]);
    rows.push(['Settings', hello.config ? 'Saved on the controller' : 'None yet: running on defaults (send them in step 2)', hello.config ? 'ok' : 'bad']);
    if (hello.rtc) rows.push(['Clock', escapeHtml(clockText(hello.rtc)), hello.rtc.state === 'ok' && !hello.rtc.wasLost ? 'ok' : 'bad']);
  } else if (other) {
    rows.push(['Installed', `Other firmware: ${escapeHtml(other)}. Install the gateway in step 1.`, 'bad']);
  } else if (drive) {
    rows.push(['Installed', 'Unknown: the controller is in install mode (a new or empty controller always starts this way)', 'ok']);
  }
  const chip = hello?.chip?.replace(/[AB]$/, '') || drive?.chip;
  const build = chip && buildFor(chip);
  if (chip) rows.push(['Firmware', build ? `IRIV IOC Gateway ${escapeHtml(manifest.version)} for <b>${escapeHtml(build.variant || build.chip)}</b> matches this ${escapeHtml(chip)}` : `No build for ${escapeHtml(chip)}: this firmware is for ${escapeHtml((manifest?.builds || []).map(b => b.variant || b.chip).join(', ') || 'the IRIV IOC (RP2350A)')}`, build ? 'ok' : 'bad']);
  $('#inst-detected').hidden = !rows.length;
  $('#inst-detected').innerHTML = rowsHtml('Detected device', rows);
}

// rows: [label, html, 'ok' | 'bad' | 'info']
const rowsHtml = (title, rows) => `<small>${title}</small><dl>${rows.map(([k, v, s]) => `<div class="${s}"><dt>${k}</dt><dd>${icon(s === 'ok' ? 'check' : s === 'info' ? 'info' : 'alert')}<span>${v}</span></dd></div>`).join('')}</dl>`;

// Judges the facts the controller reports for SELFTEST.
function checkRows(t) {
  const fs = t.flash || {}, st = t.settings || {};
  const leds = `<span class="leds">${t.di.map((on, i) => `<span class="led ${on ? 'on' : ''}" title="DI${i} ${on ? 'on' : 'off'}"><i></i>${i}</span>`).join('')}</span>`;
  return [
    ['Ethernet chip', t.ethernetChip ? 'W5500 answers' : 'W5500 not answering: Ethernet will not work', t.ethernetChip ? 'ok' : 'bad'],
    ['Ethernet port', t.link ? `Cable in · ${t.ip !== '0.0.0.0' ? escapeHtml(t.ip) : 'no address yet'}` : 'No cable: plug one in to test the port', t.link ? 'ok' : 'info'],
    ['Clock', escapeHtml(clockText(t.rtc)), t.rtc.state === 'ok' ? 'ok' : 'bad'],
    ['Flash storage', !fs.mounted ? 'Cannot mount: settings cannot be saved' : fs.total ? `${kb(fs.total - fs.used)} free of ${kb(fs.total)}` : 'Mounted', fs.mounted ? 'ok' : 'bad'],
    ['Settings', !st.saved ? 'None yet: running on defaults' : st.note ? `Saved, but: ${escapeHtml(st.note)}` : 'Saved and loaded', !st.saved ? 'info' : st.note ? 'bad' : 'ok'],
    ['Chip temperature', `${t.temperature.toFixed(1)} °C`, t.temperature < 85 ? 'ok' : 'bad'],
    ['Analog inputs', `AN0 ${t.an[0].toFixed(2)} V · AN1 ${t.an[1].toFixed(2)} V`, 'info'],
    ['Digital inputs', `${leds} <small class="muted">switch an input on: it lights up here</small>`, 'info']
  ];
}
const checkProblems = () => check?.result ? checkRows(check.result).filter(r => r[2] === 'bad').map(r => r[0]) : [];

function renderCheck() {
  $('#inst-check').hidden = !check;
  if (!check) return;
  $('#inst-identify').hidden = !check.result;
  if (!check.result) {
    $('#inst-check-rows').innerHTML = rowsHtml('Controller check', [['Self-test', `Firmware ${escapeHtml(check.unsupported)} has no self-test (it came in 0.3.0). Install the update, then connect again to check the hardware.`, 'info']]);
    return;
  }
  const problems = checkProblems().length;
  $('#inst-check-rows').innerHTML = rowsHtml(`Controller check · ${problems ? `${problems} problem${problems > 1 ? 's' : ''}` : 'no problems found'} · ${new Date(check.at).toLocaleTimeString()}`, checkRows(check.result));
}

// Runs the controller's self-test over l (an open IocLink).
async function runChecks(l) {
  try {
    check = { result: await l.request('SELFTEST', ['selftest'], 3000), at: Date.now() };
  } catch (e) {
    if (!/unknown command/.test(e.message)) throw e;
    check = { unsupported: detected.hello?.version || 'on this controller' };
  }
  renderCheck();
}

// ---------- USB serial link ----------
class IocLink {
  constructor(port) { this.port = port; this.waiters = []; this.buffer = ''; this.seen = ''; }

  async open() {
    await this.port.open({ baudRate: 115200 });
    await this.port.setSignals({ dataTerminalReady: true }).catch(() => {}); // Arduino-Pico only sends while DTR is set
    this.writer = this.port.writable.getWriter();
    const decoder = new TextDecoderStream();
    this.piped = this.port.readable.pipeTo(decoder.writable).catch(() => {});
    this.reader = decoder.readable.getReader();
    this.reading = this.readLoop();
  }

  async readLoop() {
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.buffer += value;
        let end;
        while ((end = this.buffer.indexOf('\n')) >= 0) {
          const line = this.buffer.slice(0, end).replace(/\r$/, '');
          this.buffer = this.buffer.slice(end + 1);
          this.onLine(line);
        }
      }
    } catch { /* port closed or unplugged */ }
  }

  onLine(line) {
    logLine(line);
    this.seen = (this.seen + line + '\n').slice(-2000); // to recognise other firmware by its banner
    if (!line.startsWith('@IOC ')) return;
    let msg;
    try { msg = JSON.parse(line.slice(5)); } catch { return; }
    const waiter = this.waiters.find(w => w.types.includes(msg.type) || msg.type === 'error');
    if (!waiter) return;
    this.waiters.splice(this.waiters.indexOf(waiter), 1);
    clearTimeout(waiter.timer);
    msg.type === 'error' ? waiter.reject(new Error(msg.message)) : waiter.resolve(msg);
  }

  async send(text, payload) {
    await this.writer.write(new TextEncoder().encode(text + '\n'));
    // Small chunks keep the controller's USB buffer from overflowing.
    if (payload) for (let i = 0; i < payload.length; i += 512) await this.writer.write(payload.subarray(i, i + 512));
  }

  request(text, types, timeoutMs = 3000, payload) {
    return new Promise((resolve, reject) => {
      const waiter = { types, resolve, reject };
      waiter.timer = setTimeout(() => {
        this.waiters.splice(this.waiters.indexOf(waiter), 1);
        reject(new Error('The controller did not answer. Is the gateway firmware installed?'));
      }, timeoutMs);
      this.waiters.push(waiter);
      this.send(text, payload).catch(e => { clearTimeout(waiter.timer); reject(e); });
    });
  }

  async close() {
    await this.reader?.cancel().catch(() => {});
    await this.piped;
    try { this.writer?.releaseLock(); } catch { /* already released */ }
    await this.port.close().catch(() => {});
  }
}

// ---------- helpers ----------
function logLine(text) {
  const log = $('#inst-log');
  log.textContent = (log.textContent + text + '\n').slice(-6000);
  log.scrollTop = log.scrollHeight;
}
const wait = ms => new Promise(r => setTimeout(r, ms));
const kb = n => `${Math.round(n / 1024)} KB`;

function setDevice(text, ok) {
  $('#inst-device').textContent = text;
  $('#inst-device').classList.toggle('on', !!ok);
  $('#inst-send').disabled = !ok;
}

async function choosePort() {
  if (!hasSerial) throw new Error('This browser cannot open USB serial ports. Use Chrome or Edge on a computer.');
  return navigator.serial.requestPort(); // the user picks the controller
}

// ---------- firmware ----------
async function loadFirmwareInfo() {
  try {
    manifest = await (await fetch(`${FIRMWARE_DIR}manifest.json`, { cache: 'no-store' })).json();
    // Older manifests had one build at the top level (always the IRIV IOC's RP2350).
    manifest.builds ||= [{ chip: 'RP2350', variant: 'RP2350A', board: manifest.board, file: manifest.file, size: manifest.size, sha256: manifest.sha256 }];
    const main = manifest.builds[0];
    $('#inst-download').href = FIRMWARE_DIR + main.file;
    renderVersion();
    $('#inst-fw-info').innerHTML = `Version <b>${escapeHtml(manifest.version)}</b> for ${manifest.builds.map(b => `<b>${escapeHtml(b.boardName || b.board)}</b> (${escapeHtml(b.variant || b.chip)})`).join(', ')} · ${kb(main.size)} · built ${escapeHtml(manifest.built.slice(0, 10))} · SHA-256 <code title="${escapeHtml(main.sha256)}">${escapeHtml(main.sha256.slice(0, 12))}…</code>`;
  } catch {
    manifest = null;
    // Opened from disk or the site is not built: point to the published installer instead.
    $('#inst-download').href = 'https://github.com/dineshpalsingh1989/IRIV-IOC-Gateway/releases/latest';
    $('#inst-download').removeAttribute('download');
    $('#inst-fw-info').innerHTML = 'Firmware is not bundled with this copy. Use <a href="https://dineshpalsingh1989.github.io/IRIV-IOC-Gateway/#install" target="_blank" rel="noreferrer">the online installer</a>, or pick a .uf2 file below.';
  }
}

// Firmware for the chip on the drive: the user's own file (checked) or the matching published build.
async function firmwareFor(chip) {
  if (customFile) {
    const fileChip = uf2Chip(customFile.bytes);
    if (fileChip !== chip) throw new Error(`${customFile.name} is built for ${fileChip || 'an unknown chip'}, but this controller has a ${chip}. Nothing was written.`);
    return customFile;
  }
  const build = buildFor(chip);
  if (!build) {
    const offered = (manifest?.builds || []).map(b => b.variant || b.chip).join(', ');
    throw new Error(manifest
      ? `This device has a ${chip}; the IRIV IOC Gateway firmware is for ${offered}. Nothing was written.`
      : 'No firmware file yet: download it, or pick a .uf2 file from this computer.');
  }
  const response = await fetch(FIRMWARE_DIR + build.file);
  if (!response.ok) throw new Error(`Could not download the firmware (${response.status})`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (uf2Chip(bytes) !== chip) throw new Error('The downloaded firmware does not match its description. Nothing was written.');
  return { name: build.file, bytes };
}

async function enterInstallMode() {
  const port = link ? link.port : await choosePort();
  if (link) await disconnect(); // reopened below
  detected.usb = usbInfo(port);
  renderDetected();
  // Rebooting into install mode only makes sense on RP2040/RP2350 boards: stop on anything else.
  if (detected.usb && !detected.usb.rp) {
    throw new Error(`That port is a ${detected.usb.name}, not an IRIV IO Controller (RP2350). Pick the controller's port.`);
  }
  // Ask our gateway first; any Arduino-Pico firmware also reboots on a 1200-baud open and close.
  const tryLink = new IocLink(port);
  let gateway = false;
  try {
    await tryLink.open();
    detected.hello = await tryLink.request('HELLO', ['hello'], 1500);
    gateway = true;
  } catch { /* not our gateway */ }
  if (gateway) {
    renderDetected();
    renderVersion();
    // Check the hardware before replacing the firmware.
    await runChecks(tryLink).catch(e => logLine(`Controller check failed: ${e.message}`));
    const problems = checkProblems();
    logLine(`Self-test before install: ${!check?.result ? 'not available on this firmware' : problems.length ? `problems: ${problems.join(', ')}` : 'no problems found'}`);
    if (problems.length && !confirm(`The controller check found problems: ${problems.join(', ')}.\n\nInstall anyway?`)) {
      await tryLink.close();
      return;
    }
    // Gateway 0.3.1+ takes the published firmware straight over USB: no install mode, no drive to pick.
    const update = buildFor((detected.hello.chip || '').replace(/[AB]$/, ''))?.update;
    if (update && !customFile && cmpVersion(detected.hello.version, '0.3.1') >= 0) {
      if (cmpVersion(detected.hello.version, manifest.version) >= 0 && !confirm(`This controller already runs ${detected.hello.version}. Install ${manifest.version} again?`)) {
        await tryLink.close();
        return;
      }
      try { await updateOverUsb(tryLink, update); } finally { await tryLink.close(); }
      if (!(await reconnect(port))) toast('Firmware installed. Click Connect over USB (step 2) to check it');
      return;
    }
    await tryLink.send('BOOTLOADER');
    await wait(200);
    await tryLink.close();
  } else {
    await tryLink.close();
    await port.open({ baudRate: 1200 });
    await port.setSignals({ dataTerminalReady: false }).catch(() => {});
    await port.close().catch(() => {});
  }
  toast('Install mode: a drive called RP2350 should appear in a few seconds');
}

// Downloads the published .bin, checks it against the manifest and sends it to a running gateway (0.3.1+),
// which checks the MD5 again before installing it on restart.
async function updateOverUsb(l, update) {
  const response = await fetch(FIRMWARE_DIR + update.file);
  if (!response.ok) throw new Error(`Could not download the firmware (${response.status})`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  const sha256 = [...digest].map(b => b.toString(16).padStart(2, '0')).join('');
  if (bytes.length !== update.size || sha256 !== update.sha256) throw new Error('The downloaded firmware does not match its description. Nothing was written.');
  logLine(`Installing firmware ${manifest.version} over USB (${kb(bytes.length)}). Keep the cable in…`);
  toast(`Installing firmware ${manifest.version}… keep the cable in`);
  await l.request(`UPDATE ${bytes.length} ${update.md5}`, ['updated'], 120000, bytes);
  logLine('Firmware received and checked. The controller restarts and installs it…');
}

async function writeFirmware() {
  if (!hasDirectoryPicker) throw new Error('This browser cannot write to drives. Download the .uf2 and drag it onto the RP2350 drive.');
  const dir = await window.showDirectoryPicker({ id: 'rp2350', mode: 'readwrite' });
  // Every RP2040/RP2350 install-mode drive has this file, naming the chip: refuse anything else.
  let info;
  try { info = await (await (await dir.getFileHandle('INFO_UF2.TXT')).getFile()).text(); } catch {
    throw new Error('That is not the RP2350 drive (no INFO_UF2.TXT). Put the controller in install mode and pick its drive.');
  }
  detected.drive = parseDriveInfo(info);
  renderDetected();
  if (!detected.drive.chip) throw new Error(`Unrecognised install drive (${detected.drive.model || 'no model'}). Nothing was written.`);
  const firmware = await firmwareFor(detected.drive.chip); // same processor as the drive, or an error
  const file = await dir.getFileHandle(firmware.name || 'iriv-ioc-gateway.uf2', { create: true });
  const writable = await file.createWritable();
  await writable.write(firmware.bytes);
  try {
    await writable.close();
  } catch { /* the bootloader restarts as soon as it has every block, which can cut the drive off mid-close */ }
  toast('Firmware copied. The controller is restarting: reconnecting…');
  logLine(`Firmware ${firmware.name} copied (${kb(firmware.bytes.length)}). Waiting for the controller to restart…`);
  if (!(await reconnect())) toast('Firmware installed. Click Connect over USB (step 2) to check it');
}

// ---------- settings ----------
function renderSummary() {
  const c = config;
  const items = [
    ['Ethernet', c.network.dhcp ? 'DHCP' : `static ${c.network.ip || '(no address set)'}`],
    ['Modbus TCP', c.modbus.enabled ? `port ${c.modbus.port}, unit ${c.modbus.unit}` : 'off'],
    ['MQTT', c.mqtt.enabled ? `${c.mqtt.host || '(no broker set)'}:${c.mqtt.port}` : 'off'],
    ['RS485', c.rs485.enabled ? `${points().length} point${points().length === 1 ? '' : 's'} at ${c.rs485.baud} baud` : 'off'],
    ['Analog', config.channels.filter(x => x.kind === 'analog-input').map(x => `${x.id} ${x.mode}`).join(' · ')]
  ];
  $('#inst-summary').innerHTML = items.map(([k, v]) => `<li><span>${k}</span><b>${escapeHtml(v)}</b></li>`).join('');
  $('#inst-pass-row').hidden = !c.mqtt.pass;
}

async function connect(port) {
  if (link) await disconnect();
  detected.usb = usbInfo(port);
  link = new IocLink(port);
  await link.open();
  let hello;
  try {
    hello = await link.request('HELLO', ['hello'], 2500);
  } catch {
    // Something answers on USB, but not our gateway: name it if its banner says what it is.
    detected.hello = null;
    detected.other = /CircuitPython/i.test(link.seen) ? 'CircuitPython' : /MicroPython/i.test(link.seen) ? 'MicroPython' : 'not the IRIV IOC Gateway (for example the factory firmware)';
    renderDetected();
    renderVersion();
    await disconnect();
    throw new Error(`This controller runs ${detected.other}. Install the gateway firmware in step 1 first.`);
  }
  detected.hello = hello;
  detected.other = null;
  await syncClock(hello);
  await runChecks(link).catch(e => logLine(`Controller check failed: ${e.message}`));
  renderDetected();
  renderVersion();
  setDevice(`${hello.host} · ${hello.chip || 'chip ?'} · firmware ${hello.version}`, true);
  logLine(`Connected to ${hello.host} (${hello.chip}, board ${hello.board})`);
  if (hello.chip && hello.builtFor && hello.chip !== hello.builtFor) {
    toast(`Warning: this controller has a ${hello.chip}; the firmware is built for ${hello.builtFor}. Pins may not match.`);
  }
  refreshStatus();
  statusTimer = setInterval(refreshStatus, 3000);
}

// Sets the controller's RTC from this computer when it lost its time or is more than 2 s off.
async function syncClock(hello) {
  const rtc = hello.rtc; // absent on firmware before 0.3.0
  if (!rtc || rtc.state === 'missing') return;
  const now = Math.round(Date.now() / 1000);
  if (rtc.state === 'ok' && Math.abs(rtc.time - now) <= 2) return;
  try {
    const reply = await link.request(`TIME ${now}`, ['time'], 2500);
    hello.rtc = { ...reply.rtc, synced: true, wasLost: rtc.state === 'lost' };
    logLine(`Clock set from this computer (was ${rtc.state === 'lost' ? 'lost' : new Date(rtc.time * 1000).toLocaleString()})`);
    if (rtc.state === 'lost') toast('The controller had lost its time (CR2032 flat or missing, or never set). Clock set from this computer.');
  } catch (e) { logLine(`Could not set the clock: ${e.message}`); }
}

// After a restart the controller re-enumerates over USB: reconnect to a port the user already allowed,
// then report in the USB log whether it came up and on which IP address. False when it did not come back.
async function reconnect(port) {
  if (!hasSerial) return false;
  setDevice('Restarting…', false);
  for (let attempt = 0; attempt < 24; attempt++) { // an update takes longer: the new firmware is copied first
    await wait(1500);
    const known = await navigator.serial.getPorts();
    for (const p of [port, ...known.filter(k => k !== port)].filter(Boolean)) {
      try { await connect(p); await reportRunning(); return true; } catch { await disconnect(); }
    }
  }
  setDevice('Not connected', false);
  return false;
}

// Waits up to 60 s for an IP address, then writes a summary to the USB log and shows it.
async function reportRunning() {
  let s;
  for (let i = 0; i < 30 && link; i++) {
    try { s = await link.request('STATUS', ['status'], 2500); } catch { /* busy, try again */ }
    if (s?.online) break;
    await wait(2000);
  }
  if (!s) return;
  const problems = checkProblems();
  const lines = [
    `==== ${s.host} · IRIV IOC Gateway ${s.version} is running ====`,
    `IP address : ${s.online ? `${s.ip} (${s.dhcp ? 'DHCP' : 'static'})` : s.link ? 'no address yet: is there a DHCP server?' : 'no Ethernet cable'}`,
    `Modbus TCP : ${s.modbus.enabled ? `port ${s.modbus.port}` : 'off'}`,
    `MQTT       : ${s.mqtt.enabled ? `${s.mqtt.connected ? 'connected to' : 'not connected to'} ${s.mqtt.host}${s.mqtt.tls ? (s.mqtt.brokerChecked ? ' (TLS, broker checked)' : ' (TLS)') : ''}` : 'off'}`,
    `Self-test  : ${!check?.result ? 'not available' : problems.length ? `problems: ${problems.join(', ')}` : 'no problems found'}`
  ];
  lines.forEach(logLine);
  $('#inst-log').closest('details').open = true;
  $('#inst-log').scrollIntoView({ behavior: 'smooth', block: 'center' });
  toast(s.online ? `Running on ${s.ip}` : 'Running, but no IP address yet');
}

async function disconnect() {
  clearInterval(statusTimer);
  const old = link;
  link = null;
  await old?.close();
  setDevice('Not connected', false);
}

async function sendSettings() {
  const text = JSON.stringify(buildConfigFile($('#inst-pass').checked && !!config.mqtt.pass));
  const bytes = new TextEncoder().encode(text);
  $('#inst-send').disabled = true;
  try {
    const saved = await link.request(`CONFIG ${bytes.length}`, ['saved'], 20000, bytes);
    toast(saved.warning ? `Saved with a warning: ${saved.warning}` : 'Settings saved. The controller is restarting…');
    const port = link.port;
    await disconnect();
    if (!(await reconnect(port))) toast('Settings saved. Click Connect over USB to see the status');
  } catch (e) {
    $('#inst-send').disabled = !link;
    throw e;
  }
}

async function refreshStatus() {
  if (!link) return;
  let s;
  try { s = await link.request('STATUS', ['status'], 2500); } catch { return; }
  const yes = (ok, a, b) => `<span class="state-text ${ok ? 'on' : ''}">${ok ? a : b}</span>`;
  const rows = [
    ['Ethernet', s.online ? `${yes(true, 'Online', '')} ${escapeHtml(s.ip)} ${s.dhcp ? '(DHCP)' : '(static)'}` : yes(false, '', s.link ? 'Cable in, no address yet' : 'No cable')],
    ['Modbus TCP', s.modbus.enabled ? `port ${s.modbus.port} · ${s.modbus.clients} client${s.modbus.clients === 1 ? '' : 's'} · ${s.modbus.requests} requests` : 'off'],
    ['MQTT', s.mqtt.enabled ? `${yes(s.mqtt.connected, 'Connected', 'Not connected')} ${escapeHtml(s.mqtt.host)}` : 'off'],
    ['Running for', `${Math.floor(s.uptime / 60)} min ${s.uptime % 60} s · firmware ${escapeHtml(s.version)}`],
    ...(s.rtc ? [['Clock', escapeHtml(clockText(s.rtc))]] : [])
  ];
  const pts = s.rs485.length ? `<table class="channel-table mini"><thead><tr><th>Point</th><th>Value</th><th>Errors</th></tr></thead><tbody>${s.rs485.map(p => `<tr><td>${escapeHtml(p.id)}</td><td>${p.value === null ? '<span class="badge danger">no reply</span>' : num(p.value)}</td><td>${p.errors}</td></tr>`).join('')}</tbody></table>` : '';
  $('#inst-status').innerHTML = `<dl class="kv">${rows.map(([k, v]) => `<div><dt>${k}</dt><dd>${v}</dd></div>`).join('')}</dl>${pts}`;
  if (link && check?.result) await runChecks(link).catch(() => {}); // keeps the digital inputs live
}

// ---------- wiring ----------
function guard(button, action) {
  $(button).addEventListener('click', async () => {
    try { await action(); }
    catch (e) { if (e.name !== 'NotFoundError' && e.name !== 'AbortError') toast(e.message); } // NotFound/Abort = picker cancelled
  });
}

$('#inst-browser').hidden = hasSerial && hasDirectoryPicker;
guard('#inst-bootloader', enterInstallMode);
guard('#inst-write', writeFirmware);
guard('#inst-connect', async () => connect(await choosePort()));
guard('#inst-send', sendSettings);
guard('#inst-identify', async () => {
  if (!link) throw new Error('Connect over USB first (step 2)');
  await link.request('IDENTIFY', ['identify'], 2500);
});
$('#inst-file').addEventListener('change', async e => {
  const file = e.target.files[0];
  if (!file) return;
  const bytes = new Uint8Array(await file.arrayBuffer());
  const chip = uf2Chip(bytes);
  if (!chip || !/^RP2(040|350)$/.test(chip)) {
    customFile = null;
    $('#inst-file-info').textContent = `${file.name} is not a usable .uf2 firmware file (${chip || 'not UF2'})`;
    return;
  }
  customFile = { name: 'iriv-ioc-gateway.uf2', bytes };
  $('#inst-file-info').textContent = `${file.name} · ${kb(file.size)} · built for ${chip}. It will only be written to a ${chip}.`;
});
if (hasSerial) navigator.serial.addEventListener('disconnect', e => { if (link && e.target === link.port) disconnect(); });
window.addEventListener('hashchange', () => { if (location.hash === '#install') renderSummary(); });
renderSummary();
loadFirmwareInfo();
