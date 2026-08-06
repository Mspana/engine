'use strict';
/* Aristotle Remote — web client.
 *
 * Zero dependencies. All crypto is WebCrypto (X25519 / HKDF-SHA256 / AES-256-GCM),
 * which matches the mbedTLS primitives on the C++ side.
 *
 * NOTE ON SECURE CONTEXT: crypto.subtle only exists in a secure context, which means
 * http://localhost or https://<lan-ip>. Plain http:// to a LAN IP will fail the
 * capability check below and show an explanation rather than breaking silently.
 */

// ============================================================ tiny helpers

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}
function show(node, on) { node.classList.toggle('hidden', !on); }

const TE = new TextEncoder();
const TD = new TextDecoder();

function b64e(buf) {
  const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
  return btoa(s);
}
function b64d(str) {
  const s = atob(String(str));
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}
// Pairing codes are base64url of 32 bytes, possibly without padding.
function b64urlD(str) {
  let s = String(str).trim().replace(/\s+/g, '').replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  return b64d(s);
}
function cat(...parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}
function randomBytes(n) { return crypto.getRandomValues(new Uint8Array(n)); }

// ============================================================ crypto layer

const S = crypto.subtle;

const genEphemeral = () => S.generateKey({ name: 'X25519' }, true, ['deriveBits']);
const importPub = (raw) => S.importKey('raw', raw, { name: 'X25519' }, true, []);
const rawPub = async (key) => new Uint8Array(await S.exportKey('raw', key));

/** X25519 shared secret (32 bytes) between our private key and a raw peer public key. */
async function x25519(priv, peerRaw) {
  const pub = await importPub(peerRaw);
  return new Uint8Array(await S.deriveBits({ name: 'X25519', public: pub }, priv, 256));
}

async function hkdf(ikm, salt, infoStr, outBytes) {
  const key = await S.importKey('raw', ikm, 'HKDF', false, ['deriveBits']);
  const bits = await S.deriveBits(
    { name: 'HKDF', hash: 'SHA-256', salt, info: TE.encode(infoStr) }, key, outBytes * 8);
  return new Uint8Array(bits);
}

const aesKey = (raw) => S.importKey('raw', raw, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']);

/** 12-byte nonce: 4-byte big-endian direction prefix, then the 8-byte BE counter. */
function nonce(prefix, counter) {
  const n = new Uint8Array(12);
  const dv = new DataView(n.buffer);
  dv.setUint32(0, prefix, false);
  dv.setBigUint64(4, BigInt(counter), false);
  return n;
}
const DIR_PAIR = 1, DIR_C2S = 2, DIR_S2C = 3;

const gcmEnc = (key, iv, aad, pt) =>
  S.encrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 128 }, key, pt);
const gcmDec = (key, iv, aad, ct) =>
  S.decrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 128 }, key, ct);

// ============================================================ identity store

const LS_KEY = 'aristotle.remote.identity.v1';
const ident = { priv: null, privB64: null, pubRaw: null, srvPubRaw: null, deviceId: null, name: null };

const isPaired = () => !!(ident.priv && ident.srvPubRaw && ident.deviceId);

async function ensureKeypair() {
  if (ident.priv) return;
  const kp = await genEphemeral();
  ident.priv = kp.privateKey;
  ident.privB64 = b64e(await S.exportKey('pkcs8', kp.privateKey));
  ident.pubRaw = await rawPub(kp.publicKey);
}

async function loadIdentity() {
  const raw = localStorage.getItem(LS_KEY);
  if (!raw) return false;
  try {
    const j = JSON.parse(raw);
    ident.privB64 = j.priv;
    ident.priv = await S.importKey('pkcs8', b64d(j.priv), { name: 'X25519' }, true, ['deriveBits']);
    ident.pubRaw = b64d(j.pub);
    ident.srvPubRaw = j.srv ? b64d(j.srv) : null;
    ident.deviceId = j.device_id || null;
    ident.name = j.name || null;
    log('loaded identity, device_id=' + ident.deviceId);
    return isPaired();
  } catch (e) {
    log('identity load failed: ' + e.message);
    localStorage.removeItem(LS_KEY);
    return false;
  }
}

function persistIdentity() {
  localStorage.setItem(LS_KEY, JSON.stringify({
    priv: ident.privB64,
    pub: b64e(ident.pubRaw),
    srv: ident.srvPubRaw ? b64e(ident.srvPubRaw) : null,
    device_id: ident.deviceId,
    name: ident.name,
  }));
}

function forgetDevice() {
  localStorage.removeItem(LS_KEY);
  location.reload();
}

// ============================================================ connection log

const logLines = [];
function log(msg) {
  const line = new Date().toLocaleTimeString() + '  ' + msg;
  logLines.push(line);
  if (logLines.length > 200) logLines.shift();
  const text = logLines.join('\n');
  for (const box of $$('.log-body')) {
    box.textContent = text;
    box.scrollTop = box.scrollHeight;
  }
}

// ============================================================ wire transport

