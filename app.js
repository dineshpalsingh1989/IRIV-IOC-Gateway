const STORAGE_KEY = 'fieldnote-iriv-ioc-v1';
const VIEWS = ['overview', 'channels', 'connections', 'install'];

// Analog modes. The AN-IN MODE DIP switch must match (datasheet §5.2.3): V = 0–10.56 V, mA = 0–42.58 mA full scale.
const SIGNAL = {
  '0-10V': { label: '0–10 V', lo: 0, hi: 10, unit: 'V', dip: 'Voltage' },
  '0-20mA': { label: '0–20 mA', lo: 0, hi: 20, unit: 'mA', dip: 'Current' },
  '4-20mA': { label: '4–20 mA', lo: 4, hi: 20, unit: 'mA', dip: 'Current' }
};
const PATTERNS = {
  continuous: { label: 'Continuous', on: Infinity, off: 0 },
  beep: { label: 'Fast beep', on: 0.2, off: 0.2 },
  pulse: { label: 'Slow pulse', on: 0.5, off: 1 },
  once: { label: 'Single beep', on: 0.3, off: Infinity }
};
// Modbus TCP tables served by the IOC
const REG = {
  coil: { name: 'Coil', prefix: '0', fc: 'FC01 / FC05' },
  discrete: { name: 'Discrete input', prefix: '1', fc: 'FC02' },
  input: { name: 'Input register', prefix: '3', fc: 'FC04' }
};
// Function codes the IOC uses to read RS485 devices
const FC = {
  holding: { label: 'FC03 · Holding register', short: 'FC03' },
  input: { label: 'FC04 · Input register', short: 'FC04' },
  coil: { label: 'FC01 · Coil', short: 'FC01', bool: true },
  discrete: { label: 'FC02 · Discrete input', short: 'FC02', bool: true }
};
const TYPES = { int16: 'int16', uint16: 'uint16', int32: 'int32 · 2 regs', uint32: 'uint32 · 2 regs', float32: 'float32 · 2 regs' };
// Modbus TCP addresses served by the gateway firmware. Keep in sync with firmware/src/register_map.h
// (python firmware/tools/check_register_map.py). DI/DO/AN/counters match Cytron's own firmware.
const MB = { DI: 0x0000, DO: 0x0100, ANV: 0x0200, ANA: 0x0210, AN_SCALED: 0x0220, COUNT_EN: 0x0300, COUNT_RST: 0x0310, COUNT: 0x0400, BUZZER: 0x0500, RS485: 0x1000, MODEL: 0x0F00, VERSION: 0x0F10 };
const RS485_REGS = 128; // float32 per point: room for 64 points
const COUNTER_INPUTS = [1, 3, 5, 7, 9]; // pulse counting is only wired on these inputs
const KIND = {
  'digital-input': { name: 'Digital input', icon: 'input' },
  'digital-output': { name: 'Digital output', icon: 'output' },
  'analog-input': { name: 'Analog input', icon: 'gauge' },
  buzzer: { name: 'Buzzer', icon: 'bell' }
};
// Fields a user may change; id, kind, pin and Modbus address are fixed hardware/map
const EDITABLE = ['label', 'value', 'topic', 'cmdTopic', 'mqtt', 'unit', 'mode', 'scaleLo', 'scaleHi', 'counter', 'trigger', 'compare', 'threshold', 'freq', 'pattern', 'silence'];
const definitions = [
  // pin = RP2350A GPIO per datasheet §5.1; address = fixed Modbus TCP address
  ...Array.from({ length: 11 }, (_, i) => ({ id: `DI${i}`, kind: 'digital-input', pin: i, label: `Digital input ${i}`, value: false, address: MB.DI + i, topic: '', mqtt: true, ...(COUNTER_INPUTS.includes(i) && { counter: false }) })),
  ...Array.from({ length: 4 }, (_, i) => ({ id: `DO${i}`, kind: 'digital-output', pin: 12 + i, label: `Digital output ${i}`, value: false, address: MB.DO + i, topic: '', cmdTopic: '', mqtt: true })),
  { id: 'AN0', kind: 'analog-input', pin: 26, label: 'Analog input 0', value: 7.36, address: 0, topic: '', mqtt: true, mode: '0-10V', unit: 'V', scaleLo: 0, scaleHi: 10 },
  { id: 'AN1', kind: 'analog-input', pin: 27, label: 'Analog input 1', value: 12.4, address: 1, topic: '', mqtt: true, mode: '4-20mA', unit: 'bar', scaleLo: 0, scaleHi: 10 },
  { id: 'BZ', kind: 'buzzer', pin: 11, label: 'Buzzer', value: false, address: MB.BUZZER, topic: '', cmdTopic: '', mqtt: true, trigger: 'remote', compare: 'above', threshold: 8, freq: 2700, pattern: 'beep', silence: 0 }
];
const defaults = () => ({
  channels: structuredClone(definitions),
  network: { dhcp: true, ip: '', subnet: '255.255.255.0', gateway: '', dns: '' },
  // Publishing is QoS 0 in WilcoxonMqtt; `qos` is the subscribe QoS for output/buzzer commands.
  mqtt: { enabled: false, host: '', port: '1883', tls: false, ca: '', client: 'iriv-ioc-01', topic: 'factory/line01/ioc01', user: '', pass: '', qos: '0', retain: true, interval: '5' },
  modbus: { enabled: true, port: '502', unit: '255', wordOrder: 'ABCD' },
  rs485: { enabled: false, baud: '9600', parity: 'None', stopBits: '1', poll: '1000', timeout: '500', wordOrder: 'ABCD', points: [] },
  web: { password: '' } // the controller's own web page (user admin)
});
let config = loadConfig();
let selectedChannel = null;
let editingPoint = null;
let activeFilter = 'all';
let dirty = false;
const $ = (s, root = document) => root.querySelector(s);
const $$ = (s, root = document) => [...root.querySelectorAll(s)];
const icon = name => `<svg class="i" aria-hidden="true"><use href="#i-${name}"/></svg>`;
const points = () => config.rs485.points;
const owners = () => [...config.channels, ...points()]; // everything that has an MQTT topic and a Modbus TCP address
const byId = id => owners().find(c => c.id === id);
const escapeHtml = value => String(value).replace(/[&<>"']/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch]);
const options = (pairs, selected) => pairs.map(([v, l]) => `<option value="${escapeHtml(v)}"${String(v) === String(selected) ? ' selected' : ''}>${escapeHtml(l)}</option>`).join('');
const clampPct = n => Math.min(100, Math.max(0, n));
const num = n => Number(n).toLocaleString(undefined, { maximumFractionDigits: 2 });

function loadConfig() {
  try { return normalizeConfig(JSON.parse(localStorage.getItem(STORAGE_KEY))); } catch { return defaults(); }
}
// A saved setup (this browser, or ioc_config.json from the controller) on top of today's defaults.
function normalizeConfig(stored) {
  const fresh = defaults();
  if (!stored) return fresh;
  let storedPoints = (Array.isArray(stored.rs485?.points) ? stored.rs485.points : []).map(p => ({ ...p, kind: 'rs485' }));
  // Points saved before the Cytron-compatible map started at 100: move them into the RS485 block.
  if (storedPoints.some(p => !(p.tcp >= MB.RS485))) storedPoints = storedPoints.map((p, i) => ({ ...p, tcp: MB.RS485 + 2 * i }));
  const { wsPort, ...storedMqtt } = stored.mqtt || {}; // an option the IRIV IOC does not support
  return {
    network: { ...fresh.network, ...stored.network },
    mqtt: { ...fresh.mqtt, ...storedMqtt, qos: storedMqtt.qos === '1' ? '1' : '0' },
    modbus: { ...fresh.modbus, ...stored.modbus },
    rs485: { ...fresh.rs485, ...stored.rs485, points: storedPoints },
    web: { ...fresh.web, ...stored.web },
    channels: fresh.channels.map(c => {
      const s = (stored.channels || []).find(x => x.id === c.id) || {};
      for (const k of EDITABLE) if (k in c && s[k] !== undefined) c[k] = s[k];
      return c;
    })
  };
}
function persist(message = 'Setup saved in this browser') {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(config)); toast(message); }
  catch { toast('Could not save: browser storage is unavailable'); }
}
let toastTimer;
function toast(message) {
  const el = $('#toast'); el.textContent = message; el.classList.add('show');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => el.classList.remove('show'), 3200);
}

