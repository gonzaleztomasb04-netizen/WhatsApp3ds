// Puente WhatsApp <-> 3DS para correr en el celular (Termux).
// Usa Baileys (sin navegador) y se vincula con CÓDIGO, no con QR.
//
// Uso:  PHONE=5215512345678 TOKEN=tuclave node server.js
// PHONE = tu número con código de país, solo dígitos.
const fs = require('fs');
const express = require('express');
const pino = require('pino');
const {
  default: makeWASocket,
  useMultiFileAuthState,
  DisconnectReason,
  fetchLatestBaileysVersion,
} = require('@whiskeysockets/baileys');

const TOKEN = process.env.TOKEN || 'cambia-esto';
const PORT = Number(process.env.PORT) || 8080;
const PHONE = (process.env.PHONE || '').replace(/\D/g, '');
const STORE_FILE = 'store.json';

// ---- Almacén simple en memoria (Baileys no guarda chats por sí solo) ----
let store = { chats: {}, msgs: {} };
try {
  store = JSON.parse(fs.readFileSync(STORE_FILE, 'utf8'));
} catch {}
setInterval(() => {
  try {
    fs.writeFileSync(STORE_FILE, JSON.stringify(store));
  } catch {}
}, 10000);

const clean = (s) =>
  String(s || '')
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/[^\x20-\x7e]/g, '?');

  const unwrap = (x) => {
  while (x) {
    const inner =
      (x.ephemeralMessage && x.ephemeralMessage.message) ||
      (x.viewOnceMessage && x.viewOnceMessage.message) ||
      (x.viewOnceMessageV2 && x.viewOnceMessageV2.message) ||
      (x.documentWithCaptionMessage && x.documentWithCaptionMessage.message);
    if (!inner) break;
    x = inner;
  }
  return x;
};

const labelOf = (m) => {
  const x = unwrap(m.message);
  if (!x) return '';
  if (x.conversation) return x.conversation;
  if (x.extendedTextMessage) return x.extendedTextMessage.text || '';
  const cap = (label, o) => (o && o.caption ? `${label} ${o.caption}` : label);
  if (x.imageMessage) return cap('[foto]', x.imageMessage);
  if (x.videoMessage) return cap('[video]', x.videoMessage);
  if (x.audioMessage) return '[audio]';
  if (x.stickerMessage) return '[sticker]';
  if (x.documentMessage) return '[documento]';
  if (x.contactMessage) return '[contacto]';
  if (x.locationMessage || x.liveLocationMessage) return '[ubicacion]';
  if (x.reactionMessage) return '[reaccion]';
  return '[media]';
};
const textOf = (m) => {
  const x = m.message;
  if (!x) return '';
  return (
    x.conversation ||
    (x.extendedTextMessage && x.extendedTextMessage.text) ||
    (x.imageMessage && x.imageMessage.caption) ||
    '[media]'
  );
};

const chatOf = (jid) => (store.chats[jid] = store.chats[jid] || { name: '', unread: 0, ts: 0 });

function addMsg(m) {
  const jid = m.key && m.key.remoteJid;
  if (!jid || jid === 'status@broadcast') return;
  const list = (store.msgs[jid] = store.msgs[jid] || []);
  if (m.key.id && list.some((x) => x.id === m.key.id)) return;
  const ts = Number(m.messageTimestamp) || Math.floor(Date.now() / 1000);
  list.push({ id: m.key.id, me: m.key.fromMe ? 1 : 0, text: labelOf(m), ts });
  if (list.length > 50) list.shift();
  const c = chatOf(jid);
  c.ts = Math.max(c.ts, ts);
  if (!m.key.fromMe) {
    c.unread += 1;
    if (!c.name && m.pushName && !jid.endsWith('@g.us')) c.name = m.pushName;
  }
}

let sock;
let pairingAsked = false;