const WS_PATH = '/ws'; // must match the server's upgrade route
const wsUrl = () => (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + WS_PATH;

let sock = null;
let kC2S = null, kS2C = null;
let sendCtr = 0n, lastRecvCtr = -1n;
let sessionReady = false;
let backoff = 1000;
let reconnectTimer = null;
let pingTimer = null;

function setDot(state) {
  const dot = $('#dot');
  dot.className = 'dot ' + state;
  dot.setAttribute('aria-label', state);
}

/* Both directions are strictly ordered by a counter, and every step here is
   async (WebCrypto returns promises). Without serialization two overlapping
   calls could number frames 0,1 but put 1 on the wire first — which the server
   is required to reject. These chains keep numbering and I/O in lockstep. */
let sendChain = Promise.resolve();
let recvChain = Promise.resolve();
const chain = (prev, fn) => prev.catch(() => {}).then(fn);

/** Encrypt and send one application message as a binary data frame. */
function sendFrame(obj) {
  sendChain = chain(sendChain, () => encryptAndSend(obj));
  return sendChain;
}

async function encryptAndSend(obj) {
  if (!kC2S || !sock || sock.readyState !== WebSocket.OPEN) return false;
  const ctr = sendCtr++;
  const hdr = new Uint8Array(10);
  hdr[0] = 1; // version
  hdr[1] = 1; // type = data
  new DataView(hdr.buffer).setBigUint64(2, ctr, false);
  const ct = await gcmEnc(kC2S, nonce(DIR_C2S, ctr), hdr, TE.encode(JSON.stringify(obj)));
  sock.send(cat(hdr, new Uint8Array(ct)));
  return true;
}

async function onBinaryFrame(bytes) {
  if (!kS2C) return dropSession('data frame before the handshake finished');
  // 10 header + at least the 16-byte tag.
  if (bytes.length < 26) return dropSession('short frame');
  if (bytes[0] !== 1 || bytes[1] !== 1) return dropSession('bad frame header');

  const hdr = bytes.slice(0, 10);
  const ctr = new DataView(hdr.buffer, hdr.byteOffset, 10).getBigUint64(2, false);
  // Strictly increasing: blocks replay and reordering.
  if (ctr <= lastRecvCtr) return dropSession('stale counter ' + ctr);

  let plain;
  try {
    plain = await gcmDec(kS2C, nonce(DIR_S2C, ctr), hdr, bytes.slice(10));
  } catch (e) {
    return dropSession('frame decrypt failed');
  }
  lastRecvCtr = ctr;

  let msg;
  try { msg = JSON.parse(TD.decode(plain)); } catch (e) { return dropSession('bad frame JSON'); }
  handleApp(msg);
}

/** Fatal protocol problem: tear the socket down and let the backoff loop retry. */
function dropSession(reason) {
  log('session dropped: ' + reason);
  // Browsers only allow 1000 or 3000-4999 from script, so 1002 would throw.
  if (sock) { try { sock.close(4001, 'protocol'); } catch (e) { /* already closing */ } }
}

// ---------------------------------------------------------- session handshake

async function connect() {
  if (!isPaired() || sock) return;
  clearTimeout(reconnectTimer);
  reconnectTimer = null;

  setDot('connecting');
  sessionReady = false;
  kC2S = kS2C = null;
  sendCtr = 0n;
  lastRecvCtr = -1n;
  sendChain = recvChain = Promise.resolve();

  const eph = await genEphemeral();
  const ephPubRaw = await rawPub(eph.publicKey);
  const nonceC = randomBytes(32);

  const s = new WebSocket(wsUrl());
  s.binaryType = 'arraybuffer';
  sock = s;
  log('connecting to ' + wsUrl());

  s.onopen = () => {
    log('socket open, sending hello');
    s.send(JSON.stringify({
      t: 'hello',
      dev_pk: b64e(ident.pubRaw),
      eph_pk: b64e(ephPubRaw),
      nonce_c: b64e(nonceC),
    }));
  };

  // Queued rather than handled inline, so hello_ack finishes deriving the keys
  // before the `ready` frame that may already be sitting behind it.
  s.onmessage = (ev) => {
    recvChain = chain(recvChain, async () => {
      if (sock !== s) return; // late event from a socket we already replaced
      try {
        if (typeof ev.data !== 'string') return await onBinaryFrame(new Uint8Array(ev.data));
        const m = JSON.parse(ev.data);
        if (m.t === 'hello_ack') {
          await deriveSession(eph.privateKey, b64d(m.eph_pk), nonceC, b64d(m.nonce_s));
        } else if (m.t === 'error') {
          log('server error: ' + m.message);
          onHandshakeError(String(m.message || ''));
        } else {
          log('unexpected text message: ' + m.t);
        }
      } catch (e) {
        dropSession('handler threw: ' + e.message);
      }
    });
  };

  s.onclose = (ev) => {
    log('socket closed (' + ev.code + ')');
    if (sock === s) {
      sock = null;
      sessionReady = false;
      kC2S = kS2C = null;
      clearInterval(pingTimer);
      setDot('offline');
      applyGates();
      scheduleReconnect();
    }
  };
  s.onerror = () => log('socket error');
}

/** Noise-KK-shaped mix: ee || es || se || ss -> HKDF -> k_c2s || k_s2c. */
async function deriveSession(ephPriv, ephPubS, nonceC, nonceS) {
  const ee = await x25519(ephPriv, ephPubS);
  const es = await x25519(ephPriv, ident.srvPubRaw);
  const se = await x25519(ident.priv, ephPubS);
  const ss = await x25519(ident.priv, ident.srvPubRaw);
  const okm = await hkdf(cat(ee, es, se, ss), cat(nonceC, nonceS),
    'aristotle-remote-session-v1', 64);
  kC2S = await aesKey(okm.slice(0, 32));
  kS2C = await aesKey(okm.slice(32, 64));
  log('session keys derived, sending hello_verify');
  // The first frame that decrypts on the server proves we hold the static key.
  await sendFrame({ t: 'hello_verify' });
}

function onHandshakeError(message) {
  // The server no longer knows us (revoked, or its key store was reset): the only
  // recovery is a fresh pairing, so drop back to screen 1 rather than looping.
  if (/unknown|unpaired|not paired|revoked|no such device/i.test(message)) {
    localStorage.removeItem(LS_KEY);
    ident.srvPubRaw = null;
    ident.deviceId = null;
    showPairScreen('This device is no longer paired with the editor. Pair again to continue.');
  }
}

function scheduleReconnect() {
  if (reconnectTimer || !isPaired()) return;
  const delay = Math.min(30000, backoff) + Math.floor(Math.random() * 300);
  log('reconnecting in ' + Math.round(delay / 1000) + 's');
  reconnectTimer = setTimeout(() => { reconnectTimer = null; connect(); }, delay);
  backoff = Math.min(30000, backoff * 2);
}

function onReady() {
  sessionReady = true;
  backoff = 1000;
  setDot('online');
  log('session ready');
  // Re-sync everything: state, transcript and chat list.
  sendFrame({ t: 'get_state' });
  sendFrame({ t: 'get_history' });
  sendFrame({ t: 'list_chats' });
  clearInterval(pingTimer);
  pingTimer = setInterval(() => sendFrame({ t: 'ping' }), 25000);
  applyGates();
}

// ============================================================ pairing

async function doPair(codeText, deviceName) {
  const P = b64urlD(codeText);
  if (P.length !== 32) throw new Error('That pairing code looks wrong (expected 32 bytes).');
  await ensureKeypair();

  return new Promise((resolve, reject) => {
    const s = new WebSocket(wsUrl());
    s.binaryType = 'arraybuffer';
    let srvPub = null;
    let settled = false;
    const finish = (err, val) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      try { s.close(); } catch (e) { /* ignore */ }
      err ? reject(err) : resolve(val);
    };
    const timeout = setTimeout(() => finish(new Error('Pairing timed out.')), 20000);

    s.onopen = () => {
      log('pairing: socket open');
      s.send(JSON.stringify({ t: 'pair_hello', dev_pk: b64e(ident.pubRaw), name: deviceName }));
    };
    s.onerror = () => log('pairing: socket error');
    s.onclose = () => finish(new Error('Connection closed before pairing completed.'));

    s.onmessage = async (ev) => {
      try {
        const m = JSON.parse(String(ev.data));
        if (m.t === 'pair_ack') {
          srvPub = b64d(m.srv_pk);
          const salt = b64d(m.salt);
          // Mixing the pairing secret P into the IKM is what authenticates this
          // exchange — an attacker who swaps keys still cannot derive k.
          const shared = await x25519(ident.priv, srvPub);
          const k = await aesKey(await hkdf(cat(shared, P), salt, 'aristotle-remote-pair-v1', 32));
          const pt = cat(TE.encode('pair-confirm'), ident.pubRaw, srvPub);
          const proof = await gcmEnc(k, nonce(DIR_PAIR, 0), new Uint8Array(0), pt);
          log('pairing: sending proof');
          s.send(JSON.stringify({ t: 'pair_confirm', proof: b64e(proof) }));
        } else if (m.t === 'pair_ok') {
          ident.srvPubRaw = srvPub;
          ident.deviceId = String(m.device_id);
          ident.name = deviceName;
          persistIdentity();
          log('paired as ' + ident.deviceId);
          finish(null, true);
        } else if (m.t === 'error') {
          finish(new Error(m.message || 'Pairing rejected.'));
        }
      } catch (e) {
        finish(e);
      }
    };
  });
}

