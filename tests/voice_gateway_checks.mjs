// Existing local voice fixture/assertions, ported from the shell driver's Python.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
const [mode, file] = process.argv.slice(2);
if (mode === 'fixtures') {
  for (const [name, size] of [['ggml-base.en.bin', 147964211], ['ggml-tiny.en.bin', 77704715]]) {
    const fd = fs.openSync(path.join(file, name), 'w'); fs.ftruncateSync(fd, size); fs.closeSync(fd);
  }
  const samples = [0, 500, -500, 0], data = Buffer.alloc(3200), header = Buffer.alloc(44);
  for (let i = 0; i < 1600; i++) data.writeInt16LE(samples[i % 4], i * 2);
  header.write('RIFF'); header.writeUInt32LE(36 + data.length, 4); header.write('WAVEfmt ', 8);
  header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22);
  header.writeUInt32LE(16000, 24); header.writeUInt32LE(32000, 28);
  header.writeUInt16LE(2, 32); header.writeUInt16LE(16, 34); header.write('data', 36); header.writeUInt32LE(data.length, 40);
  fs.writeFileSync(path.join(file, 'voice.wav'), Buffer.concat([header, data]));
} else {
  const d = JSON.parse(fs.readFileSync(file, 'utf8'));
  switch (mode) {
    case 'status':
      assert.equal(d.stt_model_id, 'voice-stt-whisper-base-en');
      assert.equal(d.stt_runtime_ready, true); assert.equal(d.stt_model_downloaded, true); assert.equal(d.stt_ready, true); break;
    case 'tiny':
      assert.equal(d.stt_model_id, 'voice-stt-whisper-tiny-en'); assert.equal(d.stt_ready, true); assert.equal(d.stt_model_downloaded, true); break;
    case 'browser': assert.equal(d.tts_model_id, 'voice-tts-browser'); assert.equal(d.tts_neural_ready, false); break;
    case 'moss-not-ready': assert.equal(d.error.code, 'voice_model_not_ready'); break;
    case 'moss-browser':
      assert.equal(d.tts_model_id, 'voice-tts-moss-nano'); assert.equal(d.tts_neural_ready, true); assert.equal(d.tts_engine, 'moss_browser'); break;
    case 'browser-speech': assert.equal(d.error.code, 'browser_tts_only'); break;
    case 'transcript': assert.deepEqual(d, { text: 'hello from local whisper', duration_seconds: 0.1, engine: 'whisper.cpp' }); break;
    case 'changed': assert.equal(d.error.code, 'voice_not_ready'); assert.equal(d.text, undefined); break;
    default: throw new Error(`Unknown voice assertion: ${mode}`);
  }
}
