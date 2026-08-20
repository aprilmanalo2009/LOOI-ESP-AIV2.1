// server.js — JITTER BUFFER FIX ONLY (Gemini config corrected)
import express from 'express';
import { createServer } from 'http';
import { WebSocketServer, WebSocket } from 'ws';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { randomBytes } from 'crypto';
import { fileURLToPath } from 'url';
import { fetchPublicText } from './search-security.js';
import pg from 'pg';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const app = express();
const httpServer = createServer(app);
const { Pool } = pg;

// ── Connection tracking ───────────────────────────────────────────
const connections = {
  gemini: new Set(),
  esp32: new Set(),
};

// ── Face identity storage (Neon/PostgreSQL) ─────────────────────
const faceDbPool = process.env.DATABASE_URL
  ? new Pool({
      connectionString: process.env.DATABASE_URL,
      ssl: process.env.DATABASE_URL.includes('neon.tech') ? { rejectUnauthorized: false } : undefined,
      max: 4,
      idleTimeoutMillis: 30000,
    })
  : null;

let faceSchemaPromise = null;
const faceProfileCache = new Map();
const FACE_PROFILE_CACHE_MS = 5000;

async function ensureFaceSchema() {
  if (!faceDbPool) return false;
  if (!faceSchemaPromise) {
    faceSchemaPromise = faceDbPool.query(`
      CREATE TABLE IF NOT EXISTS face_profiles (
        id BIGSERIAL PRIMARY KEY,
        device_id TEXT NOT NULL,
        name TEXT NOT NULL,
        embedding JSONB NOT NULL,
        created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
        updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
        UNIQUE (device_id, name)
      );
      CREATE INDEX IF NOT EXISTS face_profiles_device_idx ON face_profiles (device_id);
    `).then(() => true).catch(err => {
      faceSchemaPromise = null;
      console.error('[FaceDB] schema initialization failed:', err.message);
      return false;
    });
  }
  return faceSchemaPromise;
}

function cleanFaceName(value) {
  return String(value || '').replace(/\s+/g, ' ').trim().slice(0, 80);
}

function cleanDeviceId(value) {
  return String(value || '').replace(/[^a-zA-Z0-9._:-]/g, '').slice(0, 120);
}

function cleanEmbedding(value) {
  if (!Array.isArray(value) || value.length !== 128) return null;
  const embedding = value.map(Number);
  return embedding.every(n => Number.isFinite(n) && Math.abs(n) <= 1.0001) ? embedding : null;
}

function cosineSimilarity(a, b) {
  let dot = 0, aNorm = 0, bNorm = 0;
  for (let i = 0; i < a.length; i++) {
    dot += a[i] * b[i];
    aNorm += a[i] * a[i];
    bNorm += b[i] * b[i];
  }
  return aNorm && bNorm ? dot / Math.sqrt(aNorm * bNorm) : -1;
}

async function registerFaceProfile({ deviceId, name, embedding }) {
  if (!faceDbPool) throw new Error('DATABASE_URL is not configured');
  if (!(await ensureFaceSchema())) throw new Error('Face database is unavailable');
  const safeDeviceId = cleanDeviceId(deviceId);
  const safeName = cleanFaceName(name);
  const safeEmbedding = cleanEmbedding(embedding);
  if (!safeDeviceId) throw new Error('deviceId is required');
  if (safeName.length < 2) throw new Error('A name with at least 2 characters is required');
  if (!safeEmbedding) throw new Error('A valid 128-value face embedding is required');

  const embeddingJson = JSON.stringify(safeEmbedding);
  const existing = await faceDbPool.query(
    'SELECT id FROM face_profiles WHERE device_id = $1 AND LOWER(name) = LOWER($2) LIMIT 1',
    [safeDeviceId, safeName],
  );
  if (existing.rows[0]) {
    await faceDbPool.query(
      'UPDATE face_profiles SET name = $1, embedding = $2::jsonb, updated_at = NOW() WHERE id = $3',
      [safeName, embeddingJson, existing.rows[0].id],
    );
    faceProfileCache.delete(safeDeviceId);
    return { updated: true, name: safeName };
  }
  await faceDbPool.query(
    'INSERT INTO face_profiles (device_id, name, embedding) VALUES ($1, $2, $3::jsonb)',
    [safeDeviceId, safeName, embeddingJson],
  );
  faceProfileCache.delete(safeDeviceId);
  return { updated: false, name: safeName };
}

