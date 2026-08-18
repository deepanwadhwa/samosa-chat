/* The wake-word backend is intentionally dependency-free and lives beside the
 * existing browser microphone owner. Extracting the pure algorithm block lets
 * us exercise its safety properties without opening a microphone or needing a
 * browser automation dependency. */
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const app = readFileSync(new URL("../assets/app.html", import.meta.url), "utf8");
const begin = app.indexOf("      function normalizeWakeWord(text) {");
const end = app.indexOf("      function blankVoiceTranscript(text) {");
assert.ok(begin >= 0 && end > begin, "wake-word algorithm block must remain extractable");
const fns = eval(`(() => {const WAKE_PROFILE_KEY = "samosa-wake-profile-v1"; const voiceSpeechThreshold = noiseFloor => Math.max(.03, Math.min(.09, (Number(noiseFloor) || .012) * 1.8 + .01));${app.slice(begin, end)} return {
  wakeResample, wakeTrimSpeech, wakeExtractFeatures, wakeCosineDistance,
  wakeDtw, wakeDecision, wakeEffectiveThreshold, wakeProfileDecision, wakeSpeechBoundary, loadWakeProfile
}; })()`);

assert.equal(fns.wakeCosineDistance([1, 0], [1, 0]), 0);
assert.equal(fns.wakeCosineDistance([1, 0], [0, 1]), 1);
assert.equal(fns.wakeCosineDistance([0, 0], [0, 0]), 0);
assert.equal(fns.wakeCosineDistance([0, 0], [1, 0]), 1);

const same = [[1, 0], [0.8, 0.2], [0, 1]];
const stretched = [[1, 0], [1, 0], [0.8, 0.2], [0, 1]];
const different = [[0, 1], [0, 1], [0, 1]];
assert.equal(fns.wakeDtw(same, same), 0, "identical sequences must have zero DTW distance");
assert.ok(fns.wakeDtw(same, stretched) < fns.wakeDtw(same, different), "DTW must tolerate time stretching better than a different phrase");
assert.ok(Number.isFinite(fns.wakeDtw([[0, 0]], [[0, 0]])), "degenerate vectors must not produce NaN");
assert.equal(fns.wakeDtw([], []), 0);
assert.equal(fns.wakeDtw([], [[1, 0]]), Infinity);
assert.equal(fns.wakeEffectiveThreshold({ threshold: .08 }), .08, "the strict setting is preserved");
assert.equal(fns.wakeEffectiveThreshold({ threshold: .22 }), .18, "obsolete overly-relaxed profiles are capped");
assert.equal(fns.wakeEffectiveThreshold({}), .14, "new profiles default to balanced matching");

const sampleRate = 16000;
const audio = new Float32Array(sampleRate);
for (let i = sampleRate / 2; i < sampleRate * .9; i++) {
  const t = (i - sampleRate / 2) / sampleRate;
  audio[i] = Math.sin(2 * Math.PI * (180 + t * 900) * t) * .18;
}
const trimmed = fns.wakeTrimSpeech(audio, sampleRate);
assert.ok(trimmed.samples && trimmed.duration > .15 && trimmed.duration < 1, "speech trimming keeps the utterance and removes silence");
assert.equal(fns.wakeTrimSpeech(new Float32Array(sampleRate), sampleRate).samples, undefined, "silence is rejected");
assert.equal(fns.wakeTrimSpeech(Float32Array.from({ length: sampleRate / 2 }, () => 1), sampleRate).samples, undefined, "clipped audio is rejected");
const fan = Float32Array.from({ length: sampleRate }, (_, i) => .035 * Math.sin(i * .17) + .012 * Math.sin(i * .47));
assert.equal(fns.wakeTrimSpeech(fan, sampleRate).samples, undefined, "steady room noise is rejected");

const features = fns.wakeExtractFeatures(trimmed.samples, sampleRate);
assert.ok(features.length > 2 && features.every(vector => vector.length === 12), "speech features have a stable compact dimension");
assert.ok(features.every(vector => Array.from(vector).every(Number.isFinite)), "speech features contain no NaNs or infinities");
assert.ok(features.some(vector => Math.abs(vector[0]) > 1e-5), "the first spectral coefficient carries information instead of being identically zero");
const resampled = fns.wakeResample(trimmed.samples, sampleRate, 8000);
assert.equal(resampled.length, Math.floor(trimmed.samples.length / 2));

