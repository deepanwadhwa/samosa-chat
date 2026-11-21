// Existing durable-audio fixture and assertions; no model/runtime implementation.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
const [mode, ...args] = process.argv.slice(2);
const json = file => JSON.parse(fs.readFileSync(file, 'utf8'));
const input = () => json(0);
const between = (value, low, high) => { assert.equal(typeof value, 'number'); assert(value >= low && value <= high, `${value} outside ${low}..${high}`); };
function wavHeader(frames, channels = 1, rate = 16000) {
  const h = Buffer.alloc(44), bytes = frames * channels * 2;
  h.write('RIFF'); h.writeUInt32LE(36 + bytes, 4); h.write('WAVEfmt ', 8);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(channels, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * channels * 2, 28);
  h.writeUInt16LE(channels * 2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(bytes, 40);
  return h;
}
function sparseWav(file, channels, seconds, marker = 0) {
  const h = wavHeader(16000 * seconds, channels), fd = fs.openSync(file, 'w');
  fs.writeSync(fd, h); fs.ftruncateSync(fd, 44 + 16000 * seconds * channels * 2);
  if (marker) { const sample = Buffer.alloc(2); sample.writeInt16LE(marker); fs.writeSync(fd, sample, 0, 2, 44); }
  fs.closeSync(fd);
}
function stereoTone(file, seconds, frequency) {
  const frames = 44100 * seconds, data = Buffer.alloc(frames * 4);
  for (let i = 0; i < frames; i++) {
    const sample = Math.trunc(5000 * Math.sin(2 * Math.PI * frequency * i / 44100));
    data.writeInt16LE(sample, i * 4); data.writeInt16LE(Math.floor(sample / 2), i * 4 + 2);
  }
  fs.writeFileSync(file, Buffer.concat([wavHeader(frames, 2, 44100), data]));
}
function readWav(file) {
  const b = fs.readFileSync(file); assert.equal(b.toString('ascii', 0, 4), 'RIFF'); assert.equal(b.toString('ascii', 8, 12), 'WAVE');
  let channels, width, rate, bytes;
  for (let at = 12; at + 8 <= b.length;) {
    const name = b.toString('ascii', at, at + 4), length = b.readUInt32LE(at + 4), start = at + 8;
    assert(start + length <= b.length);
    if (name === 'fmt ') { assert(length >= 16); assert.equal(b.readUInt16LE(start), 1); channels = b.readUInt16LE(start + 2); rate = b.readUInt32LE(start + 4); width = b.readUInt16LE(start + 14) / 8; }
    if (name === 'data') bytes = length;
    at = start + length + (length % 2);
  }
  assert(channels && width && rate && bytes !== undefined);
  return { channels, width, rate, frames: bytes / (channels * width) };
}
function windows(root, id) {
  const found = [];
  function walk(directory) {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const file = path.join(directory, entry.name);
      if (entry.isDirectory()) walk(file);
      else if (entry.isFile() && entry.name.startsWith(`${id}.transcript.window-`) && entry.name.endsWith('.json')) found.push(file);
    }
  }
  walk(root); return found.sort();
}
switch (mode) {
  case 'model': { const fd = fs.openSync(args[0], 'w'); fs.ftruncateSync(fd, 147964211); fs.closeSync(fd); break; }
  case 'fixtures': {
    const root = args[0], file = name => path.join(root, name);
    sparseWav(file('podcast.wav'), 1, 14);
    const malformed = fs.readFileSync(file('podcast.wav')); malformed.writeUInt32LE(36, 4); fs.writeFileSync(file('podcast.wav.short-riff'), malformed);
    sparseWav(file('stereo.wav'), 2, 4); sparseWav(file('cancel.wav'), 1, 25, 1);
    sparseWav(file('long.wav'), 1, 180); sparseWav(file('ranking.wav'), 1, 271);
    stereoTone(file('compressed-source.wav'), 14, 440); stereoTone(file('compressed-cancel-source.wav'), 25, 523);
    sparseWav(file('video-audio.wav'), 1, 2); sparseWav(file('exact-window.wav'), 1, 30);
    const broken = Buffer.alloc(20); broken.writeUInt32BE(24, 0); broken.write('ftypM4A ', 4); broken.write('M4A ', 16); fs.writeFileSync(file('malformed.m4a'), broken);
    break;
  }
  case 'probe-aac': {
    const d = json(args[0]); assert.equal(d.ok, true); assert.equal(d.audio_streams, 1); assert.equal(d.video_streams, 0);
    assert.equal(d.selected_stream, 0); assert.equal(d.codec_fourcc, 'aac '); between(d.duration_seconds, 13.9, 14.1); break;
  }
  case 'probe-mp3': {
    const d = json(args[0]); assert.equal(d.ok, true); assert.equal(d.codec_fourcc, '.mp3');
    assert.equal(d.audio_streams, 1); assert.equal(d.video_streams, 0); between(d.duration_seconds, 1, 1.3); break;
  }
  case 'probe-video': {
    const d = json(args[0]); assert.equal(d.ok, true); assert.equal(d.video_streams, 1); assert.equal(d.audio_streams, 1);
    assert.equal(d.codec_fourcc, 'aac '); assert(d.audio_start_seconds <= 0.05); assert(d.audio_end_seconds >= d.duration_seconds - 0.25); break;
  }
  case 'decoded-window': case 'decoded-mp3': case 'decoded-video': {
    const w = readWav(args[0]); assert.equal(w.channels, 1); assert.equal(w.width, 2); assert.equal(w.rate, 16000);
    if (mode === 'decoded-mp3') assert(w.frames > 15000);
    else between(w.frames / 16000, mode === 'decoded-window' ? 4.9 : 1.9, mode === 'decoded-window' ? 5.1 : 2.1);
    break;
  }
  case 'caps-audio': {
    const d = input(), audio = Object.fromEntries(d.sources.map(source => [source.kind, source])).audio, compressed = Boolean(Number(args[0]));
    assert.deepEqual(audio.media_types, compressed ? ['audio/wav', 'audio/mpeg', 'audio/mp4'] : ['audio/wav']);
    assert(audio.available_operations.includes('transcribe_audio')); assert(audio.limitations.includes('english_only'));
    assert.equal(audio.limitations.includes('compressed_audio_macos_only'), compressed); assert.equal(d.limits.max_audio_duration_seconds, 14400); break;
  }
  case 'visual-only-id': {
    const d = input(); assert.equal(d.media_type, 'video/mp4'); assert.equal(d.capabilities.video, true); assert.equal(d.capabilities.audio, false);
    assert.equal(d.source.kind, 'video'); assert.equal(d.source.duration_seconds, null); assert.deepEqual(d.source.available_operations, ['analyze_video', 'inspect_metadata']); console.log(d.id); break;
  }
  case 'id': { const d = input(); assert('id' in d); console.log(d.id); break; }
  case 'duration-id': { const d = input(); between(d.source.duration_seconds, Number(args[0]), Number(args[1] ?? args[0])); console.log(d.id); break; }
  case 'wav-upload': {
    const d = input(), s = d.source; assert.equal(d.media_type, 'audio/wav'); assert.equal(d.capabilities.audio, true); assert.equal(d.capabilities.document, false);
    assert.equal(s.kind, 'audio'); assert.equal(s.container, 'wav'); assert.equal(s.codec, 'pcm_s16le'); assert.equal(s.duration_seconds, 14);
    assert(s.available_operations.includes('transcribe_audio')); assert(s.limitations.includes('speaker_diarization_unavailable')); break;
  }
  case 'evidence': {
    const d = json(args[0]), id = args[1]; assert.equal(d.schema, 'samosa.evidence.v1'); assert.equal(d.attachment_id, id);
    assert.deepEqual(d.coverage, { start_seconds: 0, end_seconds: 14, complete: true });
    assert.deepEqual(d.segments[1], { start_seconds: 1, end_seconds: 2.5, text: 'The podcast sentinel appears here.' });
    assert(d.segments.some(segment => segment.start_seconds === 10 && segment.end_seconds === 11.5 && segment.text === 'The podcast sentinel appears here.' && Object.keys(segment).length === 3));
    const files = windows(args[2], id); assert.equal(files.length, 2);
    files.forEach((file, index) => { const w = json(file); assert.equal(w.schema, 'samosa.evidence.v1'); assert.equal(w.kind, 'transcript_window');
      assert.equal(w.operation, 'transcribe_audio'); assert.equal(w.attachment_id, id); assert.equal(w.window_index, index); assert.equal(w.complete, true); assert(w.srt); });
    assert.equal(json(files[0]).start_seconds, 0); assert.equal(json(files[0]).end_seconds, 10);
    assert.equal(json(files[1]).start_seconds, 9); assert.equal(json(files[1]).end_seconds, 14); break;
  }
  case 'manifest': case 'video-manifest': {
    const item = json(args[0]).documents[0]; assert.equal(item.attachment_id, args[1]); assert.equal(item.source_kind, 'audio');
    if (mode === 'manifest') assert.equal(item.mode, 'retrieval'); break;
  }
  case 'm4a-upload': {
    const d = input(), s = d.source; assert.equal(d.media_type, 'audio/mp4'); assert.equal(d.capabilities.audio, true); assert.equal(s.kind, 'audio');
    assert.equal(s.container, 'm4a'); assert.equal(s.codec, 'aac'); between(s.duration_seconds, 13.9, 14.1); assert(s.available_operations.includes('transcribe_audio'));
    assert(s.limitations.includes('decoded_to_pcm_s16le_mono_16000hz')); assert(s.limitations.includes('compressed_audio_macos_only')); break;
  }
  case 'm4a-evidence': {
    const d = json(args[0]); assert.equal(d.attachment_id, args[1]); assert.equal(d.operation, 'transcribe_audio'); assert.equal(d.coverage.complete, true);
    between(d.coverage.end_seconds, 13.9, 14.1); assert.equal(d.provider.id, 'whisper_cpp'); break;
  }
  case 'mp3-upload': {
    const d = input(), s = d.source; assert.equal(d.media_type, 'audio/mpeg'); assert.equal(s.kind, 'audio'); assert.equal(s.container, 'mp3'); assert.equal(s.codec, 'mp3');
    between(s.duration_seconds, 1, 1.3); assert(s.limitations.includes('decoded_to_pcm_s16le_mono_16000hz')); break;
  }
  case 'caps-video': {
    const video = Object.fromEntries(input().sources.map(source => [source.kind, source])).video;
    assert(video.available_operations.includes('analyze_video')); assert(video.available_operations.includes('transcribe_audio'));
    assert(video.limitations.includes('transcribe_audio_requires_full_span_aac_stream')); break;
  }
  case 'video-audio-id': {
    const d = input(), s = d.source; assert.equal(d.media_type, 'video/mp4'); assert.equal(d.capabilities.video, true); assert.equal(d.capabilities.audio, false);
    assert.equal(s.kind, 'video'); assert.equal(s.container, 'mp4'); assert.equal(s.duration_seconds, 2);
    assert(s.available_operations.includes('analyze_video')); assert(s.available_operations.includes('transcribe_audio')); assert(s.limitations.includes('full_span_aac_audio_stream')); console.log(d.id); break;
  }
  case 'exact-evidence': {
    const d = json(args[0]); assert.deepEqual(d.coverage, { start_seconds: 0, end_seconds: 30, complete: true });
    const files = windows(args[2], args[1]); assert.equal(files.length, 4);
    assert.deepEqual(files.map(file => { const w = json(file); return [w.start_seconds, w.end_seconds]; }), [[0, 10], [9, 19], [18, 28], [27, 30]]);
    assert.deepEqual(d.segments.at(-1), { start_seconds: 27, end_seconds: 30, text: 'The bounded trailing sentinel appears here.' }); break;
  }
  default: throw new Error(`Unknown audio assertion: ${mode}`);
}