// ============================================================ app state

let canControl = false;      // read-only until the desktop editor grants control
let running = false;
let pendingApproval = null;
let chatId = '';
let chatTitles = new Map();
let statusText = '';
const POLICY = ['ask', 'auto', 'plan'];

function handleApp(m) {
  switch (m.t) {
    case 'ready':      onReady(); break;
    case 'state':
      chatId = m.chat_id || '';
      canControl = !!m.can_control;
      running = !!m.running;
      statusText = m.status || '';
      setApproval(m.approval);
      updateHeader(m.policy_mode);
      applyGates();
      break;
    case 'history':
      chatId = m.chat_id || chatId;
      renderHistory(Array.isArray(m.items) ? m.items : []);
      updateHeader();
      break;
    case 'chats':      renderChats(Array.isArray(m.chats) ? m.chats : []); break;
    // A rewind/reload invalidated our copy of the transcript.
    case 'history_invalidated': refetchHistory(); break;
    case 'chat_switched':
      chatId = m.chat_id || chatId;
      refetchHistory();               // coalesces with the invalidation that follows
      sendFrame({ t: 'list_chats' });
      updateHeader();
      break;
    case 'policy_mode': updateHeader(m.mode | 0); break;
    case 'item':       onItem(m.item); break;
    case 'delta':      onDelta(m.kind, m.text || ''); break;
    case 'run_state':
      running = !!m.running;
      if (!running) clearLive('thinking');
      applyGates();
      break;
    case 'approval':   setApproval(m.info); applyGates(); break;
    case 'status':     statusText = m.text || ''; updateHeader(); break;
    case 'error':      toastError(m.message || 'Server error'); break;
    case 'pong':       break;
    default:           log('unhandled server message: ' + m.t);
  }
}