function wavDataUrl(samples) {
  const bytes = Buffer.alloc(44 + samples.length * 2);
  bytes.write("RIFF", 0); bytes.writeUInt32LE(36 + samples.length * 2, 4); bytes.write("WAVEfmt ", 8);
  bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(1, 22);
  bytes.writeUInt32LE(16000, 24); bytes.writeUInt32LE(32000, 28); bytes.writeUInt16LE(2, 32); bytes.writeUInt16LE(16, 34);
  bytes.write("data", 36); bytes.writeUInt32LE(samples.length * 2, 40);
  for (let i = 0; i < samples.length; i++) bytes.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(samples[i] * 32767))), 44 + i * 2);
  return `data:audio/wav;base64,${bytes.toString("base64")}`;
}
const recording = wavDataUrl(audio), storage = new Map();
globalThis.localStorage = { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) };
storage.set("samosa-wake-profile-v1", JSON.stringify({ version: 1, backend: "acoustic-template-v1", wake_word: "Paaro",
  sample_rate: 16000, feature_dimensions: 12, templates: [same, same, same], recordings: [recording, recording, recording], threshold: .22, aggregation: "consensus" }));
const migrated = fns.loadWakeProfile();
assert.equal(migrated.version, 2, "existing profiles with local recordings migrate to the corrected extractor");
assert.equal(migrated.backend, "acoustic-template-v2");
assert.equal(migrated.threshold, .14, "migration removes the old overly-relaxed setting");
assert.equal(JSON.parse(storage.get("samosa-wake-profile-v1")).version, 2, "the migrated profile is persisted locally");
delete globalThis.localStorage;

assert.deepEqual(fns.wakeDecision([.08, .10, .31], .15, "consensus"), { detected: true, score: .10, scores: [.08, .10, .31], required: 2 });
assert.equal(fns.wakeDecision([.08, .31, .32], .15, "consensus").detected, false);
assert.equal(fns.wakeDecision([.08, .31, .32], .15, "reference").detected, true);
const profile = { templates: [features, features, features], threshold: .14, aggregation: "consensus" };
assert.equal(fns.wakeProfileDecision(profile, features).detected, true, "three matching templates must agree");
assert.equal(fns.wakeProfileDecision(profile, [...features, ...features]).detected, false, "a similarly shaped utterance with the wrong duration is rejected");
const profileDecisionBlock = app.slice(app.indexOf("      function wakeProfileDecision"), app.indexOf("      function wakeRingPush"));
assert.doesNotMatch(profileDecisionBlock, /pairScores|wakeDtw\(a, b\)/,
  "reference-to-reference DTW must never be recomputed for each live microphone candidate");
const stressTemplate = Array.from({ length: 180 }, (_, frame) =>
  Array.from({ length: 12 }, (_, band) => Math.sin(frame * .071 + band * .23)));
const stressProfile = { templates: Array.from({ length: 6 }, () => stressTemplate), threshold: .14,
  aggregation: "consensus", duration_ms: 1200, cohesion: .08 };
const stressStarted = performance.now();
fns.wakeProfileDecision(stressProfile, stressTemplate);
assert.ok(performance.now() - stressStarted < 500,
  "a maximum-size six-recording wake comparison must not monopolize the UI thread");

const boundaryState = { wakeNoiseFloor: .012, wakeSpeechActive: false, wakeSpeechFrames: 0,
  wakeSpeechStartedAt: 0, wakeLastSpeechAt: 0, wakeSpeechPeak: 0, wakeIgnoreUntil: 0 };
let at = 0, boundaries = [];
for (const level of [.01, .01, .09, .09, .09, .09, .09, .01, .01, .01, .01, .01]) {
  at += 85;
  const boundary = fns.wakeSpeechBoundary(boundaryState, level, at, 4096, 48000);
  if (boundary) boundaries.push(boundary);
}
assert.equal(boundaries.length, 1, "one isolated utterance produces one decision rather than overlapping confirmations");
assert.equal(boundaries[0].validShape, true, "a short speech burst followed by silence becomes a candidate");
assert.equal(fns.wakeSpeechBoundary(boundaryState, .01, at + 85, 4096, 48000), null, "trailing silence cannot score the same utterance again");

const wakeSettingBegin = app.indexOf("      function setWakeListeningRequested(enabled) {");
const wakeSettingEnd = app.indexOf("      function wakeActivateConversation", wakeSettingBegin);
assert.ok(wakeSettingBegin >= 0 && wakeSettingEnd > wakeSettingBegin, "persistent wake setting helper must remain extractable");
const wakeSetting = eval(`(() => {
  const WAKE_ENABLED_KEY = "samosa-wake-enabled-v1", values = new Map();
  const localStorage = { getItem: key => values.get(key) || null, setItem: (key, value) => values.set(key, value), removeItem: key => values.delete(key) };
  let wakeListeningRequested = false, wakeProfile = { wake_word: "Paaro" };
  ${app.slice(wakeSettingBegin, wakeSettingEnd)}
  return { set: setWakeListeningRequested, enabled: () => wakeListeningRequested, stored: () => localStorage.getItem(WAKE_ENABLED_KEY), clearProfile: () => { wakeProfile = null; } };
})()`);
wakeSetting.set(true);
assert.equal(wakeSetting.enabled(), true, "turning wake listening on arms the persistent state");
assert.equal(wakeSetting.stored(), "1", "the armed state survives a page refresh");
wakeSetting.set(false);
assert.equal(wakeSetting.stored(), null, "explicitly stopping wake listening clears persistence");
wakeSetting.clearProfile();
wakeSetting.set(true);
assert.equal(wakeSetting.stored(), null, "wake listening cannot persist without an enrolled profile");

