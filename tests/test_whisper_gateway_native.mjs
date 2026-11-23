import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const release = fs.realpathSync(process.argv[2]);
const fixture = fs.realpathSync(process.env.SAMOSA_TEST_REAL_WHISPER_DIR);
const tinyFixture = fs.realpathSync(process.env.SAMOSA_TEST_REAL_WHISPER_TINY);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'samosa-whisper-gateway-'));
const base = 'voice-stt-whisper-base-en';
const tiny = 'voice-stt-whisper-tiny-en';
const voice = path.join(root, 'voice');
const runtime = path.join(voice, 'runtime');
const cli = path.join(runtime, 'whisper-cli');
const pinPath = path.join(runtime, 'whisper-cli.pin');
const tinyPath = path.join(voice, 'stt-whisper-tiny-en', 'ggml-tiny.en.bin');
const selection = path.join(voice, 'stt-selection');
const audio = fs.readFileSync(path.join(fixture, 'long-window.wav'));
const logPath = path.join(root, 'gateway.log');
const gateway = path.join(release, 'bin', 'samosa-gateway');
const fakeBackend = path.resolve('build/test_fake_openai_backend');
let child, token, url;

async function port() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const value = server.address().port;
  server.close();
  await once(server, 'close');
  return value;
}

async function start() {
  const publicPort = await port();
  const env = { ...process.env, SAMOSA_HOME: root, SAMOSA_PORT: String(publicPort),
    SAMOSA_BACKEND_PORT: String(await port()), SAMOSA_QWEN_ENGINE: fakeBackend,
    SAMOSA_QWEN_MODEL: path.join(root, 'qwen-model'), SAMOSA_TOKENIZER: path.join(root, 'tokenizer.json') };
  for (const name of ['SAMOSA_WHISPER_CLI', 'SAMOSA_WHISPER_MODEL', 'SAMOSA_WHISPER_TINY_MODEL',
    'SAMOSA_VOICE_RUNTIME', 'SAMOSA_MODELS_CATALOG']) delete env[name];
  const log = fs.openSync(logPath, 'a');
  child = spawn(gateway, [], { env, stdio: ['ignore', log, log] });
  fs.closeSync(log);
  url = `http://127.0.0.1:${publicPort}`;
  const tokenPath = path.join(root, 'run', 'ui-token');
  for (let i = 0; i < 200; ++i) {
    assert.equal(child.exitCode, null);
    try {
      const response = await fetch(`${url}/healthz`, { signal: AbortSignal.timeout(500) });
      if (response.ok && fs.existsSync(tokenPath)) {
        token = fs.readFileSync(tokenPath, 'utf8').trim();
        return;
      }
    } catch {}
    await delay(50);
  }
  assert.fail('Gateway did not become available');
}

async function stop() {
  if (child && child.exitCode === null && child.signalCode === null) {
    const exited = once(child, 'exit');
    child.kill('SIGTERM');
    await exited;
  }
  child = null;
}

async function request(route, { expected = 200, post, wav, upload } = {}) {
  const headers = { 'X-Samosa-Token': token };
  let body;
  if (post) {
    headers['Content-Type'] = 'application/json';
    body = JSON.stringify(post);
  } else if (wav || upload) {
    headers['Content-Type'] = 'audio/wav';
    if (upload) {
      headers['X-Samosa-Media-Type'] = 'audio/wav';
      headers['X-Samosa-Filename-B64'] = Buffer.from('long.wav').toString('base64');
    }
    body = audio;
  }
  const response = await fetch(`${url}${route}`, {
    method: body ? 'POST' : 'GET', headers, body, signal: AbortSignal.timeout(90000),
  });
  const raw = await response.text();
  assert.ok([expected].flat().includes(response.status), `${route}: HTTP ${response.status} ${raw}`);
  return JSON.parse(raw);
}

function revision(model) {
  const state = path.join(voice, `whisper-${model}-activation`, 'activation', 'activation.sqlite3');
  return execFileSync('/usr/bin/sqlite3', [state,
    "select revision,json_extract(config,'$.bundle_config.profile') from current"], { encoding: 'utf8' }).trim();
}

async function voiceStatus(model, ready = true) {
  const result = await request('/v1/voice/status');
  assert.equal(result.stt_model_id, model);
  assert.equal(result.stt_ready, ready);
  return result;
}

async function transcribe() {
  const result = await request('/v1/voice/transcriptions', { wav: true });
  assert.equal(result.engine, 'whisper.cpp');
  assert.equal(result.duration_seconds, 33.482);
  assert.match(result.text, /silver compass/i);
}