/* Switching a chat makes the engine emit chat_switched *and* history_invalidated,
   so coalesce the refetch instead of pulling the transcript twice. */
let historyRefetch = null;
function refetchHistory() {
  clearTimeout(historyRefetch);
  historyRefetch = setTimeout(() => sendFrame({ t: 'get_history' }), 60);
}

function updateHeader(policyMode) {
  $('#chat-title').textContent = chatTitles.get(chatId) || (chatId ? 'Chat' : 'Aristotle');
  const bits = [];
  if (statusText) bits.push(statusText);
  else if (running) bits.push('working…');
  if (typeof policyMode === 'number') updateHeader.policy = POLICY[policyMode] || 'ask';
  if (updateHeader.policy) bits.push(updateHeader.policy);
  $('#chat-sub').textContent = bits.join(' · ');
}

/** Single place that decides what the user may touch right now. */
function applyGates() {
  const input = $('#input');
  const send = $('#send');
  const blocked = !canControl || !sessionReady || !!pendingApproval;

  input.disabled = blocked;
  input.placeholder = !canControl ? 'View only' :
    !sessionReady ? 'Reconnecting…' :
      pendingApproval ? 'Waiting for your approval…' : 'Message Aristotle…';

  // While a run is in flight the send button becomes a stop button, which stays
  // live even in the "blocked" states above (as long as we can control at all).
  send.classList.toggle('stop', running);
  $('#send-icon').textContent = running ? '■' : '↑';
  send.setAttribute('aria-label', running ? 'Stop' : 'Send');
  send.disabled = running ? (!canControl || !sessionReady) : (blocked || !input.value.trim());

  const banner = $('#banner');
  if (!canControl) {
    banner.replaceChildren(
      el('b', null, 'View only. '),
      document.createTextNode('This device can watch the conversation but cannot send ' +
        'messages or answer approvals. Grant control for this device in the Remote Access ' +
        'panel of the desktop editor.'));
    show(banner, true);
  } else {
    show(banner, false);
  }
}

function toastError(text) {
  log('error: ' + text);
  const node = el('div', 'meta', '⚠ ' + text);
  $('#messages').appendChild(node);
  scrollToEnd();
}

// ============================================================ transcript

const messages = () => $('#messages');
let toolCards = new Map();   // tool_call_id -> card element (so results merge in)
let live = { assistant: null, thinking: null };
let liveText = { assistant: '', thinking: '' };
let rafPending = false;

function nearBottom() {
  const m = messages();
  return m.scrollHeight - m.scrollTop - m.clientHeight < 120;
}
function scrollToEnd(force) {
  const m = messages();
  if (force || nearBottom()) m.scrollTop = m.scrollHeight;
}

function renderHistory(rows) {
  messages().replaceChildren();
  toolCards = new Map();
  live = { assistant: null, thinking: null };
  liveText = { assistant: '', thinking: '' };
  for (const row of rows) if (row && row.item) appendItem(row.item);
  scrollToEnd(true);
}

function onItem(item) {
  if (!item || typeof item !== 'object') return;
  // A persisted item supersedes whatever we were streaming for that channel.
  if (item.role === 'assistant') clearLive('assistant');
  if (item.type === 'thinking') clearLive('thinking');
  if (item.role === 'user') { clearLive('assistant'); clearLive('thinking'); }
  const stick = nearBottom();
  appendItem(item);
  scrollToEnd(stick);
}

function appendItem(item) {
  const node = buildItem(item);
  if (node) messages().appendChild(node);
}

