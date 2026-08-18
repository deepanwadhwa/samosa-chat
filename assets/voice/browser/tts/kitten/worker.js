import { KittenTTS } from './kitten-tts.browser.js';

const MAX_TEXT_CHARS = 720;
const MAX_AUDIO_SECONDS = 24;
const DEFAULT_VOICE = 'Jasper';

let model = null;
let preparing = null;
let requestCounter = 0;
const activeRequests = new Map();

function post(type, payload = {}, transfer = []) {
  self.postMessage({ type, ...payload }, transfer);
}

async function prepare() {
  if (model) return model;
  if (!preparing) {
    preparing = KittenTTS.from_pretrained('KittenML/kitten-tts-nano-0.8')
      .then((loaded) => { model = loaded; return loaded; })
      .finally(() => { preparing = null; });
  }
  return preparing;
}

async function synthesize(message) {
  const requestId = message.requestId || `kitten-${++requestCounter}`;
  const text = String(message.text || '').trim();
  if (!text) throw new Error('Kitten TTS received empty text.');
  if (text.length > MAX_TEXT_CHARS) throw new Error('Kitten TTS phrase is too long; split it into shorter phrases.');
  const request = { cancelled: false };
  activeRequests.set(requestId, request);
  try {
    const tts = await prepare();
    let samplesSent = 0;
    let sampleRate = 24000;
    post('started', { requestId });
    for await (const chunk of tts.stream(text, {
      voice: String(message.voice || DEFAULT_VOICE),
      clean: true,
    })) {
      if (request.cancelled) return;
      const audio = chunk?.audio;
      if (!audio?.data?.length) continue;
      sampleRate = Number(audio.sampling_rate) || sampleRate;
      const data = audio.data instanceof Float32Array ? audio.data : new Float32Array(audio.data);
      samplesSent += data.length;
      if (samplesSent > sampleRate * MAX_AUDIO_SECONDS) {
        throw new Error('Kitten TTS produced an unusually long utterance; playback was stopped for safety.');
      }
      const copy = data.slice();
      post('audio', { requestId, sampleRate, data: copy }, [copy.buffer]);
    }
    if (!request.cancelled) post('done', { requestId, sampleRate, samples: samplesSent });
  } finally {
    activeRequests.delete(requestId);
  }
}

self.addEventListener('message', (event) => {
  const message = event.data || {};
  if (message.type === 'prepare') {
    prepare()
      .then(() => post('ready'))
      .catch((error) => post('error', { error: error?.message || String(error) }));
    return;
  }
  if (message.type === 'synthesize') {
    synthesize(message).catch((error) => post('error', {
      requestId: message.requestId,
      error: error?.message || String(error),
    }));
    return;
  }
  if (message.type === 'cancel') {
    const request = activeRequests.get(message.requestId);
    if (request) request.cancelled = true;
  }
});