async function listFaceProfiles({ deviceId }) {
  if (!faceDbPool) throw new Error('DATABASE_URL is not configured');
  if (!(await ensureFaceSchema())) throw new Error('Face database is unavailable');
  const safeDeviceId = cleanDeviceId(deviceId);
  if (!safeDeviceId) throw new Error('deviceId is required');
  const result = await faceDbPool.query(
    'SELECT name, created_at FROM face_profiles WHERE device_id = $1 ORDER BY LOWER(name) ASC',
    [safeDeviceId],
  );
  return result.rows.map(row => ({ name: row.name, createdAt: row.created_at }));
}

async function clearFaceProfile({ deviceId, name }) {
  if (!faceDbPool) throw new Error('DATABASE_URL is not configured');
  if (!(await ensureFaceSchema())) throw new Error('Face database is unavailable');
  const safeDeviceId = cleanDeviceId(deviceId);
  const safeName = cleanFaceName(name);
  if (!safeDeviceId) throw new Error('deviceId is required');
  if (safeName.length < 2) throw new Error('A user name is required');
  const result = await faceDbPool.query(
    'DELETE FROM face_profiles WHERE device_id = $1 AND LOWER(name) = LOWER($2) RETURNING name',
    [safeDeviceId, safeName],
  );
  faceProfileCache.delete(safeDeviceId);
  return { cleared: Boolean(result.rows[0]), name: result.rows[0]?.name || safeName };
}

async function clearAllFaceProfiles({ deviceId }) {
  if (!faceDbPool) throw new Error('DATABASE_URL is not configured');
  if (!(await ensureFaceSchema())) throw new Error('Face database is unavailable');
  const safeDeviceId = cleanDeviceId(deviceId);
  if (!safeDeviceId) throw new Error('deviceId is required');
  const result = await faceDbPool.query(
    'DELETE FROM face_profiles WHERE device_id = $1',
    [safeDeviceId],
  );
  faceProfileCache.delete(safeDeviceId);
  return { cleared: true, count: result.rowCount || 0 };
}

async function recognizeFaceProfile({ deviceId, embedding }) {
  if (!faceDbPool) throw new Error('DATABASE_URL is not configured');
  if (!(await ensureFaceSchema())) throw new Error('Face database is unavailable');
  const safeDeviceId = cleanDeviceId(deviceId);
  const safeEmbedding = cleanEmbedding(embedding);
  if (!safeDeviceId) throw new Error('deviceId is required');
  if (!safeEmbedding) throw new Error('A valid 128-value face embedding is required');
  const cached = faceProfileCache.get(safeDeviceId);
  let profiles = cached && Date.now() - cached.createdAt < FACE_PROFILE_CACHE_MS
    ? cached.profiles
    : null;
  if (!profiles) {
    const result = await faceDbPool.query(
      'SELECT name, embedding FROM face_profiles WHERE device_id = $1',
      [safeDeviceId],
    );
    profiles = result.rows;
    faceProfileCache.set(safeDeviceId, { createdAt: Date.now(), profiles });
  }
  let best = null;
  for (const row of profiles) {
    const stored = cleanEmbedding(row.embedding);
    if (!stored) continue;
    const similarity = cosineSimilarity(safeEmbedding, stored);
    if (!best || similarity > best.similarity) best = { name: row.name, similarity };
  }
  if (!best || best.similarity < 0.55) {
    return { matched: false, similarity: best ? Number(best.similarity.toFixed(4)) : null };
  }
  return { matched: true, name: best.name, similarity: Number(best.similarity.toFixed(4)) };
}

// ── Middleware ────────────────────────────────────────────────
app.use((req, res, next) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});
app.use(express.json({ limit: '50mb' }));

const noCacheHeaders = { 'Cache-Control': 'no-cache, no-store, must-revalidate', Pragma: 'no-cache', Expires: '0' };

// ── 10 Gemini API Key Setup & Auto-Rotation Pool ─────────────
function _loadApiKeys() {
  const clean = s => (s || '').replace(/[\u200e\u200f\u200b\u200c\u200d\uFEFF]/g, '').trim();
  const keys = [];
  const k1 = clean(process.env.GEMINI_API_KEY);
  if (k1) keys.push(k1);
  for (let i = 2; i <= 10; i++) {
    const k = clean(process.env[`GEMINI_API_KEY_${i}`] || process.env[`GEMINI_API_KEY${i}`]);
    if (k) keys.push(k);
  }
  return keys;
}