/* ---------- Analog ---------- */
function engValue(c, raw = c.value) {
  const s = SIGNAL[c.mode];
  return c.scaleLo + (raw - s.lo) / (s.hi - s.lo) * (c.scaleHi - c.scaleLo);
}
// NAMUR NE43: below 3.6 mA on a 4–20 loop means a broken wire or failed sensor
function analogFault(mode, raw) {
  if (mode === '4-20mA') return raw < 3.6 ? 'Wire break' : raw > 21 ? 'Over range' : '';
  return raw > SIGNAL[mode].hi * 1.05 ? 'Over range' : '';
}

/* ---------- Buzzer ---------- */
function buzzerActive(bz) {
  if (bz.trigger === 'remote') return bz.value;
  const src = byId(bz.trigger);
  if (!src) return false;
  if (src.kind === 'digital-input') return src.value;
  if (analogFault(src.mode, src.value)) return true; // a faulty sensor should always alarm
  const v = engValue(src);
  return bz.compare === 'above' ? v > bz.threshold : v < bz.threshold;
}
function triggerText(bz) {
  if (bz.trigger === 'remote') return 'Remote command (MQTT / Modbus)';
  const src = byId(bz.trigger);
  if (!src) return 'Not assigned';
  if (src.kind === 'digital-input') return `While ${src.id} (${src.label}) is active`;
  return `${src.id} ${bz.compare === 'above' ? '>' : '<'} ${num(bz.threshold)} ${src.unit} or sensor fault`;
}
let audio;
function playTone(freq, pattern) {
  try { audio ??= new AudioContext(); audio.resume(); } catch { toast('Audio is not available in this browser'); return; }
  const osc = audio.createOscillator(), gain = audio.createGain(), t0 = audio.currentTime + 0.02, total = 1.6;
  const { on, off, label } = PATTERNS[pattern] || PATTERNS.beep;
  osc.type = 'square'; osc.frequency.value = freq; gain.gain.value = 0;
  osc.connect(gain).connect(audio.destination);
  for (let t = 0; t < total; t += on + off) { gain.gain.setValueAtTime(0.05, t0 + t); gain.gain.setValueAtTime(0, t0 + Math.min(t + on, total)); }
  osc.start(t0); osc.stop(t0 + total);
  toast(`Playing ${freq} Hz · ${label} (browser preview of the buzzer)`);
}