/** Returns a DOM node, or null for items we deliberately do not render. */
function buildItem(item) {
  if (!item || typeof item !== 'object') return null;
  try {
    if (item.role === 'user') return userBubble(item);
    if (item.role === 'assistant') return assistantBlock(item);
    if (item.role === 'tool') return toolResultItem(item);

    switch (item.type) {
      case 'thinking':
        return thinkingBlock(String(item.text || ''));
      case 'todo_state':
        return todoPanel(Array.isArray(item.tasks) ? item.tasks : []);
      case 'model_info':
        return el('div', 'meta', `${item.model_id || 'model'} · ${item.provider || ''}`.trim());
      case 'engine_state': {
        const e = item.error_count | 0, w = item.warning_count | 0;
        if (!e && !w && !item.game_running) return null;
        const parts = [];
        if (item.game_running) parts.push('game running');
        if (e) parts.push(e + ' error' + (e === 1 ? '' : 's'));
        if (w) parts.push(w + ' warning' + (w === 1 ? '' : 's'));
        return el('div', 'meta', 'engine: ' + parts.join(', '));
      }
      case 'parse_error_state':
        return el('div', 'meta',
          `${item.error_count | 0} parse error(s) in ${shortPath(item.file_path)}`);
      case 'scene_diff': {
        const n = Array.isArray(item.scenes) ? item.scenes.length : 0;
        return n ? el('div', 'meta', `scene changes (${n})`) : null;
      }
      default:
        return null; // unknown item types are skipped, never fatal
    }
  } catch (e) {
    log('render failed for item: ' + e.message);
    return null;
  }
}

function shortPath(p) {
  const s = String(p || '');
  return s.length > 42 ? '…' + s.slice(-40) : s;
}

function userBubble(item) {
  const wrap = el('div', 'msg user');
  wrap.appendChild(document.createTextNode(String(item.content == null ? '' : item.content)));
  if (Array.isArray(item.images)) {
    for (const raw of item.images) {
      // Strip anything that is not base64 before building the data URL.
      const clean = String(raw).replace(/^data:[^,]*,/, '').replace(/[^A-Za-z0-9+/=]/g, '');
      if (!clean) continue;
      const img = el('img');
      img.src = 'data:image/png;base64,' + clean;
      img.alt = 'attached image';
      img.loading = 'lazy';
      wrap.appendChild(img);
    }
  }
  return wrap;
}

function assistantBlock(item) {
  const wrap = el('div', 'msg assistant');
  const raw = item.content;
  const blocks = Array.isArray(raw) ? raw
    : typeof raw === 'string' ? [{ type: 'text', text: raw }] : [];
  for (const b of blocks) {
    if (!b || typeof b !== 'object') continue;
    if (b.type === 'text' && b.text) wrap.appendChild(markdown(String(b.text)));
    else if (b.type === 'tool_call') wrap.appendChild(toolCard(b.name, b.args, b.id));
  }
  return wrap.childNodes.length ? wrap : null;
}

function thinkingBlock(text) {
  const wrap = el('div', 'think');
  wrap.appendChild(el('div', 'think-label', 'thinking'));
  wrap.appendChild(el('div', 'think-text', text));
  return wrap;
}

function todoPanel(tasks) {
  const wrap = el('div', 'todo');
  wrap.appendChild(el('div', 'todo-h', 'plan'));
  for (const t of tasks) {
    if (!t || typeof t !== 'object') continue;
    const status = String(t.status || 'pending');
    const row = el('div', 'todo-row ' + status);
    row.appendChild(el('span', 'box',
      status === 'done' ? '✓' : status === 'in_progress' ? '▸' : '○'));
    row.appendChild(el('span', 'txt', String(t.text || t.id || '')));
    wrap.appendChild(row);
  }
  return wrap;
}

// --------------------------------------------------------------- tool cards

/** Compact one-line hint from the args, e.g. the path a tool is acting on. */
function argSummary(args) {
  let a = args;
  if (typeof a === 'string') { try { a = JSON.parse(a); } catch (e) { return a.slice(0, 60); } }
  if (!a || typeof a !== 'object') return '';
  for (const k of Object.keys(a)) {
    const v = a[k];
    if (typeof v === 'string' && v) return v.length > 60 ? v.slice(0, 58) + '…' : v;
  }
  return '';
}

function pretty(v) {
  if (v == null) return '';
  let s;
  if (typeof v === 'string') s = v;
  else { try { s = JSON.stringify(v, null, 2); } catch (e) { s = String(v); } }
  return s.length > 8000 ? s.slice(0, 8000) + '\n… [truncated]' : s;
}

function addKV(body, label, value) {
  if (value === '' || value == null) return;
  body.appendChild(el('div', 'kv', label));
  body.appendChild(el('pre', null, value));
}