const API_KEYS = _loadApiKeys();
const _keyExhausted = new Map();

function getActiveKey() {
  const now = Date.now();
  for (const k of API_KEYS) {
    if (((_keyExhausted.get(k)) || 0) <= now) return k;
  }
  return null;
}

function markKeyUnavailable(key, reason = 'unavailable') {
  _keyExhausted.set(key, Date.now() + 24 * 60 * 60 * 1000);
  const available = API_KEYS.filter(k => (_keyExhausted.get(k) || 0) <= Date.now()).length;
  console.log(`[KeyPool] key …${key.slice(-6)} marked ${reason} → ${available}/${API_KEYS.length} key(s) still available`);
}

function keyPoolStatus() {
  const now = Date.now();
  const total = API_KEYS.length;
  const available = API_KEYS.filter(k => (_keyExhausted.get(k) || 0) <= Date.now()).length;
  return { total, available };
}

console.log(`[KeyPool] loaded ${API_KEYS.length} API key(s)`);

// ── Root Status Page ──────────────────────────────────────────
app.get('/', (_req, res) => {
  const uptime = process.uptime();
  const h = Math.floor(uptime / 3600);
  const m = Math.floor((uptime % 3600) / 60);
  const s = Math.floor(uptime % 60);
  const uptimeStr = `${h}h ${m}m ${s}s`;
  res.set(noCacheHeaders);
  res.setHeader('Content-Type', 'text/html');
  res.send(`<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Mochi Robot Server</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { background: #0a0a0f; color: #e0e0e0; font-family: 'Segoe UI', sans-serif;
           display: flex; align-items: center; justify-content: center; min-height: 100vh; }
    .card { background: #111118; border: 1px solid #1e3a5f; border-radius: 16px;
            padding: 40px 48px; max-width: 420px; width: 90%; text-align: center; }
    .dot { width: 12px; height: 12px; background: #00e5a0; border-radius: 50%;
           display: inline-block; margin-right: 8px; animation: pulse 2s infinite; }
    @keyframes pulse { 0%,100%{opacity:1;} 50%{opacity:.4;} }
    h1 { font-size: 1.6rem; color: #00bfff; margin: 16px 0 6px; }
    .sub { color: #666; font-size: 0.85rem; margin-bottom: 28px; }
    .stat { display: flex; justify-content: space-between; padding: 10px 0;
            border-bottom: 1px solid #1a1a2e; font-size: 0.9rem; }
    .stat:last-of-type { border-bottom: none; }
    .label { color: #888; }
    .value { color: #00e5a0; font-weight: 600; }
    .btn { display: inline-block; margin-top: 28px; padding: 12px 28px;
           background: #00bfff18; border: 1px solid #00bfff55; border-radius: 8px;
           color: #00bfff; text-decoration: none; font-size: 0.9rem; transition: .2s; }
    .btn:hover { background: #00bfff28; }
  </style>
</head>
<body>
  <div class="card">
    <div><span class="dot"></span><span style="color:#00e5a0;font-size:.85rem;font-weight:600;">ONLINE</span></div>
    <h1>🤖 Mochi Robot</h1>
    <p class="sub">ESP32 & Gemini Live Framework</p>
    <div class="stat"><span class="label">Status</span><span class="value">Running</span></div>
    <div class="stat"><span class="label">Uptime</span><span class="value">${uptimeStr}</span></div>
    <div class="stat"><span class="label">Voice / LLM</span><span class="value">Gemini Live ✓</span></div>
    <div class="stat"><span class="label">WebSocket</span><span class="value">/ws/gemini + /ws/esp32 ✓</span></div>
    <div class="stat"><span class="label">API Keys</span><span class="value" style="color:${keyPoolStatus().available>0?'#00e5a0':'#ff4444'}">${keyPoolStatus().available}/${keyPoolStatus().total} available</span></div>
    <div class="stat"><span class="label">Connections</span><span class="value">Web:${connections.gemini.size} ESP:${connections.esp32.size}</span></div>
    <a href="/app" class="btn">Open Robot Web UI →</a>
  </div>
</body>
</html>`);
});