/* ---------- MQTT topics ---------- */
const slug = c => c.kind === 'buzzer' ? 'buzzer' : c.kind === 'rs485' ? `rs485/${c.id.toLowerCase()}` : c.id.toLowerCase();
const baseTopic = (raw = config.mqtt.topic) => raw.trim().replace(/^\/+|\/+$/g, '') || 'iriv-ioc';
const defaultTopic = (c, field) => `${baseTopic()}/${slug(c)}/${field === 'cmdTopic' ? 'set' : 'state'}`;
const topicOf = (c, field = 'topic') => (c[field] || '').trim() || defaultTopic(c, field);
const hasCmd = c => 'cmdTopic' in c;
function topicError(c, field, raw) {
  const t = raw.trim();
  if (!t) return '';
  if (/[+#]/.test(t)) return 'Topics cannot contain the + or # wildcards';
  if (/^\/|\/$|\/\//.test(t)) return 'Topic cannot start or end with / or have empty levels';
  for (const x of owners()) for (const f of ['topic', 'cmdTopic']) {
    if (!(f in x) || (x === c && f === field)) continue;
    if (topicOf(x, f) === t) return `Topic already used by ${x.id}`;
  }
  return '';
}

/* ---------- Modbus TCP map ---------- */
const hex = n => `0x${n.toString(16).toUpperCase().padStart(4, '0')}`;
function plcRef(table, address, width = 1) {
  const ref = n => `${REG[table].prefix}${String(n).padStart(4, '0')}`;
  return width > 1 ? `${ref(address + 1)}–${ref(address + width)}` : ref(address + 1);
}
const pointRef = p => plcRef('input', p.tcp, 2);
// Every register the firmware serves, one row each. Mirrors buildModbusMap() in firmware/src/main.cpp.
function modbusRows() {
  const rows = [];
  const add = (source, name, table, address, width, format) => rows.push({ source, name, table, address, width, format });
  for (const c of config.channels) {
    if (c.kind === 'digital-input') add(c.id, c.label, 'discrete', c.address, 1, c.counter ? 'reads 0 while counting' : '1 bit');
    if (c.kind === 'digital-output') add(c.id, c.label, 'coil', c.address, 1, '1 bit');
    if (c.kind === 'buzzer') add(c.id, `${c.label} · remote command`, 'coil', c.address, 1, '1 bit');
    if (c.kind === 'analog-input') {
      const i = c.address;
      add(c.id, `${c.label} · scaled (${c.unit})`, 'input', MB.AN_SCALED + 2 * i, 2, 'float32 · NaN on fault');
      add(c.id, `${c.label} · terminal voltage`, 'input', MB.ANV + i, 1, 'uint16 · mV');
      add(c.id, `${c.label} · terminal current`, 'input', MB.ANA + i, 1, 'uint16 · µA');
    }
  }
  COUNTER_INPUTS.forEach((di, k) => {
    const c = byId(`DI${di}`);
    add(c.id, `${c.label} · pulse count`, 'input', MB.COUNT + 2 * k, 2, 'uint32 · high word first');
    add(c.id, `${c.label} · counter enable`, 'coil', MB.COUNT_EN + k, 1, '1 bit');
    add(c.id, `${c.label} · counter reset`, 'coil', MB.COUNT_RST + k, 1, 'write 1, clears itself');
  });
  for (const p of points()) add(p.id, p.name, 'input', p.tcp, 2, 'float32 · scaled · NaN when no reply');
  add('IOC', 'Model "IOC"', 'input', MB.MODEL, 2, 'ASCII');
  add('IOC', 'Firmware version', 'input', MB.VERSION, 3, 'major, minor, patch');
  return rows;
}
// New points go after the highest one so existing SCADA tags never move, even after a delete
const nextTcp = () => Math.max(MB.RS485, ...points().map(p => p.tcp + 2));
const tcpSpaceLeft = () => nextTcp() + 2 <= MB.RS485 + RS485_REGS;
const nextPointId = () => `P${Math.max(0, ...points().map(p => Number(p.id.slice(1)) || 0)) + 1}`;
const pointSource = p => `Slave ${p.slave} · ${FC[p.fn].short} · reg ${p.reg} · ${p.type}`;

/* ---------- Rendering ---------- */
function configText(c) {
  if (c.kind === 'digital-input') return `${c.counter ? 'Pulse counter · ' : ''}${c.pin < 5 ? 'Group 1 · common S/S0..4' : 'Group 2 · common S/S5..10'}`;
  if (c.kind === 'digital-output') return 'Solid-state relay · 50 V / 500 mA';
  if (c.kind === 'analog-input') return `${SIGNAL[c.mode].label} → ${num(c.scaleLo)}–${num(c.scaleHi)} ${c.unit}`;
  return `${triggerText(c)} · ${c.freq} Hz ${PATTERNS[c.pattern].label.toLowerCase()}`;
}
const toggleSwitch = (c, label, on) => `<button type="button" class="mini-toggle" role="switch" aria-checked="${on}" data-toggle="${c.id}" aria-label="${escapeHtml(label)}"></button>`;
function stateCell(c) {
  if (c.kind === 'digital-input' && c.counter) return '<span class="state"><i class="dot"></i>Counting pulses</span>';
  if (c.kind === 'digital-input') return `${toggleSwitch(c, `Simulate ${c.id} ${c.label}`, c.value)}<span class="state-text ${c.value ? 'on' : ''}">${c.value ? 'Active' : 'Inactive'}</span>`;
  if (c.kind === 'digital-output') return `${toggleSwitch(c, `${c.id} ${c.label} (preview)`, c.value)}<span class="state-text ${c.value ? 'on' : ''}">${c.value ? 'On' : 'Off'}</span>`;
  if (c.kind === 'analog-input') {
    const fault = analogFault(c.mode, c.value);
    return fault ? `<span class="badge danger">${icon('alert')}${fault}</span> <small class="raw">${num(c.value)} ${SIGNAL[c.mode].unit}</small>`
      : `<span class="state">${num(engValue(c))} ${escapeHtml(c.unit)}</span> <small class="raw">${num(c.value)} ${SIGNAL[c.mode].unit}</small>`;
  }
  const on = buzzerActive(c);
  return c.trigger === 'remote' ? toggleSwitch(c, 'Simulate buzzer remote command', c.value) + `<span class="state-text ${on ? 'on' : ''}">${on ? 'Sounding' : 'Silent'}</span>`
    : `<span class="state ${on ? 'on' : ''}"><i class="dot"></i>${on ? 'Sounding' : 'Silent'}</span>`;
}
function renderChannels() {
  $('#channel-rows').innerHTML = config.channels.filter(c => activeFilter === 'all' || c.kind === activeFilter).map(c => `<tr>
      <td><span class="channel-id">${c.id}</span><small class="pin">GP${c.pin}</small></td>
      <td class="channel-name">${escapeHtml(c.label)}</td>
      <td><span class="type-pill ${c.kind}">${icon(KIND[c.kind].icon)}${KIND[c.kind].name}</span></td>
      <td class="config-cell">${escapeHtml(configText(c))}</td>
      <td class="state-cell">${stateCell(c)}</td>
      <td class="row-actions"><button type="button" class="icon-button" data-edit="${c.id}" aria-label="Edit ${c.id}, ${escapeHtml(c.label)}">${icon('pencil')}</button></td>
    </tr>`).join('');
}
function renderOverview() {
  const of = kind => config.channels.filter(c => c.kind === kind);
  const inputs = of('digital-input'), outputs = of('digital-output'), analog = of('analog-input'), bz = of('buzzer')[0];

  $('#di-count').textContent = inputs.filter(c => c.value).length;
  // Two groups with separate S/S commons, as on the hardware (datasheet §5.2.4)
  $('#di-leds').innerHTML = [['Group 1 · S/S0..4', inputs.slice(0, 5)], ['Group 2 · S/S5..10', inputs.slice(5)]].map(([title, group]) =>
    `<div><span class="led-group-label">${title}</span><div class="leds">${group.map(c =>
      `<button type="button" class="led ${c.value ? 'on' : ''}" data-toggle="${c.id}" aria-pressed="${c.value}" aria-label="Simulate ${c.id} ${escapeHtml(c.label)}" title="${c.id} · ${escapeHtml(c.label)}"><i></i>${c.id.slice(2)}</button>`).join('')}</div></div>`).join('');

  $('#do-count').textContent = outputs.filter(c => c.value).length;
  $('#do-tiles').innerHTML = outputs.map(c =>
    `<button type="button" class="do-tile" role="switch" aria-checked="${c.value}" data-toggle="${c.id}" aria-label="${c.id} ${escapeHtml(c.label)} (preview)" title="${escapeHtml(c.label)}"><span class="do-id">${c.id}</span><span class="do-state">${c.value ? 'On' : 'Off'}</span></button>`).join('');

  $('#analog-mini').innerHTML = analog.map(c => {
    const s = SIGNAL[c.mode]; const fault = analogFault(c.mode, c.value);
    const pct = clampPct((c.value - s.lo) / (s.hi - s.lo) * 100);
    return `<div class="analog-value">
      <div class="analog-head"><span>${c.id} · GP${c.pin}</span><span class="badge">${s.label}</span></div>
      <b>${fault ? '—' : num(engValue(c))}<small>${escapeHtml(c.unit)}</small></b>
      <span class="analog-name">${escapeHtml(c.label)}${fault ? ` <span class="badge danger">${icon('alert')}${fault}</span>` : ''}</span>
      <div class="bar ${fault ? 'fault' : ''}" role="meter" aria-label="${c.id} signal" aria-valuemin="${s.lo}" aria-valuemax="${s.hi}" aria-valuenow="${c.value}"><i style="width:${pct}%"></i></div>
      <div class="scale"><span>${s.lo}</span><span>${num(c.value)} ${s.unit}</span><span>${s.hi}</span></div>
    </div>`;
  }).join('');

  const sounding = buzzerActive(bz);
  $('#bz-state').textContent = sounding ? 'Sounding' : 'Silent';
  $('#bz-state').classList.toggle('on', sounding);
  $('#bz-summary').innerHTML = `<dl class="kv">
      <div><dt>Trigger</dt><dd>${escapeHtml(triggerText(bz))}</dd></div>
      <div><dt>Tone</dt><dd>${bz.freq} Hz · ${PATTERNS[bz.pattern].label}${bz.silence ? ` · auto-silence ${bz.silence} s` : ''}</dd></div>
      <div><dt>Output</dt><dd>GP11 · PWM passive buzzer</dd></div>
    </dl>${bz.trigger === 'remote' ? `<label class="sim-row">${toggleSwitch(bz, 'Simulate buzzer remote command', bz.value)} Simulate remote command</label>` : ''}`;

  const { mqtt: m, modbus: b, rs485: r } = config;
  $('#mqtt-detail').textContent = m.enabled ? `${m.host}:${m.port}` : 'Publish to a broker on the LAN';
  $('#modbus-detail').textContent = b.enabled ? `Server · port ${b.port} · unit ${b.unit}` : 'Server on Ethernet';
  $('#rs485-detail').textContent = `${points().length} point${points().length === 1 ? '' : 's'} · ${r.baud} 8${r.parity[0]}${r.stopBits}`;
  [['#mqtt-state', m.enabled], ['#modbus-state', b.enabled], ['#rs485-state', r.enabled]].forEach(([sel, on]) => { $(sel).textContent = on ? 'Enabled' : 'Off'; $(sel).classList.toggle('on', on); });
}
let flashId = null;
function renderPoints() {
  $('#pt-empty').hidden = points().length > 0;
  $('#pt-table').hidden = points().length === 0;
  $('#rs485-points').innerHTML = points().map(p => `<tr class="${p.id === flashId ? 'flash' : ''}">
      <td><span class="channel-id">${p.id}</span><small class="pin">RS485</small></td>
      <td class="channel-name">${escapeHtml(p.name)}</td>
      <td class="config-cell">${escapeHtml(pointSource(p))}</td>
      <td>${FC[p.fn].bool ? '<span class="muted">—</span>' : `×${num(p.scale)}`} ${escapeHtml(p.unit)}</td>
      <td><code class="ref">${pointRef(p)}</code></td>
      <td><code class="topic-code">${escapeHtml(topicOf(p))}</code></td>
      <td>${pointValue(p)}</td>
      <td class="row-actions"><span class="actions"><button type="button" class="icon-button" data-pt-edit="${p.id}" aria-label="Edit ${p.id} ${escapeHtml(p.name)}">${icon('pencil')}</button><button type="button" class="icon-button danger" data-pt-delete="${p.id}" aria-label="Delete ${p.id} ${escapeHtml(p.name)}">${icon('trash')}</button></span></td>
    </tr>`).join('');
  flashId = null;
}
// Live value from the controller when this page comes from it; a dash in the preview.
function pointValue(p) {
  const live = device.status?.rs485?.find(x => x.id === p.id);
  if (!live) return `<span class="muted" title="Live values show when this page is opened from the controller">—</span>`;
  return live.value === null ? '<span class="badge danger">no reply</span>' : `${num(live.value)} ${escapeHtml(p.unit)}`;
}
function renderMaps() {
  const idCell = c => `<td><span class="channel-id">${c.id}</span><small class="pin">${c.kind === 'rs485' ? 'RS485' : `GP${c.pin}`}</small></td><td class="channel-name">${escapeHtml(c.label ?? c.name)}</td>`;
  const topicInput = (c, field) => `<input class="table-input topic-input" data-ch="${c.id}" data-set="${field}" value="${escapeHtml(c[field] || '')}" placeholder="${escapeHtml(defaultTopic(c, field))}" aria-label="${c.id} ${field === 'topic' ? 'state' : 'command'} topic" spellcheck="false" autocomplete="off">`;
  $('#mqtt-map').innerHTML = owners().map(c => `<tr>${idCell(c)}
      <td>${topicInput(c, 'topic')}</td>
      <td>${hasCmd(c) ? topicInput(c, 'cmdTopic') : `<span class="muted">${c.kind === 'rs485' ? 'Read only' : 'Input only'}</span>`}</td>
      <td><input type="checkbox" class="table-check" data-ch="${c.id}" data-set="mqtt" ${c.mqtt ? 'checked' : ''} aria-label="Publish ${c.id} over MQTT"></td>
    </tr>`).join('');

  $('#modbus-map').innerHTML = modbusRows().map(r => {
    const t = REG[r.table];
    const pin = r.source.startsWith('P') ? 'RS485' : r.source === 'IOC' ? '' : `GP${byId(r.source).pin}`;
    return `<tr><td><span class="channel-id">${r.source}</span><small class="pin">${pin}</small></td><td class="channel-name">${escapeHtml(r.name)}</td>
      <td>${t.name} <small class="muted">${t.fc}</small></td><td><code class="ref">${hex(r.address)}</code> <small class="muted">${r.address}</small></td>
      <td><span class="muted">${escapeHtml(r.format)}</span></td><td><code class="ref">${plcRef(r.table, r.address, r.width)}</code></td></tr>`;
  }).join('');
}
function refresh() { renderChannels(); renderOverview(); }

function toggleValue(id) {
  const c = byId(id);
  if (!c || !['digital-input', 'digital-output', 'buzzer'].includes(c.kind) || (c.kind === 'buzzer' && c.trigger !== 'remote')) return;
  if (c.counter) { toast(`${c.id} is counting pulses; turn the counter off to use it as an on/off input`); return; }
  if (device.live) { toast(c.kind === 'digital-input' ? `${c.id} is live: switch it at the terminal` : 'Live values: switch outputs and the buzzer from SCADA (Modbus) or MQTT'); return; }
  // keep keyboard focus on the same control after re-render
  const container = document.activeElement?.dataset?.toggle === id ? document.activeElement.closest('[id]').id : null;
  c.value = !c.value;
  persist(c.kind === 'digital-input' ? `${c.id} simulated ${c.value ? 'active' : 'inactive'}` : `${c.label} ${c.value ? 'on' : 'off'} · preview only`);
  refresh();
  if (container) $(`#${container} [data-toggle="${id}"]`)?.focus();
}

/* ---------- Channel edit dialog ---------- */
const field = (label, control, cls = '', attrs = '') => `<label class="field ${cls}" ${attrs}><span>${label}</span>${control}</label>`;
const section = (title, body) => `<fieldset class="fs"><legend>${title}</legend><div class="form-grid">${body}</div></fieldset>`;
function openEdit(id) {
  const c = selectedChannel = config.channels.find(x => x.id === id);
  if (!c) return;
  $('#dialog-title').textContent = `${c.id} · ${KIND[c.kind].name}`;
  const help = {
    'digital-input': `Isolated input on GP${c.pin}, reads high at 3–50 V.`,
    'digital-output': `Solid-state dry contact on GP${c.pin}, max 50 V AC/DC at 500 mA. Add a flyback diode across relay coils.`,
    'analog-input': `12-bit input on GP${c.pin}. Full scale 10.56 V in voltage mode, 42.58 mA in current mode.`,
    buzzer: 'Passive buzzer on GP11, driven by PWM so the pitch is adjustable.'
  }[c.kind];
  const parts = [section('General', field('Channel name', `<input id="edit-name" maxlength="32" value="${escapeHtml(c.label)}">`, 'span-2') + `<p class="edit-help span-2">${help}</p>`)];

  if (c.kind === 'analog-input') parts.push(section('Signal & scaling',
    field('Input mode', `<select id="edit-mode">${options(Object.entries(SIGNAL).map(([k, s]) => [k, s.label]), c.mode)}</select>`) +
    field('Engineering unit', `<input id="edit-unit" maxlength="8" value="${escapeHtml(c.unit)}" placeholder="bar, °C, %…">`) +
    field('Scale low <small>at min signal</small>', `<input id="edit-lo" type="number" step="any" required value="${c.scaleLo}">`) +
    field('Scale high <small>at max signal</small>', `<input id="edit-hi" type="number" step="any" required value="${c.scaleHi}">`) +
    field('Simulated signal <small id="sig-unit"></small>', `<input id="edit-value" type="number" step="any" min="0" required value="${c.value}">`) +
    `<div class="field"><span>Scaled reading</span><output id="an-preview" class="preview-out"></output></div>` +
    `<p class="warn span-2">${icon('alert')}<span id="dip-note"></span></p>`));

  if (c.kind === 'buzzer') {
    const ins = config.channels.filter(x => x.kind === 'digital-input'), ans = config.channels.filter(x => x.kind === 'analog-input');
    parts.push(section('Assignment & tone',
      field('Sound the buzzer when', `<select id="edit-trigger"><option value="remote"${c.trigger === 'remote' ? ' selected' : ''}>Remote command only (MQTT / Modbus)</option><optgroup label="A digital input is active">${options(ins.map(x => [x.id, `${x.id} · ${x.label}`]), c.trigger)}</optgroup><optgroup label="An analog alarm trips">${options(ans.map(x => [x.id, `${x.id} · ${x.label}`]), c.trigger)}</optgroup></select>`, 'span-2') +
      field('Condition', `<select id="edit-compare">${options([['above', 'Above threshold'], ['below', 'Below threshold']], c.compare)}</select>`, '', 'data-an-only') +
      field('Threshold <small id="th-unit"></small>', `<input id="edit-threshold" type="number" step="any" value="${c.threshold}">`, '', 'data-an-only') +
      field('Frequency <small>Hz</small>', `<input id="edit-freq" type="number" required min="100" max="10000" step="1" value="${c.freq}">`) +
      field('Pattern', `<select id="edit-pattern">${options(Object.entries(PATTERNS).map(([k, p]) => [k, p.label]), c.pattern)}</select>`) +
      field('Auto-silence <small>seconds, 0 = until cleared</small>', `<input id="edit-silence" type="number" min="0" max="3600" step="1" value="${c.silence}">`) +
      `<div class="field"><span>Preview</span><button type="button" class="button button-secondary" data-test-tone="dialog">${icon('volume')}Test tone</button></div>` +
      `<p class="edit-help span-2">Remote commands on the buzzer's MQTT topic or Modbus coil always work, whichever trigger you choose here.</p>`));
  }

  if ('counter' in c) parts.push(section('Pulse counter',
    `<label class="check-field span-2"><input id="edit-counter" type="checkbox" ${c.counter ? 'checked' : ''}> Count pulses on ${c.id} instead of reading it as on/off</label>` +
    `<p class="edit-help span-2">Counts falling edges up to 5 kHz (datasheet), e.g. a flow or energy meter. The count is on Modbus ${plcRef('input', MB.COUNT + 2 * COUNTER_INPUTS.indexOf(c.pin), 2)} and on MQTT. SCADA can also switch counting on and off with the counter-enable coil.</p>`));

  parts.push(section('MQTT',
    `<label class="check-field span-2"><input id="edit-mqtt" type="checkbox" ${c.mqtt ? 'checked' : ''}> Publish this channel over MQTT</label>` +
    field('State topic <small>publish</small>', `<input id="edit-topic" value="${escapeHtml(c.topic)}" placeholder="${escapeHtml(defaultTopic(c, 'topic'))}" spellcheck="false" autocomplete="off">`, 'span-2') +
    (hasCmd(c) ? field('Command topic <small>subscribe</small>', `<input id="edit-cmd" value="${escapeHtml(c.cmdTopic)}" placeholder="${escapeHtml(defaultTopic(c, 'cmdTopic'))}" spellcheck="false" autocomplete="off">`, 'span-2') : '')));

  const rows = modbusRows().filter(r => r.source === c.id);
  parts.push(section('Modbus TCP',
    `<ul class="reg-list span-2">${rows.map(r => `<li><code class="ref">${plcRef(r.table, r.address, r.width)}</code><span>${escapeHtml(r.name)}<small>${REG[r.table].name} ${hex(r.address)} · ${escapeHtml(r.format)}</small></span></li>`).join('')}</ul>` +
    `<p class="edit-help span-2">Addresses are fixed and match Cytron's IRIV IOC firmware, so SCADA tags never move.</p>`));

  $('#edit-fields').innerHTML = parts.join('');
  syncDialog();
  $('#edit-dialog').showModal();
  $('#edit-name').select();
}
// Live hints inside the dialog: scaled value, DIP reminder, threshold visibility, PLC reference
function syncDialog() {
  const c = selectedChannel;
  if (!c) return;
  const draft = { ...c };
  if (c.kind === 'analog-input') {
    Object.assign(draft, { mode: $('#edit-mode').value, unit: $('#edit-unit').value.trim() || SIGNAL[$('#edit-mode').value].unit, scaleLo: Number($('#edit-lo').value), scaleHi: Number($('#edit-hi').value) });
    const s = SIGNAL[draft.mode], raw = Number($('#edit-value').value), fault = analogFault(draft.mode, raw);
    $('#sig-unit').textContent = `${s.unit}, ${s.lo}–${s.hi}`;
    $('#an-preview').textContent = fault || `${num(engValue(draft, raw))} ${draft.unit}`;
    $('#an-preview').classList.toggle('fault', !!fault);
    $('#dip-note').textContent = `Set the AN-IN MODE switch for ${c.id} (under the controller) to ${s.dip}, or the reading will be wrong.`;
  }
  if (c.kind === 'buzzer') {
    const src = byId($('#edit-trigger').value);
    $$('[data-an-only]').forEach(el => { el.hidden = src?.kind !== 'analog-input'; });
    if (src?.kind === 'analog-input') $('#th-unit').textContent = src.unit;
  }
}
$('#edit-fields').addEventListener('input', e => { e.target.setCustomValidity?.(''); syncDialog(); });
$('#edit-fields').addEventListener('change', syncDialog);
$('#edit-form').addEventListener('submit', e => {
  e.preventDefault(); // native required/min/max checks have already passed when this fires
  const c = selectedChannel;
  if (!c) return;
  const topicEl = $('#edit-topic'), cmdEl = $('#edit-cmd');
  const checks = [[topicEl, topicError(c, 'topic', topicEl.value)]];
  if (cmdEl) {
    const same = (topicEl.value.trim() || defaultTopic(c, 'topic')) === (cmdEl.value.trim() || defaultTopic(c, 'cmdTopic'));
    checks.push([cmdEl, topicError(c, 'cmdTopic', cmdEl.value) || (same ? 'Command topic must differ from the state topic' : '')]);
  }
  if (c.kind === 'analog-input') checks.push([$('#edit-hi'), Number($('#edit-hi').value) === Number($('#edit-lo').value) ? 'Scale high must differ from scale low' : '']);
  checks.forEach(([el, err]) => el.setCustomValidity(err));
  const bad = checks.find(([, err]) => err);
  if (bad) { bad[0].reportValidity(); return; }

  c.label = $('#edit-name').value.trim() || c.id;
  c.mqtt = $('#edit-mqtt').checked;
  if ('counter' in c) c.counter = $('#edit-counter').checked;
  c.topic = topicEl.value.trim();
  if (cmdEl) c.cmdTopic = cmdEl.value.trim();
  if (c.kind === 'analog-input') {
    const mode = $('#edit-mode').value;
    Object.assign(c, { mode, unit: $('#edit-unit').value.trim() || SIGNAL[mode].unit, scaleLo: Number($('#edit-lo').value), scaleHi: Number($('#edit-hi').value), value: Number($('#edit-value').value) });
  }
  if (c.kind === 'buzzer') Object.assign(c, { trigger: $('#edit-trigger').value, compare: $('#edit-compare').value, threshold: Number($('#edit-threshold').value) || 0, freq: Number($('#edit-freq').value), pattern: $('#edit-pattern').value, silence: Number($('#edit-silence').value) || 0 });

  $('#edit-dialog').close(); selectedChannel = null; persist(`${c.id} updated`);
  refresh(); renderMaps(); // not fillConnectionForm: the protocol form may hold unsaved edits
  $(`#channel-rows [data-edit="${c.id}"]`)?.focus();
});
$('#edit-dialog').addEventListener('click', e => { if (e.target === $('#edit-dialog')) $('#edit-dialog').close(); });

/* ---------- RS485 points ---------- */
const ptForm = $('#pt-form');
function syncPointForm() {
  const fn = $('#pt-fn').value, bool = FC[fn].bool, typeSel = $('#pt-type'), current = typeSel.value;
  typeSel.innerHTML = bool ? '<option value="bool">bool · 1 bit</option>' : options(Object.entries(TYPES), current in TYPES ? current : 'int16');
  $('#pt-scale').disabled = bool;
  const target = editingPoint ? pointRef(editingPoint) : tcpSpaceLeft() ? plcRef('input', nextTcp(), 2) : null;
  $('#pt-preview').innerHTML = target
    ? `${icon('arrow-right')}<span>${editingPoint ? 'Keeps' : 'Will get'} Modbus TCP <code>${target}</code> and MQTT <code>${escapeHtml(topicOf(editingPoint || { kind: 'rs485', id: nextPointId() }))}</code></span>`
    : `${icon('alert')}<span>No Modbus TCP space left: the gateway serves up to 64 RS485 points</span>`;
}
function resetPointForm() {
  ptForm.reset(); editingPoint = null; $('#pt-type').innerHTML = ''; // back to int16
  $('#pt-submit span').textContent = 'Add point'; $('#pt-cancel').hidden = true;
  $$('input', ptForm).forEach(el => el.setCustomValidity(''));
  $('#pt-test').hidden = true; // a new form starts without an old result
  syncPointForm();
}
function editPoint(id) {
  const p = points().find(x => x.id === id);
  if (!p) return;
  editingPoint = p;
  $('#pt-name').value = p.name; $('#pt-slave').value = p.slave; $('#pt-fn').value = p.fn; $('#pt-reg').value = p.reg; $('#pt-scale').value = p.scale; $('#pt-unit').value = p.unit;
  syncPointForm(); $('#pt-type').value = p.type;
  $('#pt-submit span').textContent = `Save ${p.id}`; $('#pt-cancel').hidden = false;
  $('#rs485-panel').scrollIntoView({ behavior: 'smooth', block: 'start' });
  $('#pt-name').focus({ preventScroll: true });
}
ptForm.addEventListener('input', e => { e.target.setCustomValidity?.(''); });
$('#pt-fn').addEventListener('change', syncPointForm);
$('#pt-cancel').addEventListener('click', resetPointForm);
ptForm.addEventListener('submit', e => {
  e.preventDefault();
  const regEl = $('#pt-reg');
  const draft = { name: $('#pt-name').value.trim(), slave: Number($('#pt-slave').value), fn: $('#pt-fn').value, reg: Number(regEl.value), type: FC[$('#pt-fn').value].bool ? 'bool' : $('#pt-type').value || 'int16', scale: FC[$('#pt-fn').value].bool ? 1 : Number($('#pt-scale').value) || 1, unit: $('#pt-unit').value.trim() };
  const dup = points().find(p => p !== editingPoint && p.slave === draft.slave && p.fn === draft.fn && p.reg === draft.reg);
  const err = dup ? `${dup.id} (${dup.name}) already reads slave ${dup.slave} ${FC[dup.fn].short} register ${dup.reg}`
    : ['int32', 'uint32', 'float32'].includes(draft.type) && draft.reg > 65534 ? '32-bit values need two registers; use 65534 or lower' : '';
  regEl.setCustomValidity(err);
  if (err) { regEl.reportValidity(); return; }
  if (!draft.name) { $('#pt-name').setCustomValidity('Enter a name'); $('#pt-name').reportValidity(); return; }

  let p = editingPoint;
  if (p) Object.assign(p, draft);
  else {
    if (!tcpSpaceLeft()) { toast('No Modbus TCP space left: the gateway serves up to 64 RS485 points'); return; }
    p = { id: nextPointId(), kind: 'rs485', ...draft, tcp: nextTcp(), topic: '', mqtt: true }; points().push(p);
  }
  persist(`${p.id} ${editingPoint ? 'saved' : 'added'} → Modbus TCP ${pointRef(p)} · MQTT ${topicOf(p)}`);
  flashId = p.id;
  resetPointForm(); renderPoints(); renderMaps(); renderOverview();
  $('#pt-name').focus();
});
/* ---------- RS485: test a sensor before adding it ---------- */
// Same word handling as decode() in firmware/lib/IRIVModbus (IRIVModbusCore.cpp).
function decodeRegs(regs, type, order) {
  if (type === 'bool') return regs[0] ? 1 : 0;
  if (type === 'int16') return regs[0] << 16 >> 16;
  if (type === 'uint16') return regs[0];
  let [high, low] = order === 'CDAB' || order === 'DCBA' ? [regs[1], regs[0]] : [regs[0], regs[1]];
  if (order === 'BADC' || order === 'DCBA') { const swap = w => ((w & 0xff) << 8) | (w >> 8); high = swap(high); low = swap(low); }
  const view = new DataView(new ArrayBuffer(4));
  view.setUint16(0, high); view.setUint16(2, low);
  return type === 'float32' ? view.getFloat32(0) : type === 'int32' ? view.getInt32(0) : view.getUint32(0);
}
// Reads the point in the form once, with the bus settings on this page (saved or not), through the controller:
// over the LAN when this page comes from the controller, or over USB after Connect on the Install page.
async function testPoint() {
  const fn = $('#pt-fn').value, bool = FC[fn].bool, type = bool ? 'bool' : $('#pt-type').value || 'int16';
  const q = {
    slave: Number($('#pt-slave').value), fn: { coil: 1, discrete: 2, holding: 3, input: 4 }[fn], reg: Number($('#pt-reg').value),
    count: ['int32', 'uint32', 'float32'].includes(type) ? 2 : 1,
    baud: $('#rs485-baud').value, parity: $('#rs485-parity').value, stop: $('#rs485-stop').value, timeout: $('#rs485-timeout').value || '500'
  };
  const out = $('#pt-test');
  const show = (html, cls) => { out.hidden = false; out.className = `test-result ${cls}`; out.innerHTML = html; };
  const usb = typeof link !== 'undefined' && link; // install.js: the open USB connection
  if (!device.live && !usb) {
    show(`${icon('info')}<span>Connect to the controller first: open this page from the controller (<code>http://its-IP/</code>), or click <b>Connect over USB</b> on the <a href="#install">Install</a> page.</span>`, 'info');
    return;
  }
  show(`${icon('clock')}<span>Reading slave ${q.slave} ${FC[fn].short} register ${q.reg} at ${q.baud} baud…</span>`, 'info');
  let r;
  try {
    r = device.live
      ? await (await fetch(`api/rs485/read?${new URLSearchParams({ ...q, parity: q.parity })}`, { cache: 'no-store' })).json()
      : await usb.request(`RTUREAD ${q.slave} ${q.fn} ${q.reg} ${q.count} ${q.baud} ${q.parity[0]} ${q.stop} ${q.timeout}`, ['rturead'], Number(q.timeout) + 3000);
  } catch (e) { show(`${icon('alert')}<span>${escapeHtml(e.message)}</span>`, 'bad'); return; }
  if (!r.ok) {
    show(`${icon('alert')}<span><b>No good answer from slave ${q.slave}:</b> ${escapeHtml(r.error || 'no reply')}${r.exception ? ` (exception ${r.exception})` : ''}. Check the slave ID, wiring (A, B, G), baud rate and parity.</span>`, 'bad');
    return;
  }
  const scale = bool ? 1 : Number($('#pt-scale').value) || 1;
  const value = decodeRegs(r.regs, type, $('#rs485-word').value) * scale;
  const raw = r.regs.map(w => `0x${w.toString(16).toUpperCase().padStart(4, '0')}`).join(' ');
  show(`${icon('check')}<span><b>Slave ${q.slave} answered: ${num(value)} ${escapeHtml($('#pt-unit').value.trim())}</b> <small class="muted">raw ${raw} · ${type}${scale !== 1 ? ` × ${num(scale)}` : ''}</small>. Add it to give it its Modbus TCP address and MQTT topic.</span>`, 'ok');
}
$('#pt-test-btn').addEventListener('click', () => testPoint());
function deletePoint(id) {
  const p = points().find(x => x.id === id);
  if (!p || !confirm(`Delete ${p.id} (${p.name})? Modbus TCP ${pointRef(p)} will no longer be served.`)) return;
  config.rs485.points = points().filter(x => x !== p);
  if (editingPoint === p) resetPointForm();
  persist(`${p.id} deleted`); renderPoints(); renderMaps(); renderOverview(); syncPointForm();
}

/* ---------- Connections form ---------- */
function fillConnectionForm() {
  const n = config.network, m = config.mqtt, b = config.modbus, r = config.rs485;
  $('#net-dhcp').checked = n.dhcp; $('#net-ip').value = n.ip; $('#net-subnet').value = n.subnet; $('#net-gateway').value = n.gateway; $('#net-dns').value = n.dns;
  $('#mqtt-enabled').checked = m.enabled; $('#mqtt-host').value = m.host; $('#mqtt-port').value = m.port; $('#mqtt-tls').checked = m.tls; $('#mqtt-ca').value = m.ca; $('#mqtt-client').value = m.client; $('#mqtt-topic').value = m.topic; $('#mqtt-user').value = m.user; $('#mqtt-pass').value = m.pass;
  $('#mqtt-qos').value = m.qos; $('#mqtt-retain').checked = m.retain; $('#mqtt-interval').value = m.interval;
  $('#web-pass').value = config.web.password;
  $('#modbus-enabled').checked = b.enabled; $('#modbus-port').value = b.port; $('#modbus-unit').value = b.unit; $('#modbus-word').value = b.wordOrder;
  $('#rs485-enabled').checked = r.enabled; $('#rs485-baud').value = r.baud; $('#rs485-parity').value = r.parity; $('#rs485-stop').value = r.stopBits; $('#rs485-poll').value = r.poll; $('#rs485-timeout').value = r.timeout; $('#rs485-word').value = r.wordOrder;
  syncNetworkFields(); syncMqttRequired(); updateTopicPreview(); setDirty(false);
}
function readConnectionForm() {
  return {
    network: { dhcp: $('#net-dhcp').checked, ip: $('#net-ip').value.trim(), subnet: $('#net-subnet').value.trim(), gateway: $('#net-gateway').value.trim(), dns: $('#net-dns').value.trim() },
    mqtt: { enabled: $('#mqtt-enabled').checked, host: $('#mqtt-host').value.trim(), port: $('#mqtt-port').value || '1883', tls: $('#mqtt-tls').checked, ca: $('#mqtt-ca').value.trim(), client: $('#mqtt-client').value.trim(), topic: $('#mqtt-topic').value.trim(), user: $('#mqtt-user').value, pass: $('#mqtt-pass').value, qos: $('#mqtt-qos').value, retain: $('#mqtt-retain').checked, interval: $('#mqtt-interval').value },
    modbus: { enabled: $('#modbus-enabled').checked, port: $('#modbus-port').value || '502', unit: $('#modbus-unit').value || '255', wordOrder: $('#modbus-word').value },
    rs485: { enabled: $('#rs485-enabled').checked, baud: $('#rs485-baud').value, parity: $('#rs485-parity').value, stopBits: $('#rs485-stop').value, poll: $('#rs485-poll').value, timeout: $('#rs485-timeout').value, wordOrder: $('#rs485-word').value },
    web: { password: $('#web-pass').value }
  };
}
// Broker and topic are only required once MQTT is switched on; the browser then validates them inline
function syncMqttRequired() { const on = $('#mqtt-enabled').checked; $('#mqtt-host').required = on; $('#mqtt-topic').required = on; }
// Static address fields only apply (and are only required) with DHCP off; disabled fields skip validation.
function syncNetworkFields() {
  const fixed = !$('#net-dhcp').checked;
  $('#net-static').hidden = !fixed;
  ['#net-ip', '#net-subnet'].forEach(s => { $(s).required = fixed; });
  ['#net-ip', '#net-subnet', '#net-gateway', '#net-dns'].forEach(s => { $(s).disabled = !fixed; });
}
function updateTopicPreview() { $('#topic-preview').textContent = `${baseTopic($('#mqtt-topic').value)}/di0/state`; }
function setDirty(value) {
  dirty = value; $('#dirty').hidden = !value;
  $('.nav-item[data-view="connections"]').classList.toggle('dirty', value);
}
const connForm = $('#conn-form');
connForm.addEventListener('input', () => setDirty(true));
connForm.addEventListener('change', () => setDirty(true));
connForm.addEventListener('submit', e => {
  e.preventDefault();
  const next = readConnectionForm();
  config.network = next.network; config.mqtt = next.mqtt; config.modbus = next.modbus; config.web = next.web; Object.assign(config.rs485, next.rs485); // keep the RS485 points
  persist('Connection settings saved'); setDirty(false); refresh(); renderMaps(); renderPoints(); syncPointForm();
});
$('#mqtt-enabled').addEventListener('change', syncMqttRequired);
$('#net-dhcp').addEventListener('change', syncNetworkFields);
$('#mqtt-topic').addEventListener('input', updateTopicPreview);
$('#toggle-pass').addEventListener('click', e => {
  const btn = e.currentTarget; const pass = $('#mqtt-pass'); const show = pass.type === 'password';
  pass.type = show ? 'text' : 'password';
  btn.setAttribute('aria-pressed', show); btn.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  btn.innerHTML = icon(show ? 'eye-off' : 'eye');
});
window.addEventListener('beforeunload', e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } });

// Topic map saves per field. Only the changed cell is touched so Tab keeps flowing through the table.
document.addEventListener('change', e => {
  const el = e.target; const key = el.dataset?.set; const c = key && byId(el.dataset.ch);
  if (!c) return;
  const value = el.type === 'checkbox' ? el.checked : el.value.trim();
  const err = key === 'topic' || key === 'cmdTopic' ? topicError(c, key, value) : '';
  if (err) { toast(err); if (el.type === 'checkbox') el.checked = c[key]; else el.value = c[key]; return; }
  c[key] = value;
  persist(`${c.id} ${{ topic: 'state topic', cmdTopic: 'command topic', mqtt: 'MQTT publishing' }[key]} saved`);
  refresh(); if (c.kind === 'rs485') renderPoints();
});

/* ---------- Export: ioc_config.json for the gateway firmware ---------- */
// The saved setup (never the unsaved form) as the file the firmware reads. Also sent by the installer.
function buildConfigFile(includePass) {
  const { mqtt, web, ...rest } = config;
  const map = {
    mqtt: owners().filter(c => c.mqtt).map(c => ({ source: c.id, publish: topicOf(c), ...(hasCmd(c) && { subscribe: topicOf(c, 'cmdTopic') }) })),
    modbusTcp: modbusRows().map(r => ({ source: r.source, name: r.name, table: r.table, address: r.address, registers: r.width, plcReference: plcRef(r.table, r.address, r.width), format: r.format }))
  };
  return { format: 'iriv-ioc-gateway-config', version: 4, exportedAt: new Date().toISOString(), ...rest, mqtt: { ...mqtt, pass: includePass ? mqtt.pass : '' }, web: { password: includePass ? web.password : '' }, map };
}
function exportConfig() {
  const { mqtt, web } = config;
  // The firmware needs the passwords; they are only included when the user agrees.
  const includePass = !!(mqtt.pass || web.password) && confirm('Include the passwords (MQTT broker and web page) in ioc_config.json?\n\nThe controller needs them, but anyone with the file can read them. Choose Cancel to leave them out and add them to the file yourself.');
  const data = buildConfigFile(includePass);
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' }));
  a.download = 'ioc_config.json'; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  toast(`ioc_config.json downloaded${dirty ? ' · unsaved connection edits not included' : ''}${(mqtt.pass || web.password) && !includePass ? ' · passwords left out' : ''}`);
}

/* ---------- On the controller: live values and saving over the LAN ---------- */
// When the controller itself serves this page (http://<its IP>/), api/status answers: the page then loads the
// controller's settings, shows live values every 2 s and saves straight to it. On the website it stays a preview.
const device = { live: false, status: null };
async function connectDevice() {
  let s;
  try {
    const r = await fetch('api/status', { cache: 'no-store' });
    if (!r.ok || !(r.headers.get('content-type') || '').includes('json')) return;
    s = await r.json();
  } catch { return; }
  device.live = true;
  document.body.classList.add('on-device');
  try {
    const r = await fetch('api/config', { cache: 'no-store' });
    if (r.ok) config = normalizeConfig(await r.json()); // what the controller runs, not this browser's copy
  } catch { /* keep the browser's copy */ }
  $('#save-device').hidden = false;
  renderMaps(); renderPoints(); fillConnectionForm();
  applyStatus(s);
  setInterval(async () => {
    try { applyStatus(await (await fetch('api/status', { cache: 'no-store' })).json()); }
    catch { $('#mode-pill').lastChild.textContent = ' Controller not answering'; $('#mode-pill').classList.remove('live'); }
  }, 2000);
  if (typeof startWebInstall === 'function') startWebInstall(s); // install.js: updates from GitHub
}
function applyStatus(s) {
  device.status = s;
  for (const c of config.channels) {
    if (c.kind === 'digital-input') c.value = !!s.io.di[c.pin];
    else if (c.kind === 'digital-output') c.value = !!s.io.do[c.pin - 12];
    else if (c.kind === 'analog-input') c.value = s.io.an[c.address];
    else if (c.kind === 'buzzer' && c.trigger === 'remote') c.value = !!s.io.buzzer;
  }
  if (s.rs485.length) renderPoints(); // live point values
  $('#mode-pill').classList.add('live');
  $('#mode-pill').lastChild.textContent = ` Live · ${s.host} · ${s.ip} · v${s.version}`;
  $('#dev-state').textContent = 'Running';
  $('#dev-state-note').textContent = `Up ${Math.floor(s.uptime / 60)} min · settings on the controller`;
  $('#dev-network').textContent = `${s.ip} · MQTT ${!s.mqtt.enabled ? 'off' : s.mqtt.connected ? 'connected' : 'not connected'}`;
  $('#footer-store').textContent = 'Settings on the controller';
  refresh();
}
async function saveToDevice() {
  if (dirty) { toast('Save the connection settings first'); return; }
  if (!device.status?.webPassword) { toast('No web password on the controller yet: set one under Connections and send the settings over USB once'); return; }
  if (!confirm('Send this setup to the controller? It restarts with the new settings (about 10 s).')) return;
  const r = await fetch('api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(buildConfigFile(true)) });
  if (!r.ok) { toast(r.status === 401 ? 'Wrong password' : `Not saved: ${await r.text()}`); return; }
  const reply = await r.json();
  toast(reply.warning ? `Saved with a warning: ${reply.warning}. Restarting…` : 'Saved on the controller. It is restarting…');
  setTimeout(() => location.reload(), 12000);
}
$('#save-device').addEventListener('click', () => saveToDevice().catch(e => toast(e.message)));

/* ---------- Routing & global events ---------- */
function route(focus = true) {
  const name = location.hash.slice(1);
  if (!VIEWS.includes(name)) return; // e.g. #main from the skip link
  $$('.page').forEach(p => p.classList.toggle('active', p.id === `view-${name}`));
  $$('.nav-item').forEach(n => n.dataset.view === name ? n.setAttribute('aria-current', 'page') : n.removeAttribute('aria-current'));
  document.title = `${name[0].toUpperCase()}${name.slice(1)} · IRIV IOC`;
  window.scrollTo(0, 0);
  if (focus) $(`#view-${name} h1`).focus({ preventScroll: true });
}
window.addEventListener('hashchange', () => route());

document.addEventListener('click', e => {
  const t = e.target.closest('[data-toggle], [data-edit], [data-close], [data-export], [data-test-tone], [data-pt-edit], [data-pt-delete], .notice-close');
  if (!t) return;
  if (t.dataset.toggle) toggleValue(t.dataset.toggle);
  else if (t.dataset.edit) openEdit(t.dataset.edit);
  else if (t.dataset.ptEdit) editPoint(t.dataset.ptEdit);
  else if (t.dataset.ptDelete) deletePoint(t.dataset.ptDelete);
  else if (t.hasAttribute('data-close')) $('#edit-dialog').close();
  else if (t.hasAttribute('data-export')) exportConfig();
  else if (t.dataset.testTone === 'dialog') playTone(Number($('#edit-freq').value) || 2700, $('#edit-pattern').value);
  else if (t.dataset.testTone) { const bz = byId('BZ'); playTone(bz.freq, bz.pattern); }
  else t.closest('.notice').remove();
});
$$('.channel-tab').forEach(btn => btn.addEventListener('click', () => {
  activeFilter = btn.dataset.filter;
  $$('.channel-tab').forEach(b => b.setAttribute('aria-pressed', b === btn));
  renderChannels();
}));
$('#reset-config').addEventListener('click', () => {
  if (!confirm('Reset every pin (names, analog modes, buzzer and topics)? Protocol settings and RS485 points are kept.')) return;
  config.channels = defaults().channels;
  persist('Default channel setup restored'); refresh(); renderMaps();
});

refresh(); renderMaps(); renderPoints(); resetPointForm(); fillConnectionForm(); route(false);
connectDevice();
