/* Voice controls are browser-native and deliberately dependency-free. This
 * fixture evaluates their exact shipped source with a minimal Web Speech API
 * stand-in: it proves voice discovery, persisted selection, and that speaking
 * uses the selected local voice rather than inventing a provider request. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      function voiceSupported() {");
const end = app.indexOf("      async function sendPrompt(text) {");
assert.ok(begin >= 0 && end > begin, "voice controls must remain extractable");
const wavBegin = app.indexOf("      function concatenateVoiceChunks(chunks) {");
const wavEnd = app.indexOf("      function releaseHandsfreeCapture(capture) {");
assert.ok(wavBegin >= 0 && wavEnd > wavBegin, "hands-free WAV encoder must remain extractable");

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
      speak: utterance => spoken.push(utterance),
    },
    SpeechSynthesisUtterance: function SpeechSynthesisUtterance(text) { this.text = text; },
  };
  globalThis.SpeechSynthesisUtterance = window.SpeechSynthesisUtterance;
  globalThis.markdown = text => text;
  globalThis.state = { settings: { voiceURI: storedVoice } };
  globalThis.voiceStatus = { tts_neural_ready: false };
  globalThis.els = {
    voiceSettings: new Element(), voiceEnabled: new Element(), voiceSelect: new Element(),
    voicePreview: new Element(), voiceHelp: new Element(),
  };
  globalThis.installedVoices = [];
  const fns = eval(`(() => {${app.slice(begin, end)} return { voiceSupported, voiceText, selectedVoice, speakReply, refreshVoices }; })()`);
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
    constructor() { this.state = "suspended"; this.destination = {}; }
    async resume() { resumes++; this.state = "running"; }
    async decodeAudioData() { return { decoded: true }; }
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
  globalThis.authFetch = async (_url, request) => {
    requestedVoice = JSON.parse(request.body).voice;
    return { ok: true, arrayBuffer: async () => new ArrayBuffer(44) };
  };
  fns.refreshVoices();
  assert.equal(els.voiceSelect.value, "kokoro:bella", "Kokoro defaults to Bella");
  await fns.speakReply("Hello from native Kokoro.");
  assert.equal(requestedVoice, "bella", "native playback sends the selected Kokoro voice");
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

assert.match(app, /id="voiceToggle"/, "the microphone control is shipped in the composer");
assert.match(app, /\/v1\/voice\/transcriptions/, "hands-free capture is sent only to the local voice route");
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
assert.match(app, /Kokoro-82M/, "the downloadable native neural voice is shown in Settings");
assert.doesNotMatch(app, /Pocket TTS/, "Settings must not advertise the removed Python voice path");
assert.match(app, /startVoiceStream\(run\)/, "voice replies begin queuing while the model streams");
assert.match(app, /unlockVoicePlayback\(\)/, "voice replies use an unlocked Web Audio context rather than autoplay-prone HTML audio");
assert.doesNotMatch(app, /els\.voiceEnabled\.checked && !handsfreeInFlight/, "typed replies never start automatic playback");

process.stdout.write("voice UI fixtures: PASS\n");