app.get('/app', (_req, res) => res.sendFile(path.join(__dirname, 'public', 'index.html'), { headers: noCacheHeaders }));
app.use(express.static('public', { setHeaders: (res, filePath) => { if (filePath.endsWith('.html')) res.set(noCacheHeaders); } }));

// ── HTTP API Routes ────────────────────────────────────────────
app.get('/health', (_req, res) => res.json({ status: 'ok' }));

app.get('/api/gemini/status', (_req, res) => {
  const pool = keyPoolStatus();
  res.json({
    ok: pool.available > 0,
    configured: pool.total > 0,
    ...pool,
    endpoints: { phone: '/ws/gemini', esp32: '/ws/esp32' },
  });
});

app.get('/api/face/status', async (_req, res) => {
  res.json({ configured: Boolean(faceDbPool), ready: faceDbPool ? await ensureFaceSchema() : false });
});

app.post('/api/face/register', async (req, res) => {
  try {
    const profile = await registerFaceProfile(req.body || {});
    res.status(profile.updated ? 200 : 201).json({ ok: true, ...profile });
  } catch (err) {
    res.status(400).json({ error: err.message });
  }
});

// ── WebSocket Server Setup for Phone & ESP32 ─────────────────
const GEMINI_LIVE_URL = 'wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent';
const GEMINI_LIVE_MODEL = process.env.GEMINI_LIVE_MODEL || 'models/gemini-3.1-flash-live-preview';

// FIXED JITTER: Mas maliit na frames para sa mobile data — 512 bytes para smooth
const ESP32_AUDIO_FRAME_BYTES = 512;
const MAX_QUEUED_UPSTREAM_BYTES = 512 * 1024;

// CRITICAL FIX: perMessageDeflate=false para hindi mag-compress ang data papuntang ESP32
const geminiLiveWss = new WebSocketServer({ noServer: true, perMessageDeflate: false });
const esp32LiveWss = new WebSocketServer({ noServer: true, perMessageDeflate: false });

const ROBOT_TOOLS = [{
  functionDeclarations: [
    {
      name: 'run_scenario',
      description: 'Execute a robot action, movement, or expression for Mochi.',
      parameters: {
        type: 'OBJECT',
        properties: {
          action: {
            type: 'STRING',
            enum: ['follow_target', 'take_picture', 'eating', 'drinking', 'angry', 'loving', 'happy', 'sad', 'wink', 'news', 'scanning', 'idle', 'forward', 'backward', 'left', 'right', 'look_up', 'look_down', 'look_center', 'shocked', 'kiss', 'question']
          },
          led: {
            type: 'STRING',
            enum: ['NONE','LED_ON','LED_OFF','LED_WHITE','LED_RED','LED_GREEN','LED_BLUE','LED_CYAN','LED_PURPLE','LED_ORANGE','LED_YELLOW','LED_PINK','LED_BLINK','LED_FADE']
          },
          move: {
            type: 'STRING',
            enum: ['NONE','FORWARD','BACKWARD','LEFT','RIGHT','LOOK_UP','LOOK_DOWN','LOOK_CENTER']
          },
          speed: { type: 'INTEGER', minimum: 0, maximum: 255 }
        },
        required: ['action']
      }
    }
  ]
}];

const GEMINI_LIVE_SYSTEM = `You are LOOI, an interactive AI Robot Companion created by April Manalo. Keep spoken responses short (1-3 sentences) and speak naturally in Tagalog/Taglish. Call run_scenario immediately for any motor gesture or emotion response.`;

