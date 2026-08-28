/* Voice controls are browser-native and deliberately dependency-free. This
 * fixture evaluates their exact shipped source with a minimal Web Speech API
 * stand-in: it proves voice discovery, persisted selection, and that speaking
 * uses the selected local voice rather than inventing a provider request. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      function voiceSupported() {");
const end = app.indexOf("      async function sendPrompt(", begin);
assert.ok(begin >= 0 && end > begin, "voice controls must remain extractable");
const wavBegin = app.indexOf("      function concatenateVoiceChunks(chunks) {");
const wavEnd = app.indexOf("      function releaseHandsfreeCapture(capture) {");
assert.ok(wavBegin >= 0 && wavEnd > wavBegin, "hands-free WAV encoder must remain extractable");
const endpointBegin = app.indexOf("      function voiceSpeechThreshold(noiseFloor) {");
const endpointEnd = app.indexOf("      /*\n       * Local wake words", endpointBegin);
assert.ok(endpointBegin >= 0 && endpointEnd > endpointBegin, "adaptive endpoint functions must remain extractable");
const batchingBegin = app.indexOf("      function pendingVoiceBatch(pending, maxCharacters, maxParts = 5) {");
const batchingEnd = app.indexOf("      function pocketPlaybackLeadSeconds()", batchingBegin);
assert.ok(batchingBegin >= 0 && batchingEnd > batchingBegin, "Pocket voice batching policy must remain extractable");

class Element {
  constructor() { this.children = []; this.disabled = false; this.hidden = false; this.value = ""; this._text = ""; }
  appendChild(child) { this.children.push(child); return child; }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text; }
  set innerHTML(value) { this._text = String(value).replace(/<[^>]*>/g, " "); }
  get innerHTML() { return this._text; }
}

function load(voices, storedVoice = "") {
  const spoken = [];
  globalThis.document = { createElement: () => new Element() };
  globalThis.window = {
    speechSynthesis: {
      getVoices: () => voices,
      cancel: () => { spoken.length = 0; },
      speak: utterance => { spoken.push(utterance); queueMicrotask(() => utterance.onend?.()); },
    },
    SpeechSynthesisUtterance: function SpeechSynthesisUtterance(text) { this.text = text; },
  };
  globalThis.SpeechSynthesisUtterance = window.SpeechSynthesisUtterance;
  globalThis.markdown = text => text;
  globalThis.state = { settings: { voiceURI: storedVoice } };
  globalThis.voiceStatus = { tts_neural_ready: false };
  globalThis.voiceTraceEvent = () => Promise.resolve();
  globalThis.els = {
    voiceSettings: new Element(), voiceEnabled: new Element(), voiceSelect: new Element(),
    voicePreview: new Element(), voiceHelp: new Element(),
  };
  globalThis.installedVoices = [];
  const fns = eval(`(() => {${app.slice(begin, end)} return { voiceSupported, voiceText, hasRepeatedSpeechRun, selectedVoice, speakReply, refreshVoices }; })()`);
  return { fns, els: globalThis.els, spoken };
}

const voices = [
  { voiceURI: "ava", name: "Ava", lang: "en_US", default: true },
  { voiceURI: "lee", name: "Lee", lang: "en_GB", default: false },
];

{
  const { fns, els, spoken } = load(voices, "lee");
  fns.refreshVoices();
  assert.equal(els.voiceSelect.children.length, 2, "every installed local voice is offered");
  assert.equal(els.voiceSelect.value, "lee", "the persisted voice is restored when still installed");
  assert.equal(els.voiceSelect.disabled, false);
  fns.speakReply("Hello\nfrom Samosa");
  assert.equal(spoken.length, 1);
  assert.equal(spoken[0].text, "Hello from Samosa");
  assert.equal(spoken[0].voice.voiceURI, "lee", "speech uses the selected voice");
  assert.equal(fns.voiceText("Clear 🎉 this 👍🏽 #️⃣ 🇺🇸 please"), "Clear this please", "speech never receives emoji clusters or their modifiers");
  assert.equal(fns.voiceText("Details: https://example.com/a/b; local/path"), "Details, link local path", "visual punctuation and URLs are not read aloud as colon or slash");
  assert.equal(fns.voiceText("The population is 8.8 to 9 million."), "The population is 8 point 8 to 9 million.", "a decimal point is verbalized instead of silently discarded");
  assert.equal(fns.voiceText("Pi is 3.14159."), "Pi is 3 point 1 4 1 5 9.", "fractional digits are spoken individually instead of as one large integer");
  assert.equal(fns.voiceText("One sentence. Another sentence."), "One sentence. Another sentence.", "sentence-ending periods are not mistaken for decimals");
  assert.equal(fns.hasRepeatedSpeechRun("one two four four three three three what what"), true, "the known repeated-word stress case is detected");
  assert.equal(fns.hasRepeatedSpeechRun("one two three four"), false, "ordinary speech is not routed away from the selected neural voice");
  voiceStatus.tts_neural_ready = true;
  await fns.speakReply("one two four four three three three what what");
  assert.equal(spoken.at(-1).text, "one two four four three three three what what", "repeated words use the deterministic system voice fallback");
}

{
  const { fns, els } = load([], "missing");
  fns.refreshVoices();
  assert.equal(els.voiceSelect.disabled, true);
  assert.equal(els.voicePreview.disabled, true);
  assert.match(els.voiceHelp.textContent, /No browser or macOS voices/i);
}

{
  const { fns, els } = load([], "");
  let resumes = 0, starts = 0, requestedVoice = "";
  class LocalAudioContext {
    constructor() { this.state = "suspended"; this.destination = {}; this.currentTime = 0; }
    async resume() { resumes++; this.state = "running"; }
    createBuffer(_channels, length, sampleRate) {
      const data = new Float32Array(length);
      return { sampleRate, getChannelData: () => data };
    }
    createBufferSource() {
      return {
        connect() {}, disconnect() {},
        start() { starts++; queueMicrotask(() => this.onended?.()); },
        stop() {},
      };
    }
  }
  window.AudioContext = LocalAudioContext;
  globalThis.voiceStatus.tts_neural_ready = true;
  globalThis.voiceStatus.tts_engine = "pocket_native";
  let requestedUrl = "";
  globalThis.authFetch = async (url, request) => {
    requestedUrl = url;
    requestedVoice = JSON.parse(request.body).voice;
    let read = false;
    return {
      ok: true, status: 200,
      headers: { get: name => name === "X-Samosa-Sample-Rate" ? "24000" : null },
      body: { getReader: () => ({ read: async () => read ? { done: true } : (read = true, { done: false, value: new Uint8Array([0, 0, 0xff, 0x7f]) }) }) }
    };
  };
  fns.refreshVoices();
  assert.equal(els.voiceSelect.value, "pocket:caro", "Pocket defaults to Caro");
  await fns.speakReply("Hello from native Pocket.");
  assert.equal(requestedVoice, "caro", "native playback sends the selected Pocket voice");
  assert.equal(requestedUrl, "/v1/voice/speech/stream", "native playback uses incremental PCM rather than waiting for a complete WAV");
  assert.equal(resumes, 1, "native playback explicitly unlocks Web Audio");
  assert.equal(starts, 1, "the received WAV starts through the unlocked audio context");
}

{
  const { encodeVoiceWav } = eval(`(() => {${app.slice(wavBegin, wavEnd)} return { encodeVoiceWav }; })()`);
  const wav = encodeVoiceWav([new Float32Array([0, .5, -.5, 0, .25, -.25])], 48000);
  const view = new DataView(wav);
  const bytes = new Uint8Array(wav);
  assert.equal(String.fromCharCode(...bytes.slice(0, 4)), "RIFF");
  assert.equal(String.fromCharCode(...bytes.slice(8, 12)), "WAVE");
  assert.equal(view.getUint16(20, true), 1, "hands-free capture is PCM, not a browser media container");
  assert.equal(view.getUint16(22, true), 1, "hands-free capture is mono");
  assert.equal(view.getUint32(24, true), 16000, "hands-free capture is resampled to the gateway's exact rate");
  assert.equal(view.getUint16(34, true), 16, "hands-free capture is 16-bit PCM");
}

{
  const { voiceSpeechThreshold, voiceEndpointPauseMs } = eval(`(() => {${app.slice(endpointBegin, endpointEnd)} return { voiceSpeechThreshold, voiceEndpointPauseMs }; })()`);
  assert.ok(voiceSpeechThreshold(.012) > .03 && voiceSpeechThreshold(.012) < .04, "quiet rooms keep a sensitive speech threshold");
  assert.equal(voiceSpeechThreshold(.2), .09, "noisy rooms cannot raise the speech threshold without bound");
  assert.equal(voiceEndpointPauseMs({ speechStartedAt: 100, lastSpeechAt: 500, noiseFloor: .012 }), 700, "short utterances get extra endpoint protection");
  assert.equal(voiceEndpointPauseMs({ speechStartedAt: 100, lastSpeechAt: 1500, noiseFloor: .012 }), 600, "ordinary utterances stop faster than the old fixed delay");
  assert.equal(voiceEndpointPauseMs({ speechStartedAt: 100, lastSpeechAt: 2600, noiseFloor: .012 }), 520, "long clear utterances use the fastest endpoint");
  assert.equal(voiceEndpointPauseMs({ speechStartedAt: 100, lastSpeechAt: 2600, noiseFloor: .04 }), 720, "noisy rooms retain enough pause to avoid clipping speech");
}

{
  const { pendingVoiceBatch, pocketVoiceBatchReady } = eval(`(() => {${app.slice(batchingBegin, batchingEnd)} return { pendingVoiceBatch, pocketVoiceBatchReady }; })()`);
  assert.deepEqual(pendingVoiceBatch(["A".repeat(30), "B".repeat(139), "C".repeat(110)], 460),
    { parts: ["A".repeat(30), "B".repeat(139), "C".repeat(110)], characters: 281 },
    "short streamed sentences are combined into one stable Pocket generation");
  assert.equal(pendingVoiceBatch(["A".repeat(250), "B".repeat(250)], 460).parts.length, 1,
    "a voice batch stays within its bounded synthesis size");
  assert.equal(pocketVoiceBatchReady({ first: true, characters: 136, inputComplete: false, playbackLeadSeconds: 0, waitedMs: 0 }), true,
    "a coherent first sentence keeps the existing zero-wait fast path");
  assert.equal(pocketVoiceBatchReady({ first: true, characters: 4, inputComplete: false, playbackLeadSeconds: 0, waitedMs: 0 }), false,
    "a tiny opener is not synthesized as its own unstable voice");
  assert.equal(pocketVoiceBatchReady({ first: true, characters: 4, inputComplete: false, playbackLeadSeconds: 0, waitedMs: 360 }), true,
    "the tiny-opener guard is tightly bounded");
  assert.equal(pocketVoiceBatchReady({ first: false, characters: 170, inputComplete: false, playbackLeadSeconds: 8, waitedMs: 200 }), false,
    "queued audio is used to collect more voice-consistent text");
  assert.equal(pocketVoiceBatchReady({ first: false, characters: 170, inputComplete: false, playbackLeadSeconds: 4.5, waitedMs: 200 }), true,
    "batching yields before queued playback can underrun");
  assert.equal(pocketVoiceBatchReady({ first: false, characters: 10, inputComplete: true, playbackLeadSeconds: 8, waitedMs: 0 }), true,
    "a completed short reply flushes immediately");
}

assert.match(app, /id="voiceToggle"/, "the microphone control is shipped in the composer");
assert.match(app, /\/v1\/voice\/transcriptions/, "hands-free capture is sent only to the local voice route");
assert.match(app, /voice-tts-moss-nano/, "MOSS TTS Nano is offered by the voice catalogue UI");
assert.match(app, /voice-tts-kitten-nano/, "Kitten TTS Nano is offered by the voice catalogue UI");
assert.match(app, /\/v1\/voice\/tts\/runtime/, "TTS catalogue cards can prepare a local runtime");
assert.match(app, /browser_onnx_host\.html/, "MOSS uses the shipped browser-local ONNX host");
assert.match(app, /tts\/kitten\/worker\.js/, "Kitten uses the shipped browser-local worker");
assert.match(app, /browserTtsStorageKey/, "browser-local TTS downloads have persistent browser state");
assert.match(app, /browserTtsMarkDownloaded/, "browser-local TTS completion is recorded after preparation");
assert.match(app, /Verify and use/, "a cached browser-local TTS model is verified instead of silently re-downloaded");
assert.match(app, /no Python runtime/, "browser-local TTS cards explain that Python is not shipped");
assert.doesNotMatch(app, /isolated Python\/ONNX runtime/, "the UI does not promise a shipped Python TTS runtime");
assert.match(app, /makeVoiceParticles/, "the samosa particle visualization is shipped with the app");
assert.doesNotMatch(app, /background-size:\s*44px 44px/, "voice mode does not put the particle field on a decorative grid");
assert.match(app, /\.voice-canvas\s*\{[^}]*position:\s*absolute;[^}]*inset:\s*0;[^}]*width:\s*100%;[^}]*height:\s*100%/, "voice particles cover the full stage");
assert.match(app, /scatterX[\s\S]*targetDispersion/, "speech energy disperses quiet-form particles across the stage");
const particleRenderer = app.slice(app.indexOf("      function renderVoiceParticles"), app.indexOf("      function concatenateVoiceChunks"));
assert.doesNotMatch(particleRenderer, /\.clip\(/, "voice particles are never clipped into the samosa shape");
assert.match(particleRenderer, /1000 \/ 30/, "voice animation is capped so it leaves compute for local inference");
assert.match(app, /voiceConversationActive/, "voice mode remains active across turns");
assert.match(app, /I’ll respond when you pause/, "voice mode ends a turn from natural silence");
assert.doesNotMatch(app, /tap the microphone again when you/i, "voice mode does not instruct people to manually finish a turn");
assert.match(app, /Hi\. What can I help you with\?/, "voice mode greets before it begins listening");
assert.match(app, /function blankVoiceTranscript\(text\)/, "blank Whisper sentinel transcripts are rejected before chat");
assert.match(app, /\["blankaudio", "silence", "nospeech"/, "[BLANK_AUDIO] never becomes a user message");
assert.match(app, /if \(!capture\.speechStartedAt && capture\.speechFrames >= 2\)[\s\S]*capture\.limitTimer/, "the listening timeout begins only after speech starts");
assert.match(app, /Pocket streaming voice/, "the downloadable streaming neural voice is shown in Settings");
assert.match(app, /voiceCatalog\.filter/, "voice choices come from the model catalog");
assert.match(app, /model\.pros/, "voice cards explain model advantages");
assert.match(app, /model\.cons/, "voice cards explain model tradeoffs");
assert.match(app, /Use this STT/, "only one installed STT can be selected at a time");
assert.match(app, /Use this TTS/, "only one installed TTS can be selected at a time");
assert.match(app, /hasRepeatedSpeechRun/, "repeated-word TTS safety detection is shipped");
assert.match(app, /tts_safety_limit/, "unusually long native audio is stopped instead of looped indefinitely");
assert.match(app, /pocket_native/, "the browser recognizes the native Pocket engine");
assert.match(app, /startVoiceStream\(run, capture\.turnId\)/, "voice replies begin queuing while the model streams and retain a trace correlation id");
assert.match(app, /\/v1\/voice\/speech\/stream/, "neural replies use the incremental PCM endpoint");
assert.match(app, /waitForPocketVoiceBatch/, "Pocket adaptively batches later sentences instead of restarting its voice for each line");
assert.match(app, /voiceSelection: state\.voiceSelection/, "the selected reference voice is pinned across an entire streamed reply");
assert.match(app, /playbackLeadSeconds <= 4\.5/, "voice-consistency batching cannot consume the playback underrun guard");
assert.match(app, /if \(first\) return characters >= 96 \|\| waitedMs >= 360/, "a normal first sentence retains the fast no-delay path");
assert.match(app, /const minimumClause = 36, lookahead = 12/, "streamed text waits for a stable clause boundary before synthesis");
assert.doesNotMatch(app, /state\.spoke \? 90 : 44/, "speech never uses the old arbitrary 44-character cut that split names");
assert.match(app, /playback_buffer_underrun/, "diagnostics record audible playback-buffer gaps");
assert.match(app, /id="voiceTraceToggle"/, "Voice settings expose an explicit diagnostics start/stop control");
assert.match(app, /\/v1\/voice\/diagnostics\/event/, "browser timing events are saved through the local diagnostics route");
const endpointProcessor = app.slice(app.indexOf("      function processConversationAudio"), app.indexOf("      async function openMicrophoneCapture"));
assert.doesNotMatch(endpointProcessor, /1050/, "voice endpointing no longer has the fixed 1.05-second pause");
assert.match(app, /unlockVoicePlayback\(\)/, "voice replies use an unlocked Web Audio context rather than autoplay-prone HTML audio");
assert.doesNotMatch(app, /els\.voiceEnabled\.checked && !handsfreeInFlight/, "typed replies never start automatic playback");

process.stdout.write("voice UI fixtures: PASS\n");