const activationBegin = app.indexOf("      function wakeActivateConversation(capture, result) {");
const activationEnd = app.indexOf("      function wakeResetCandidate", activationBegin);
const activationBlock = app.slice(activationBegin, activationEnd);
assert.match(activationBlock, /setWakeListeningRequested\(true\)/, "a wake match consumes only the capture, not the armed setting");
assert.doesNotMatch(activationBlock, /removeItem\(WAKE_ENABLED_KEY\)|wakeListeningRequested\s*=\s*false/, "a wake match must not make listening one-shot");

assert.match(app, /wakeRing = new Float32Array\(Math\.ceil\(capture\.context\.sampleRate \* 2\.05\)\)/, "runtime memory is bounded by a two-second ring");
assert.match(app, /capture\.wakeCooldownUntil = Date\.now\(\) \+ 2500/, "detected speech enters a refractory period");
assert.match(app, /localStorage\.removeItem\(WAKE_PROFILE_KEY\)/, "the profile has an explicit forget path");
assert.match(app, /Start recording/, "enrollment starts from an explicit user action");
assert.match(app, /Sound detected/, "enrollment gives live sound feedback");
assert.match(app, /3 seconds each/, "each enrollment attempt has a clear fixed duration");
assert.match(app, /How many times should I say it/, "enrollment count is user-configurable");
assert.match(app, /Next recording/, "enrollment supports stepping through multiple recordings");
assert.match(app, /Your recordings — click Play to check them/, "saved recordings are exposed for review");
assert.match(app, /function playWakeRecording/, "saved recordings have playback controls");
assert.match(app, /id="wakeWordListen"/, "wake listening has an explicit button");
assert.match(app, /function toggleWakeWordListening/, "wake listening is user-controlled");
assert.match(app, /localStorage\.getItem\(WAKE_ENABLED_KEY\) === "1"/, "the armed wake setting is restored when the app opens again");
const turnCompletionBlock = app.slice(app.indexOf("      async function stopHandsfree(send)"), app.indexOf("      function cancelHandsfree()"));
const continueVoiceAt = turnCompletionBlock.indexOf("if (completed && voiceConversationActive)");
const rearmWakeAt = turnCompletionBlock.indexOf("else if (wakeListeningRequested && wakeProfile)");
assert.ok(continueVoiceAt >= 0 && rearmWakeAt > continueVoiceAt,
  "a completed wake-triggered turn stays in multi-turn Voice instead of dropping back to background wake listening");
assert.match(turnCompletionBlock, /startHandsfree\(\{ greet: false \}\)/,
  "hands-free capture restarts after a completed wake-triggered turn");
assert.match(app, /const resumeWake = wakeListeningRequested && !!wakeProfile/, "ending a wake-triggered Voice turn preserves the armed setting");
assert.match(app, /wakeListenerStarting/, "concurrent health checks cannot open duplicate microphones while wake listening starts");
assert.match(app, /hideVoiceStage\(\);/, "wake listening stays in the background on the home screen");
assert.doesNotMatch(app, /Waiting for wake word/, "wake listening does not put confusing waiting copy on the home screen");
assert.match(app, /capture\.onAudio = chunk => processConversationAudio\(capture, chunk\)/, "confirmed wake words hand off to regular Voice capture");
assert.doesNotMatch(app, /verifyWakeWordCandidate/, "wake detection does not hard-gate arbitrary words through Whisper");
assert.doesNotMatch(app, /wakeWordEnabled/, "the old implicit wake-word checkbox is removed");
assert.match(app, /now - state\.wakeLastSpeechAt < 260/, "wake candidates are evaluated only after trailing silence");
assert.doesNotMatch(app, /wakePositiveChecks/, "overlapping windows are not miscounted as independent confirmations");
assert.match(app, /durationRatio >= \.62 && durationRatio <= 1\.60/, "runtime candidates must resemble the enrolled duration");
assert.match(app, /wake_candidate_evaluated/, "debug timing logs include accepted and rejected candidate scores");
assert.match(app, /function queueWakeCandidateEvaluation/, "wake matching is queued outside the microphone callback");
assert.match(app, /setTimeout\(\(\) => \{[\s\S]*wakeEvaluateCandidate/, "wake DTW yields before running on the browser main thread");
assert.match(app, /acoustic-template-v2/, "corrected profiles use a versioned feature extractor");
assert.match(app, /noiseFloor \* 1\.5/, "wake detection rejects steady background noise");
assert.doesNotMatch(app, /Detection threshold/, "technical threshold wording is not exposed in the main UI");
assert.match(app, /How forgiving should matching be/, "advanced matching uses plain language");
assert.match(app, />Balanced<\/output>/, "advanced matching does not expose an unexplained raw score by default");
process.stdout.write("wake-word UI fixtures: PASS\n");