async function attachment(model, conversation) {
  const upload = await request('/v1/attachments', { upload: true, expected: [200, 201] });
  const id = upload.id;
  assert.ok(id);
  await request('/v1/chat/completions', { post: {
    model: 'qwen3.6-35b-a3b', model_id: 'qwen', model_version: 'qwen-model',
    conversation_id: conversation,
    messages: [{ role: 'user', content: 'attachment audio probe: what is said at the end?' }],
    attachment_ids: [id], stream: false,
  } });
  const files = fs.readdirSync(path.join(root, 'attachments'), { recursive: true });
  const evidence = files.find(file => file.endsWith(`/${id}.transcript.json`) || file === `${id}.transcript.json`);
  assert.ok(evidence);
  const body = JSON.parse(fs.readFileSync(path.join(root, 'attachments', evidence), 'utf8'));
  assert.equal(body.provider.model_id, model);
  assert.match(JSON.stringify(body), /silver compass/i);
}

try {
  fs.symlinkSync(release, path.join(root, 'current'));
  fs.mkdirSync(runtime, { recursive: true });
  fs.mkdirSync(path.dirname(tinyPath), { recursive: true });
  const basePath = path.join(voice, 'stt-whisper-base-en', 'ggml-base.en.bin');
  fs.mkdirSync(path.dirname(basePath), { recursive: true });
  fs.copyFileSync(path.join(fixture, 'whisper-cli'), cli);
  fs.copyFileSync(path.join(fixture, 'ggml-base.en.bin'), basePath);
  fs.chmodSync(cli, 0o700);
  fs.chmodSync(basePath, 0o600);
  fs.writeFileSync(tinyPath, '', { mode: 0o600 });
  fs.truncateSync(tinyPath, fs.statSync(tinyFixture).size);
  fs.mkdirSync(path.join(root, 'qwen-model'));
  fs.writeFileSync(path.join(root, 'qwen-model', 'experts.bin'), 'fake chat fixture');
  fs.writeFileSync(path.join(root, 'tokenizer.json'), 'fake tokenizer');

  const original = fs.statSync(cli);
  await start();
  assert.equal((await voiceStatus(base)).stt_runtime_ready, true);
  const pin = fs.readFileSync(pinPath, 'utf8');
  const hash = createHash('sha256').update(fs.readFileSync(cli)).digest('hex');
  assert.equal(pin, `whisper-runtime-v1\t${hash}\t${original.size}\n`);
  assert.equal(fs.statSync(cli).ino, original.ino);
  assert.equal((await request('/v1/voice/runtime', { post: {} })).stt_ready, true);
  assert.equal(revision('base'), '1|on-demand-whisper-long-window-v1');
  await transcribe();
  await attachment(base, 'base-whisper-native');

  const refused = await request('/v1/voice/select', { expected: 409, post: { kind: 'stt', model_id: tiny } });
  assert.equal(refused.error.code, 'voice_model_not_ready');
  await voiceStatus(base);
  assert.equal(revision('tiny'), '');
  fs.copyFileSync(tinyFixture, `${tinyPath}.verified`);
  fs.chmodSync(`${tinyPath}.verified`, 0o600);
  fs.renameSync(`${tinyPath}.verified`, tinyPath);
  assert.equal((await request('/v1/voice/select', { post: { kind: 'stt', model_id: tiny } })).stt_ready, true);
  assert.equal(fs.readFileSync(selection, 'utf8').trim(), tiny);
  assert.equal(revision('tiny'), '1|on-demand-whisper-long-window-v1');
  await transcribe();
  await attachment(tiny, 'tiny-whisper-native');

  fs.copyFileSync(cli, `${cli}.verified`);
  const fd = fs.openSync(cli, 'r+');
  const tail = Buffer.alloc(1);
  fs.readSync(fd, tail, 0, 1, original.size - 1);
  tail[0] ^= 1;
  fs.writeSync(fd, tail, 0, 1, original.size - 1);
  fs.closeSync(fd);
  await voiceStatus(tiny, false);
  await request('/v1/voice/transcriptions', { wav: true, expected: 409 });
  assert.equal(fs.readFileSync(pinPath, 'utf8'), pin);
  assert.equal(revision('tiny'), '1|on-demand-whisper-long-window-v1');
  fs.renameSync(`${cli}.verified`, cli);
  await voiceStatus(tiny);
  assert.equal(revision('tiny'), '2|on-demand-whisper-long-window-v1');

  await stop();
  await start();
  await voiceStatus(tiny);
  assert.equal(revision('tiny'), '3|on-demand-whisper-long-window-v1');
  assert.deepEqual(fs.readdirSync(path.join(voice, 'tmp')), []);
  console.log('PASS installed Whisper gateway with Base and Tiny, voice and durable SRT, one-time pin adoption, rollback and restart');
} catch (error) {
  if (fs.existsSync(logPath)) console.error(fs.readFileSync(logPath, 'utf8').split('\n').slice(-30).join('\n'));
  throw error;
} finally {
  await stop();
  fs.rmSync(root, { recursive: true, force: true });
}