function toolCard(name, args, id) {
  const card = el('div', 'tool');
  card.dataset.status = 'pending';

  const head = el('button', 'tool-head');
  head.type = 'button';
  head.appendChild(el('span', 'chev', '›'));
  head.appendChild(el('span', 'tool-name', String(name || 'tool')));
  head.appendChild(el('span', 'tool-sub', argSummary(args)));
  const badge = el('span', 'tool-badge', 'running');
  head.appendChild(badge);

  const body = el('div', 'tool-body hidden');
  addKV(body, 'Arguments', pretty(args));

  head.addEventListener('click', () => {
    body.classList.toggle('hidden');
    card.classList.toggle('open');
  });

  card.append(head, body);
  card._badge = badge;
  card._body = body;
  if (id) toolCards.set(String(id), card);
  return card;
}

/** A role:"tool" item either fills in its pending call card, or stands alone. */
function toolResultItem(item) {
  const id = item.tool_call_id ? String(item.tool_call_id) : '';
  const content = (item.content && typeof item.content === 'object') ? item.content : {};
  const existing = id ? toolCards.get(id) : null;
  if (existing) {
    fillToolResult(existing, content);
    return null; // already in the DOM, attached to its tool_call
  }
  const card = toolCard(content.tool_name, content.args, id);
  fillToolResult(card, content);
  return card;
}

function fillToolResult(card, content) {
  const status = String(content.status || 'success');
  card.dataset.status = status;
  card._badge.textContent = status === 'success' ? 'ok' : status;
  if (status === 'error') {
    addKV(card._body, 'Error', pretty(content.error !== undefined ? content.error : content.result));
    card._body.classList.remove('hidden');   // failures open by default
    card.classList.add('open');
  } else {
    addKV(card._body, 'Result', pretty(content.result));
  }
}

// ------------------------------------------------------------ live streaming

function onDelta(kind, text) {
  if (kind !== 'assistant' && kind !== 'thinking') return;
  liveText[kind] += text;
  if (!live[kind]) {
    live[kind] = kind === 'assistant' ? el('div', 'msg assistant live') : el('div', 'think live');
    messages().appendChild(live[kind]);
  }
  if (!rafPending) {
    rafPending = true;
    requestAnimationFrame(renderLive);
  }
}

function renderLive() {
  rafPending = false;
  const stick = nearBottom();
  if (live.assistant) {
    // Re-render the accumulated text and park a blinking caret on the last line.
    const md = markdown(liveText.assistant);
    const last = md.lastElementChild;
    const caret = el('span', 'caret', '▍');
    (last && last.classList.contains('md-p') ? last : md).appendChild(caret);
    live.assistant.replaceChildren(md);
  }
  if (live.thinking) {
    live.thinking.replaceChildren(
      el('div', 'think-label', 'thinking'),
      el('div', 'think-text', liveText.thinking));
  }
  scrollToEnd(stick);
}

function clearLive(kind) {
  if (live[kind]) live[kind].remove();
  live[kind] = null;
  liveText[kind] = '';
}

// ============================================================ markdown (safe)

/* Everything below builds DOM nodes and text nodes. innerHTML is never used with
   model output, so there is no HTML-injection path. Underscore emphasis is
   deliberately unsupported: it would mangle snake_case identifiers. */

const BULLET = /^\s*([-*+]|\d+[.)])\s+/;

function markdown(src) {
  const out = el('div', 'md');
  const lines = String(src).split('\n');
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];

    const fence = line.match(/^\s*```(\S*)\s*$/);
    if (fence) {
      const buf = [];
      i++;
      while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) buf.push(lines[i++]);
      i++; // consume the closing fence (tolerates EOF)
      const pre = el('pre', 'code');
      if (fence[1]) pre.dataset.lang = fence[1];
      pre.appendChild(el('code', null, buf.join('\n')));
      out.appendChild(pre);
      continue;
    }

    const heading = line.match(/^(#{1,4})\s+(.*)$/);
    if (heading) {
      const h = el('h' + Math.min(5, heading[1].length + 2), 'md-h');
      inline(heading[2], h);
      out.appendChild(h);
      i++;
      continue;
    }

    if (BULLET.test(line)) {
      const ordered = /^\s*\d+[.)]\s+/.test(line);
      const list = el(ordered ? 'ol' : 'ul', 'md-list');
      while (i < lines.length && BULLET.test(lines[i])) {
        const li = el('li');
        inline(lines[i].replace(BULLET, ''), li);
        list.appendChild(li);
        i++;
      }
      out.appendChild(list);
      continue;
    }

    if (!line.trim()) { i++; continue; }

    // Otherwise: a paragraph, running until a blank line or another block start.
    const buf = [];
    while (i < lines.length && lines[i].trim() &&
           !/^\s*```/.test(lines[i]) && !/^#{1,4}\s/.test(lines[i]) && !BULLET.test(lines[i])) {
      buf.push(lines[i++]);
    }
    const p = el('p', 'md-p');
    inline(buf.join('\n'), p);
    out.appendChild(p);
  }
  return out;
}

