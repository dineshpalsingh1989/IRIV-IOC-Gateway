// Browser installer for the IRIV IOC gateway (Install page).
// Firmware: put the RP2350 in install mode, then write the .uf2 onto its USB drive.
// Settings: send ioc_config.json over USB (Web Serial) using the firmware's line protocol:
//   HELLO | STATUS | CONFIG <bytes> + JSON | REBOOT | BOOTLOADER  ->  "@IOC {json}" replies.
// Uses app.js globals: $, icon, escapeHtml, toast, config, points, buildConfigFile, num.

const FIRMWARE_DIR = 'firmware/';
const hasSerial = 'serial' in navigator;
const hasDirectoryPicker = 'showDirectoryPicker' in window;
let firmware = null;   // { name, bytes, version } once known
let link = null;       // open IocLink
let statusTimer = null;

// ---------- USB serial link ----------
class IocLink {
  constructor(port) { this.port = port; this.waiters = []; this.buffer = ''; }

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
    const manifest = await (await fetch(`${FIRMWARE_DIR}manifest.json`, { cache: 'no-store' })).json();
    firmware = { name: manifest.file, url: FIRMWARE_DIR + manifest.file, version: manifest.version };
    $('#inst-download').href = firmware.url;
    $('#inst-fw-info').innerHTML = `Version <b>${escapeHtml(manifest.version)}</b> · ${kb(manifest.size)} · built ${escapeHtml(manifest.built.slice(0, 10))} · SHA-256 <code title="${escapeHtml(manifest.sha256)}">${escapeHtml(manifest.sha256.slice(0, 12))}…</code>`;
  } catch {
    // Opened from disk or the site is not built: point to the published installer instead.
    $('#inst-download').href = 'https://github.com/dineshpalsingh1989/IRIV-IOC-Gateway/releases/latest';
    $('#inst-download').removeAttribute('download');
    $('#inst-fw-info').innerHTML = 'Firmware is not bundled with this copy. Use <a href="https://dineshpalsingh1989.github.io/IRIV-IOC-Gateway/#install" target="_blank" rel="noreferrer">the online installer</a>, or pick a .uf2 file below.';
  }
}

async function firmwareBytes() {
  if (firmware?.bytes) return firmware.bytes;
  if (!firmware?.url) throw new Error('No firmware file yet: download it, or pick a .uf2 file from this computer.');
  const response = await fetch(firmware.url);
  if (!response.ok) throw new Error(`Could not download the firmware (${response.status})`);
  firmware.bytes = new Uint8Array(await response.arrayBuffer());
  return firmware.bytes;
}

async function enterInstallMode() {
  const port = await choosePort();
  // Ask our gateway first; any Arduino-Pico firmware also reboots on a 1200-baud open and close.
  const tryLink = new IocLink(port);
  try {
    await tryLink.open();
    await tryLink.request('HELLO', ['hello'], 1500);
    await tryLink.send('BOOTLOADER');
    await wait(200);
    await tryLink.close();
  } catch {
    await tryLink.close();
    await port.open({ baudRate: 1200 });
    await port.setSignals({ dataTerminalReady: false }).catch(() => {});
    await port.close().catch(() => {});
  }
  toast('Install mode: a drive called RP2350 should appear in a few seconds');
}

async function writeFirmware() {
  if (!hasDirectoryPicker) throw new Error('This browser cannot write to drives. Download the .uf2 and drag it onto the RP2350 drive.');
  const bytes = await firmwareBytes();
  const dir = await window.showDirectoryPicker({ id: 'rp2350', mode: 'readwrite' });
  // Every RP2040/RP2350 install-mode drive has this file: refuse other folders so nothing else is overwritten.
  try { await dir.getFileHandle('INFO_UF2.TXT'); } catch {
    throw new Error('That is not the RP2350 drive (no INFO_UF2.TXT). Put the controller in install mode and pick its drive.');
  }
  const file = await dir.getFileHandle(firmware.name || 'iriv-ioc-gateway.uf2', { create: true });
  const writable = await file.createWritable();
  await writable.write(bytes);
  try {
    await writable.close();
    toast('Firmware copied. The controller restarts by itself in a few seconds');
  } catch {
    // The bootloader restarts as soon as it has every block, which can cut the drive off mid-close.
    toast('The drive disconnected after the copy: that is normal, the controller is restarting');
  }
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
  link = new IocLink(port);
  await link.open();
  const hello = await link.request('HELLO', ['hello'], 2500);
  setDevice(`${hello.host} · firmware ${hello.version}`, true);
  logLine(`Connected to ${hello.host}`);
  refreshStatus();
  statusTimer = setInterval(refreshStatus, 3000);
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
    setDevice('Restarting…', false);
    // The controller re-enumerates over USB; reconnect to the port the user already allowed.
    for (let attempt = 0; attempt < 12; attempt++) {
      await wait(1500);
      const known = await navigator.serial.getPorts();
      for (const p of [port, ...known.filter(k => k !== port)]) {
        try { await connect(p); return; } catch { await disconnect(); }
      }
    }
    setDevice('Not connected', false);
    toast('Settings saved. Click Connect over USB to see the status');
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
    ['Running for', `${Math.floor(s.uptime / 60)} min ${s.uptime % 60} s · firmware ${escapeHtml(s.version)}`]
  ];
  const pts = s.rs485.length ? `<table class="channel-table mini"><thead><tr><th>Point</th><th>Value</th><th>Errors</th></tr></thead><tbody>${s.rs485.map(p => `<tr><td>${escapeHtml(p.id)}</td><td>${p.value === null ? '<span class="badge danger">no reply</span>' : num(p.value)}</td><td>${p.errors}</td></tr>`).join('')}</tbody></table>` : '';
  $('#inst-status').innerHTML = `<dl class="kv">${rows.map(([k, v]) => `<div><dt>${k}</dt><dd>${v}</dd></div>`).join('')}</dl>${pts}`;
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
$('#inst-file').addEventListener('change', async e => {
  const file = e.target.files[0];
  if (!file) return;
  firmware = { name: 'iriv-ioc-gateway.uf2', bytes: new Uint8Array(await file.arrayBuffer()), version: 'from file' };
  $('#inst-file-info').textContent = `${file.name} · ${kb(file.size)} will be written`;
});
if (hasSerial) navigator.serial.addEventListener('disconnect', e => { if (link && e.target === link.port) disconnect(); });
window.addEventListener('hashchange', () => { if (location.hash === '#install') renderSummary(); });
renderSummary();
loadFirmwareInfo();