async function start() {
  const { state, saveCreds } = await useMultiFileAuthState('auth');
  const { version } = await fetchLatestBaileysVersion();
  sock = makeWASocket({
    version,
    auth: state,
    logger: pino({ level: 'silent' }),
    printQRInTerminal: false,
    syncFullHistory: false,
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', ({ connection, lastDisconnect }) => {
    if (connection === 'open') console.log('WhatsApp conectado');
    if (connection === 'close') {
      const code = lastDisconnect && lastDisconnect.error && lastDisconnect.error.output
        ? lastDisconnect.error.output.statusCode
        : 0;
      if (code === DisconnectReason.loggedOut) {
        console.log('Sesión cerrada. Borra la carpeta "auth" y vuelve a vincular.');
      } else {
        setTimeout(start, 2000);
      }
    }
  });

  // Vinculación por código (funciona en el mismo celular)
  if (!sock.authState.creds.registered && !pairingAsked) {
    pairingAsked = true;
    if (!PHONE) {
      console.log('Falta PHONE. Ejemplo: PHONE=5215512345678 node server.js');
    } else {
      setTimeout(async () => {
        try {
          const code = await sock.requestPairingCode(PHONE);
          console.log('\nCÓDIGO DE VINCULACIÓN:', code, '\n');
          console.log('WhatsApp > Dispositivos vinculados > Vincular dispositivo >');
          console.log('"Vincular con número de teléfono" y escribe el código.');
        } catch (e) {
          console.log('No se pudo pedir el código:', e.message);
          pairingAsked = false;
        }
      }, 3000);
    }
  }

  sock.ev.on('messaging-history.set', ({ chats = [], contacts = [], messages = [] }) => {
    chats.forEach((c) => {
      const o = chatOf(c.id);
      if (c.name) o.name = c.name;
      if (c.unreadCount) o.unread = c.unreadCount;
      if (c.conversationTimestamp) o.ts = Math.max(o.ts, Number(c.conversationTimestamp));
    });
    contacts.forEach((c) => {
      const n = c.name || c.notify;
      if (n) chatOf(c.id).name = n;
    });
    messages.forEach(addMsg);
  });
  sock.ev.on('chats.upsert', (chats) =>
    chats.forEach((c) => {
      const o = chatOf(c.id);
      if (c.name) o.name = c.name;
    })
  );
  const onContacts = (cs) =>
    cs.forEach((c) => {
      const n = c.name || c.notify;
      if (c.id && n) chatOf(c.id).name = n;
    });
  sock.ev.on('contacts.upsert', onContacts);
  sock.ev.on('contacts.update', onContacts);
  sock.ev.on('messages.upsert', ({ messages }) => messages.forEach(addMsg));
}
start();

// ---- API HTTP para la 3DS (mismo formato que el otro puente) ----
const app = express();
app.use(express.text({ type: '*/*' }));
app.use((req, res, next) => {
  if (req.get('X-Token') !== TOKEN) return res.sendStatus(401);
  next();
});

app.get('/chats', (req, res) => {
  const rows = Object.entries(store.chats)
    .filter(([jid]) => store.msgs[jid] && store.msgs[jid].length)
    .sort((a, b) => b[1].ts - a[1].ts)
    .slice(0, 30)
    .map(([jid, c]) => `${jid}\t${clean(c.name || jid.split('@')[0])}\t${c.unread}`);
  res.type('text').send(rows.join('\n'));
});

app.get('/messages', (req, res) => {
  const jid = String(req.query.chat || '');
  const limit = Number(req.query.limit) || 12;
  const list = (store.msgs[jid] || []).slice(-limit);
  if (store.chats[jid]) store.chats[jid].unread = 0;
  res.type('text').send(list.map((m) => `${m.me}\t${clean(m.text) || '[media]'}`).join('\n'));
});

app.post('/send', async (req, res) => {
  try {
    const body = String(req.body || '');
    const i = body.indexOf('\n');
    if (i < 0) return res.sendStatus(400);
    const jid = body.slice(0, i);
    const text = body.slice(i + 1);
    const sent = await sock.sendMessage(jid, { text });
    if (sent) addMsg(sent);
    res.send('ok');
  } catch (e) {
    res.status(500).send(String(e.message || e));
  }
});

app.listen(PORT, '0.0.0.0', () => console.log(`Puente en puerto ${PORT}`));
// ---- Descubrimiento automatico para la 3DS (UDP) ----
const dgram = require('dgram');
const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
udp.on('message', (msg, rinfo) => {
  if (String(msg).trim() === 'WA3DS?') {
    udp.send(Buffer.from(`WA3DS:${PORT}`), rinfo.port, rinfo.address);
  }
});
udp.on('error', (e) => console.log('UDP:', e.message));
udp.bind(8081, '0.0.0.0');