/** Inline spans: `code` first so ** inside code is left alone, then bold, then italic. */
function inline(text, parent) {
  const re = /`([^`]+)`|\*\*([\s\S]+?)\*\*|\*([^*\n]+)\*/g;
  let last = 0, m;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) parent.appendChild(document.createTextNode(text.slice(last, m.index)));
    if (m[1] !== undefined) parent.appendChild(el('code', null, m[1]));
    else if (m[2] !== undefined) parent.appendChild(el('strong', null, m[2]));
    else parent.appendChild(el('em', null, m[3]));
    last = re.lastIndex;
  }
  if (last < text.length) parent.appendChild(document.createTextNode(text.slice(last)));
}

// ============================================================ approvals

function setApproval(info) {
  pendingApproval = (info && typeof info === 'object' && Object.keys(info).length) ? info : null;
  const panel = $('#approval');
  panel.replaceChildren();
  if (!pendingApproval) { show(panel, false); applyGates(); return; }

  const i = pendingApproval;
  const kind = String(i.kind || 'command');
  let title, detail = '';
  if (kind === 'file_change') {
    title = 'Apply file changes?';
    detail = (Array.isArray(i.files) ? i.files : []).join('\n');
  } else if (kind === 'exit_plan') {
    title = 'Approve plan and execute?';
    detail = String(i.plan_summary || '');
  } else if (kind === 'editor_tool') {
    title = 'Run ' + String(i.tool || 'action') + '?';
    detail = pretty(i.args);
    if (detail === '{}') detail = '';
  } else {
    title = 'Run command?';
    detail = String(i.command || '');
    if (i.cwd) detail += '\n(in ' + i.cwd + ')';
  }
  if (i.always_ask) title += '  (remote operation)';
  if (i.reason) detail += (detail ? '\n\n' : '') + i.reason;

  panel.appendChild(el('h3', null, title));
  if (detail.trim()) panel.appendChild(el('pre', null, pretty(detail.trim())));

  const btns = el('div', 'approval-btns');
  const add = (label, decision, cls) => {
    const b = el('button', 'btn ' + (cls || ''), label);
    b.type = 'button';
    b.addEventListener('click', () => {
      if (!canControl) return;
      sendFrame({ t: 'approve', decision });
      setApproval(null);
    });
    btns.appendChild(b);
  };
  add('Allow', 'accept', 'primary');
  // Always-ask actions (remote VCS tier) may never be blanket-approved.
  if (!i.always_ask) add('Allow for session', 'acceptForSession', 'subtle');
  add('Deny', 'decline', 'danger');
  panel.appendChild(btns);

  if (!canControl) {
    panel.appendChild(el('div', 'small muted',
      'Approve from the desktop editor — this device does not have control.'));
    for (const b of $$('button', btns)) b.disabled = true;
  }
  show(panel, true);
  applyGates();
  scrollToEnd();
}

// ============================================================ chat list

function relTime(ms) {
  const d = Date.now() - Number(ms || 0);
  if (!ms || d < 0) return '';
  const m = Math.floor(d / 60000);
  if (m < 1) return 'just now';
  if (m < 60) return m + 'm ago';
  const h = Math.floor(m / 60);
  if (h < 24) return h + 'h ago';
  return Math.floor(h / 24) + 'd ago';
}

function renderChats(list) {
  const box = $('#chat-list');
  box.replaceChildren();
  chatTitles = new Map();
  for (const c of list) {
    if (!c || !c.id) continue;
    chatTitles.set(c.id, c.title || c.id);
    const b = el('button', 'chat-item' + (c.active ? ' active' : ''));
    b.type = 'button';
    b.appendChild(el('div', 'chat-item-title', c.title || c.id));
    b.appendChild(el('div', 'small muted', relTime(c.modified_ms)));
    b.addEventListener('click', () => {
      if (!c.active) sendFrame({ t: 'switch_chat', id: c.id });
      closeDrawer();
    });
    box.appendChild(b);
  }
  updateHeader();
}

const openDrawer = () => {
  show($('#drawer'), true);
  show($('#scrim'), true);
  sendFrame({ t: 'list_chats' });
};
const closeDrawer = () => { show($('#drawer'), false); show($('#scrim'), false); };

// ============================================================ screens + boot

function showPairScreen(errText) {
  show($('#screen-pair'), true);
  show($('#screen-chat'), false);
  closeDrawer();
  if (errText) {
    $('#pair-error').textContent = errText;
    show($('#pair-error'), true);
  }
  show($('#pair-forget'), !!localStorage.getItem(LS_KEY));
}

function showChatScreen() {
  show($('#screen-pair'), false);
  show($('#screen-chat'), true);
  applyGates();
}

function defaultDeviceName() {
  const ua = navigator.userAgent;
  const guess = /iPhone/.test(ua) ? 'iPhone' : /iPad/.test(ua) ? 'iPad' :
    /Android/.test(ua) ? 'Android phone' : /Mac/.test(ua) ? 'Mac' :
      /Windows/.test(ua) ? 'Windows PC' : 'Browser';
  return guess;
}

/** Keep the app box glued to the visual viewport so the keyboard never hides input. */
function trackViewport() {
  const vv = window.visualViewport;
  const apply = () => {
    const h = vv ? vv.height : window.innerHeight;
    document.documentElement.style.setProperty('--vvh', h + 'px');
    if (vv) window.scrollTo(0, 0); // iOS scrolls the page under the keyboard otherwise
    scrollToEnd();
  };
  apply();
  if (vv) {
    vv.addEventListener('resize', apply);
    vv.addEventListener('scroll', apply);
  } else {
    window.addEventListener('resize', apply);
  }
}

// A scanned QR carries the code in the URL fragment (`#c=...`). Fragments are
// never sent to the server, and we strip it the moment it is read so it does
// not linger in the address bar or in session history.
function takeCodeFromFragment() {
  const hash = location.hash || '';
  const m = hash.match(/[#&]c=([A-Za-z0-9_\-]+)/);
  if (!m) return '';
  history.replaceState(null, '', location.pathname + location.search);
  return m[1];
}

function wireUi() {
  $('#pair-origin').textContent = location.origin;
  $('#pair-name').value = defaultDeviceName();

  const scanned = takeCodeFromFragment();
  if (scanned) {
    $('#pair-code').value = scanned;
    log('pairing code supplied by QR scan');
  }

  $('#pair-go').addEventListener('click', async () => {
    const btn = $('#pair-go');
    const code = $('#pair-code').value.trim();
    const name = $('#pair-name').value.trim() || defaultDeviceName();
    show($('#pair-error'), false);
    if (!code) { $('#pair-error').textContent = 'Enter the pairing code.'; show($('#pair-error'), true); return; }
    btn.disabled = true;
    btn.textContent = 'Pairing…';
    try {
      await doPair(code, name);
      showChatScreen();
      backoff = 1000;
      connect();
    } catch (e) {
      log('pairing failed: ' + e.message);
      $('#pair-error').textContent = e.message || 'Pairing failed.';
      show($('#pair-error'), true);
    } finally {
      btn.disabled = false;
      btn.textContent = 'Pair this device';
    }
  });

  $('#pair-forget').addEventListener('click', forgetDevice);
  $('#btn-unpair').addEventListener('click', () => {
    if (confirm('Forget this device? You will need a new pairing code.')) forgetDevice();
  });

  $('#btn-menu').addEventListener('click', openDrawer);
  $('#btn-close-drawer').addEventListener('click', closeDrawer);
  $('#scrim').addEventListener('click', closeDrawer);
  $('#btn-new').addEventListener('click', () => {
    if (canControl) sendFrame({ t: 'new_chat' });
    closeDrawer();
  });

  const input = $('#input');
  const autoGrow = () => {
    input.style.height = 'auto';
    input.style.height = Math.min(140, input.scrollHeight) + 'px';
  };
  input.addEventListener('input', () => { autoGrow(); applyGates(); });

  // On touch devices Enter inserts a newline (there is no comfortable Shift+Enter);
  // desktop keyboards get the familiar Enter-to-send.
  const touch = window.matchMedia('(pointer: coarse)').matches;
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !touch) {
      e.preventDefault();
      $('#composer').requestSubmit();
    }
  });

  $('#composer').addEventListener('submit', (e) => {
    e.preventDefault();
    if (running) { sendFrame({ t: 'cancel' }); return; }
    const text = input.value.trim();
    if (!text || !canControl || pendingApproval) return;
    sendFrame({ t: 'send', text });
    input.value = '';
    autoGrow();
    applyGates();
    scrollToEnd(true);
  });

  // Opportunistic reconnects: coming back to the tab, or regaining the network.
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !sock && isPaired()) { backoff = 1000; connect(); }
  });
  window.addEventListener('online', () => {
    if (!sock && isPaired()) { backoff = 1000; connect(); }
  });
}

async function main() {
  wireUi();
  trackViewport();
  log('client start · ' + location.origin);

  // Capability gate. X25519 in WebCrypto is recent (Chrome 133+, Safari 17+,
  // Firefox 132+) and crypto.subtle only exists in a secure context at all.
  if (!window.isSecureContext || !crypto.subtle) {
    const box = $('#unsupported');
    box.textContent = 'This page is not running in a secure context, so the browser ' +
      'hides the crypto API. Use http://localhost, or the https:// address shown in ' +
      'the editor (and accept the certificate once).';
    show(box, true);
    $('#pair-go').disabled = true;
    return;
  }
  try {
    await genEphemeral();
  } catch (e) {
    const box = $('#unsupported');
    box.textContent = 'This browser does not support X25519 key agreement. ' +
      'Update to a recent Chrome, Safari or Firefox.';
    show(box, true);
    $('#pair-go').disabled = true;
    return;
  }

  if (await loadIdentity()) {
    showChatScreen();
    connect();
  } else {
    showPairScreen();
    // Arriving from a QR scan already carries the code, so pair straight away
    // rather than making the user tap a button they did not ask for.
    if ($('#pair-code').value) {
      $('#pair-go').click();
    }
  }
}

main();