function attachGeminiLive(clientWs, request, { target = 'web' } = {}) {
  let apiKey = getActiveKey();
  const cid = Date.now().toString(36);
  const clientIp = request.socket.remoteAddress || 'unknown';

  console.log(`[GeminiLive:${cid}] ➜ Client connected (${target}) from ${clientIp}`);

  // Track connection
  const connSet = target === 'esp32' ? connections.esp32 : connections.gemini;
  connSet.add(cid);

  // CRITICAL FIX: Ping every 20s para hindi i-disconnect ng Render proxy
  const pingInterval = setInterval(() => {
    if (clientWs.readyState === WebSocket.OPEN) {
      clientWs.ping();
    }
  }, 20000);

  if (!apiKey) {
    const message = 'All Gemini API keys have hit their daily quota. Try again later.';
    console.error(`[GeminiLive:${cid}] ERROR: No API key available`);
    try { 
      clientWs.send(JSON.stringify({ error: message })); 
    } catch {}
    // Huwag agad i-close — hintayin 5s para mabasa ng ESP32
    setTimeout(() => {
      clearInterval(pingInterval);
      connSet.delete(cid);
      if (clientWs.readyState < 2) clientWs.close(1011, message);
    }, 5000);
    return;
  }

  console.log(`[GeminiLive:${cid}] Using key ending with …${apiKey.slice(-6)}`);

  let gemWs = null;
  let geminiReconnectTimer = null;
  let geminiConnectAttempt = 0;
  let clientClosing = false;
  let ready = false;
  const audioQueue = [];
  let audioQueueBytes = 0;
  let inputStreamActive = false;
  let inputAudioFrames = 0;
  let outputAudioFrames = 0;

  // JITTER FIX: VAD-based audioStreamEnd for ESP32 — SAME AS ORIGINAL
  let lastAudioTimestamp = 0;
  let vadTimer = null;
  const VAD_SILENCE_MS = 800;  // Send audioStreamEnd after 800ms silence
  const VAD_MIN_SPEECH_MS = 300; // Minimum speech duration before considering end

  const sendClientJson = (message) => {
    if (clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(JSON.stringify(message));
    }
  };

  // JITTER FIX: Mas mabagal na pacing para sa mobile data
  // 24kHz * 16-bit mono PCM ≈ 48 bytes/ms (SAME AS ORIGINAL, pero may extra buffer)
  const ESP32_BYTES_PER_MS = 48;
  const esp32OutQueue = [];
  let esp32PumpTimer = null;

  const clearEsp32AudioQueue = () => {
    esp32OutQueue.length = 0;
    if (esp32PumpTimer) {
      clearTimeout(esp32PumpTimer);
      esp32PumpTimer = null;
    }
  };

  const pumpEsp32Audio = () => {
    if (esp32PumpTimer) return; // pump already running
    const step = () => {
      if (clientWs.readyState !== WebSocket.OPEN || esp32OutQueue.length === 0) {
        esp32PumpTimer = null;
        return;
      }
      const frame = esp32OutQueue.shift();
      clientWs.send(frame);
      // JITTER FIX: Mas mabagal na pacing + extra 3ms buffer para sa mobile data
      const delayMs = Math.max(6, Math.round(frame.length / ESP32_BYTES_PER_MS) + 3);
      esp32PumpTimer = setTimeout(step, delayMs);
    };
    step();
  };

  // JITTER FIX: Mas maliit na frames para smooth sa mobile data
  const sendEsp32Audio = (pcm) => {
    for (let offset = 0; offset < pcm.length; offset += ESP32_AUDIO_FRAME_BYTES) {
      esp32OutQueue.push(pcm.subarray(offset, Math.min(offset + ESP32_AUDIO_FRAME_BYTES, pcm.length)));
    }
    pumpEsp32Audio();
  };

  const queueUpstream = (message) => {
    const messageBytes = Buffer.byteLength(message);
    // Keep the newest audio and the end marker without allowing a long
    // utterance to grow memory indefinitely while setup is in progress.
    while (audioQueue.length && audioQueueBytes + messageBytes > MAX_QUEUED_UPSTREAM_BYTES) {
      audioQueueBytes -= Buffer.byteLength(audioQueue.shift());
    }
    audioQueue.push(message);
    audioQueueBytes += messageBytes;
  };

  const sendUpstreamOrQueue = (message) => {
    if (ready && gemWs?.readyState === WebSocket.OPEN) {
      gemWs.send(message);
      return;
    }
    queueUpstream(message);
  };

  // SAME AS ORIGINAL: VAD helper for ESP32 continuous mode
  const resetVadTimer = () => {
    lastAudioTimestamp = Date.now();
    if (vadTimer) {
      clearTimeout(vadTimer);
      vadTimer = null;
    }
  };

  const startVadTimer = () => {
    if (vadTimer) clearTimeout(vadTimer);
    vadTimer = setTimeout(() => {
      const silenceDuration = Date.now() - lastAudioTimestamp;
      if (silenceDuration >= VAD_SILENCE_MS && ready && gemWs?.readyState === WebSocket.OPEN) {
        console.log(`[GeminiLive:${cid}] VAD: Sending audioStreamEnd after ${silenceDuration}ms silence`);
        gemWs.send(JSON.stringify({ realtimeInput: { audioStreamEnd: true } }));
      }
      vadTimer = null;
    }, VAD_SILENCE_MS);
  };

  const bindGeminiSocket = (socket) => {
    socket.on('open', () => {
    console.log(`[GeminiLive:${cid}] Gemini WS opened`);
    geminiConnectAttempt = 0;
    socket.send(JSON.stringify({
      setup: {
        model: GEMINI_LIVE_MODEL,
        generationConfig: {
          responseModalities: ['AUDIO'],
          speechConfig: {
            voiceConfig: { prebuiltVoiceConfig: { voiceName: 'Kore' } },
            languageCode: 'fil-PH'
          }
        },
        // SAME AS ORIGINAL: realtimeInputConfig para sa ESP32
        ...(target === 'esp32' ? {
          realtimeInputConfig: {
            automaticActivityDetection: {
              disabled: false
            }
          }
        } : {}),
        tools: ROBOT_TOOLS,
        systemInstruction: { parts: [{ text: GEMINI_LIVE_SYSTEM }] }
      }
    }));
    });

    socket.on('message', (data) => {
    const str = data.toString();
    let msg;
    try { msg = JSON.parse(str); } catch { return; }

    if (msg.error) {
      const errorText = typeof msg.error === 'string'
        ? msg.error
        : msg.error.message || JSON.stringify(msg.error);
      console.error(`[GeminiLive:${cid}] Gemini protocol error: ${errorText}`);
      sendClientJson({ error: errorText });
      return;
    }

    if (msg.toolCall) {
      const immediateResponses = [];
      for (const fc of (msg.toolCall.functionCalls || [])) {
        if (fc.name === 'run_scenario') {
          const args = fc.args || {};

          // SAME AS ORIGINAL: Align format with ESP32 expectations
          const robotPayload = {
            robotAction: true,
            move: (args.move || 'NONE').toUpperCase(),
            led: (args.led || 'NONE').toUpperCase(),
            speed: args.speed !== undefined ? args.speed : 128
          };

          console.log(`[GeminiLive:${cid}] Robot command → move:${robotPayload.move} led:${robotPayload.led} speed:${robotPayload.speed}`);

          if (clientWs.readyState === WebSocket.OPEN) {
            clientWs.send(JSON.stringify(robotPayload));
          }
          immediateResponses.push({ id: fc.id, name: fc.name, response: { output: 'executed' } });
        }
      }
      if (immediateResponses.length && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ toolResponse: { functionResponses: immediateResponses } }));
      }
      return;
    }

    if (msg.serverContent?.modelTurn?.parts) {
      for (const part of msg.serverContent.modelTurn.parts) {
        if (part.inlineData && part.inlineData.mimeType.startsWith('audio/pcm')) {
          const rawPcm = Buffer.from(part.inlineData.data, 'base64');
          outputAudioFrames++;
          if (clientWs.readyState === WebSocket.OPEN) {
            if (target === 'esp32') {
              // JITTER FIX: Mas maliit na frames para sa ESP32
              sendEsp32Audio(rawPcm);
            } else {
              // Web/phone client gets the JSON wrapper
              clientWs.send(str);
            }
          }
        }
      }
    }

    if (msg.serverContent?.interrupted === true) {
      console.log(`[GeminiLive:${cid}] Gemini interrupted the current response`);
      if (target === 'esp32') clearEsp32AudioQueue();
      if (clientWs.readyState === WebSocket.OPEN) {
        clientWs.send(target === 'esp32' ? JSON.stringify({ interrupted: true }) : str);
      }
    }

    // ESP32 receives audio as binary frames, so forward the turn boundary
    // separately; otherwise it cannot leave playback mode or resume VAD.
    // The browser tester receives the original JSON event so it can update
    // its state after playback has finished.
    if (msg.serverContent?.turnComplete === true) {
      if (clientWs.readyState === WebSocket.OPEN) {
        clientWs.send(target === 'esp32' ? JSON.stringify({ turnComplete: true }) : str);
      }
      console.log(`[GeminiLive:${cid}] Gemini turn complete (${outputAudioFrames} audio frame(s))`);
      outputAudioFrames = 0;
    }

    if (msg.setupComplete !== undefined) {
      ready = true;
      console.log(`[GeminiLive:${cid}] Gemini setup complete (${GEMINI_LIVE_MODEL})`);
      sendClientJson({
        serverHello: { status: 'ready', target, model: GEMINI_LIVE_MODEL },
        setupComplete: true,
      });
      for (const c of audioQueue) {
        if (socket.readyState === WebSocket.OPEN) socket.send(c);
      }
      audioQueue.length = 0;
      audioQueueBytes = 0;
    }
    });

    socket.on('close', (code, reason) => {
      if (socket !== gemWs) return;
      gemWs = null;
      ready = false;
      console.log(`[GeminiLive:${cid}] Gemini disconnected: ${code} ${reason || ''}`);
      if (clientClosing || clientWs.readyState !== WebSocket.OPEN) return;

      const closeReason = String(reason || '');
      const authFailed = code === 1008 && /invalid authentication|authentication credentials|unauthorized|api key/i.test(closeReason);
      if (authFailed && apiKey) {
        // Do not retry a permanently rejected key every second. Move to the
        // next configured key so one stale secret cannot create a reconnect
        // storm for every connected ESP32.
        markKeyUnavailable(apiKey, 'authentication failure');
        apiKey = getActiveKey();
        if (!apiKey) {
          sendClientJson({ error: 'No valid Gemini API key is available. Update GEMINI_API_KEY secrets.' });
          setTimeout(() => {
            if (!clientClosing && clientWs.readyState === WebSocket.OPEN) clientWs.close(1011, 'Gemini authentication failed');
          }, 5000);
          return;
        }
        console.log(`[GeminiLive:${cid}] Switching to next configured Gemini key`);
      }

      // Keep the device/browser socket alive while Gemini has a transient
      // upstream failure. This prevents ESP32 reconnect storms on mobile
      // hotspots and lets queued input continue after the next handshake.
      const delay = Math.min(1000 * (2 ** Math.min(geminiConnectAttempt, 3)), 10000);
      geminiConnectAttempt++;
      sendClientJson({ serverHello: { status: 'reconnecting', target, retryInMs: delay } });
      clearTimeout(geminiReconnectTimer);
      geminiReconnectTimer = setTimeout(() => {
        geminiReconnectTimer = null;
        connectGemini();
      }, delay);
    });

    socket.on('error', (err) => {
      console.error(`[GeminiLive:${cid}] Gemini error:`, err.message);
    });
  };

  const connectGemini = () => {
    if (clientClosing || clientWs.readyState !== WebSocket.OPEN) return;
    if (!apiKey) {
      apiKey = getActiveKey();
      if (!apiKey) {
        sendClientJson({ error: 'No Gemini API key is available.' });
        return;
      }
    }
    gemWs = new WebSocket(`${GEMINI_LIVE_URL}?key=${apiKey}`);
    bindGeminiSocket(gemWs);
  };

  sendClientJson({ serverHello: { status: 'connecting', target } });
  connectGemini();

  clientWs.on('message', (data, isBinary) => {
    if (!isBinary) {
      const text = data.toString();
      try {
        const msg = JSON.parse(text);

        // SAME AS ORIGINAL: Filter ESP32 deviceHello — huwag i-forward sa Gemini
        if (msg.deviceHello) {
          console.log(`[GeminiLive:${cid}] Received deviceHello:`, msg.deviceHello);
          // Optional: acknowledge
          try { clientWs.send(JSON.stringify({ serverHello: { status: 'ok' } })); } catch {}
          return;
        }

        // Keepalive from the ESP32 must stay between the device and this
        // proxy. Forwarding {"ping":1} to Gemini Live is not valid protocol
        // input and can make Gemini close the upstream socket immediately.
        if (msg.ping !== undefined || msg.type === 'ping') {
          if (clientWs.readyState === WebSocket.OPEN) {
            clientWs.send(JSON.stringify({ pong: 1 }));
          }
          return;
        }

        if (msg.event === 'start_stream') {
          // start_stream/end_stream are bridge commands, not Gemini Live
          // protocol messages. Forwarding start_stream makes Gemini close the
          // session with "Unknown name event", which looked like a random
          // no-response on the browser tester.
          inputStreamActive = true;
          inputAudioFrames = 0;
          if (target === 'esp32') {
            console.log(`[GeminiLive:${cid}] Legacy START_STREAM ignored — ESP32 continuous audio mode`);
          } else {
            console.log(`[GeminiLive:${cid}] Browser START_STREAM accepted`);
          }
          return;
        }
        if (msg.event === 'end_stream' && target === 'esp32') {
          // Do not send audioStreamEnd in continuous mode: it would force a
          // turn boundary and bring back the unreliable local-VAD behavior.
          inputStreamActive = false;
          console.log(`[GeminiLive:${cid}] Legacy END_STREAM ignored — Gemini automatic activity detection is active`);
          return;
        }
        if (msg.event === 'end_stream') {
          console.log(`[GeminiLive:${cid}] Browser END_STREAM received — sending audioStreamEnd after ${inputAudioFrames} frame(s)`);
          if (ready && gemWs?.readyState === WebSocket.OPEN) {
            // For realtime audio input, Gemini expects audioStreamEnd. This
            // closes the current microphone activity and starts generation.
            gemWs.send(JSON.stringify({ realtimeInput: { audioStreamEnd: true } }));
          } else {
            // Keep the end marker after queued audio. Without it, a short
            // first utterance can remain open forever while setup completes.
            queueUpstream(JSON.stringify({ realtimeInput: { audioStreamEnd: true } }));
            console.log(`[GeminiLive:${cid}] audioStreamEnd queued until Gemini setup completes`);
          }
          return;
        }
      } catch {}

      // Forward other text messages to Gemini
      sendUpstreamOrQueue(text);
      return;
    }

    // SAME AS ORIGINAL: Binary audio from ESP32 → VAD + realtimeInput
    const pcmFrame = JSON.stringify({
      realtimeInput: { audio: { data: Buffer.from(data).toString('base64'), mimeType: 'audio/pcm;rate=16000' } }
    });
    inputAudioFrames++;

    // VAD: Reset silence timer on every audio frame
    if (target === 'esp32') {
      resetVadTimer();
      startVadTimer();
    }

    sendUpstreamOrQueue(pcmFrame);
  });

  clientWs.on('close', (code, reason) => {
    console.log(`[GeminiLive:${cid}] Client disconnected: ${code} ${reason || ''}`);
    clientClosing = true;
    clearInterval(pingInterval);
    clearTimeout(geminiReconnectTimer);
    if (vadTimer) clearTimeout(vadTimer);
    clearEsp32AudioQueue();
    connSet.delete(cid);
    if (gemWs && gemWs.readyState < 2) gemWs.close();
  });

  clientWs.on('error', (err) => {
    console.error(`[GeminiLive:${cid}] Client error:`, err.message);
  });

  clientWs.on('pong', () => {
    console.log(`[GeminiLive:${cid}] Client pong received`);
  });
}

geminiLiveWss.on('connection', (clientWs, request) => attachGeminiLive(clientWs, request, { target: 'web' }));
esp32LiveWss.on('connection', (clientWs, request) => attachGeminiLive(clientWs, request, { target: 'esp32' }));

// CRITICAL FIX: Detailed logging sa upgrade handler
httpServer.on('upgrade', (request, socket, head) => {
  const clientIp = socket.remoteAddress || 'unknown';
  console.log(`[WS Upgrade] ➜ Request from ${clientIp}: ${request.url}`);

  const { pathname } = new URL(request.url, 'http://localhost');
  console.log(`[WS Upgrade] Pathname: ${pathname}`);

  if (pathname === '/ws/gemini') {
    console.log('[WS Upgrade] Routing to /ws/gemini');
    geminiLiveWss.handleUpgrade(request, socket, head, (ws) => geminiLiveWss.emit('connection', ws, request));
  } else if (pathname === '/ws/esp32') {
    console.log('[WS Upgrade] Routing to /ws/esp32');
    esp32LiveWss.handleUpgrade(request, socket, head, (ws) => esp32LiveWss.emit('connection', ws, request));
  } else {
    console.log(`[WS Upgrade] ❌ Unknown path: ${pathname} — destroying socket`);
    socket.destroy();
  }
});

const PORT = process.env.PORT || 5000;
httpServer.listen(PORT, '0.0.0.0', () =>
  console.log(`🤖 Server listening on port ${PORT} (HTTP + WS /ws/gemini + /ws/esp32)`)
);
