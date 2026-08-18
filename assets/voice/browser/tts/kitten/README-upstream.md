# kitten-tts-js

> JavaScript/TypeScript port of [KittenTTS](https://github.com/KittenML/KittenTTS) — ultra-lightweight neural TTS via ONNX. Works in Node.js, browser (WebAssembly), and any JS environment. Zero Python dependency.

[![npm version](https://img.shields.io/npm/v/kitten-tts-js)](https://www.npmjs.com/package/kitten-tts-js)
[![CI](https://github.com/Algiras/kitten-tts-js/actions/workflows/ci.yml/badge.svg)](https://github.com/Algiras/kitten-tts-js/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](./LICENSE)

**[Live Demo →](https://algiras.github.io/kitten-tts-js)** · **[npm →](https://www.npmjs.com/package/kitten-tts-js)** · **[GitHub →](https://github.com/Algiras/kitten-tts-js)**

> **Based on [KittenTTS](https://github.com/KittenML/KittenTTS) by [KittenML / Stellon Labs](https://github.com/KittenML)**
> — original Python library: [github.com/KittenML/KittenTTS](https://github.com/KittenML/KittenTTS)
> — original models & voices: [huggingface.co/KittenML](https://huggingface.co/KittenML)
>
> All credit for the models, architecture, and voice embeddings goes to them.
> Licensed under [Apache 2.0](./LICENSE). See [NOTICE](./NOTICE) for full attribution.

> **Disclaimer:** This is an **unofficial** community port made by a hobbyist who needed KittenTTS in JavaScript.
> It is **not** affiliated with, endorsed by, or supported by KittenML or Stellon Labs.

---

## Features

- **Ultra-lightweight** — nano model is ~25 MB
- **Runs anywhere** — Node.js (CPU), browser (WASM), Cloudflare Workers
- **8 voices** — Bella, Luna, Rosie, Kiki, Leo, Jasper, Bruno, Hugo
- **StyleTTS2-based** ONNX models from HuggingFace
- **Streaming support** — sentence-by-sentence async generator
- **TypeScript declarations** included
- **Automatic caching** — `~/.cache/kitten-tts/` in Node, Cache API in browser

---

## Install

```bash
npm install kitten-tts-js
```

---

## Quick Start

### Node.js

```js
import { KittenTTS } from 'kitten-tts-js';

const tts = await KittenTTS.from_pretrained('KittenML/kitten-tts-nano-0.8');

console.log(tts.list_voices());
// → ['Bella', 'Jasper', 'Luna', 'Bruno', 'Rosie', 'Hugo', 'Kiki', 'Leo']

const audio = await tts.generate('Hello from KittenTTS!', { voice: 'Bella' });
await audio.save('output.wav');
```

### Browser (inline)

```html
<script type="module">
  import { KittenTTS } from 'https://esm.sh/kitten-tts-js';

  const tts = await KittenTTS.from_pretrained('KittenML/kitten-tts-nano-0.8');
  const audio = await tts.generate('Hello!', { voice: 'Luna' });

  const audioCtx = new AudioContext();
  const source = audioCtx.createBufferSource();
  source.buffer = audio.toAudioBuffer(audioCtx);
  source.connect(audioCtx.destination);
  source.start();
</script>
```

### Browser (Web Worker — recommended for production)

Running inference in a Worker keeps the UI thread responsive during the ~5–10 s model load and synthesis.

**`worker.js`**
```js
import { KittenTTS } from 'https://esm.sh/kitten-tts-js';
let tts;

self.onmessage = async ({ data }) => {
  if (data.type === 'load') {
    tts = await KittenTTS.from_pretrained(data.modelId);
    self.postMessage({ type: 'ready' });
  }
  if (data.type === 'generate') {
    const audio = await tts.generate(data.text, data.opts);
    const buf = new Float32Array(audio.data);
    self.postMessage({ type: 'audio', buf, sampleRate: audio.sampling_rate }, [buf.buffer]);
  }
};
```

**`main.js`**
```js
const worker = new Worker('./worker.js', { type: 'module' });
worker.postMessage({ type: 'load', modelId: 'KittenML/kitten-tts-nano-0.8' });

worker.onmessage = ({ data }) => {
  if (data.type === 'ready') console.log('Model loaded!');
  if (data.type === 'audio') playFloat32(data.buf, data.sampleRate);
};

worker.postMessage({ type: 'generate', text: 'Hello world!', opts: { voice: 'Bella' } });

function playFloat32(buf, sampleRate) {
  const audioCtx = new AudioContext({ sampleRate });
  const ab = audioCtx.createBuffer(1, buf.length, sampleRate);
  ab.copyToChannel(buf, 0);
  const src = audioCtx.createBufferSource();
  src.buffer = ab;
  src.connect(audioCtx.destination);
  src.start();
}
```

### Streaming (sentence-by-sentence)

```js
let i = 0;
for await (const { text, audio } of tts.stream(longText, { voice: 'Leo' })) {
  console.log(`Chunk: "${text}" → ${audio.duration.toFixed(1)}s`);
  await audio.save(`chunk-${i++}.wav`);
}
```

---

## API

### `KittenTTS.from_pretrained(modelId?, opts?)`

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `modelId` | `string` | `'KittenML/kitten-tts-nano-0.8'` | HuggingFace repo ID |
| `opts.cacheDir` | `string` | `~/.cache/kitten-tts` | Override cache dir (Node) |

### `tts.generate(text, opts?)`

Returns `Promise<RawAudio>`.

| Opt | Default | Description |
|-----|---------|-------------|
| `voice` | `'Leo'` | Voice name (see table below) |
| `speed` | `1.0` | Speed multiplier (0.5–2.0) |
| `clean` | `true` | Run text preprocessor (numbers, currency, etc.) |

### `tts.stream(text, opts?)`

Returns `AsyncGenerator<{ text: string, audio: RawAudio }>` — one chunk per sentence.

### `tts.list_voices()`

Returns `string[]` of available friendly voice names.

### `tts.release()`

Releases the underlying ONNX session to free WebAssembly memory. Useful when switching models in the browser.

### `RawAudio`

| Member | Description |
|--------|-------------|
| `.data` | `Float32Array` — raw PCM mono |
| `.sampling_rate` | `24000` |
| `.duration` | Duration in seconds |
| `.toWav()` | `ArrayBuffer` — 16-bit PCM WAV |
| `.save(path)` | Write WAV file (Node.js) |
| `.toBlob()` | `Blob` for browser download/playback |
| `.toAudioBuffer(ctx)` | Web Audio `AudioBuffer` |

---

## Available Models

| Model ID | Size | Speed | Quality |
|----------|------|-------|---------|
| `KittenML/kitten-tts-nano-0.8` | ~25 MB | ★★★ | ★★☆ |
| `KittenML/kitten-tts-micro-0.8` | ~40 MB | ★★☆ | ★★★ |
| `KittenML/kitten-tts-mini-0.8` | ~80 MB | ★☆☆ | ★★★ |

---

## Available Voices

| Friendly Name | Gender |
|---------------|--------|
| Bella | Female |
| Jasper | Male |
| Luna | Female |
| Bruno | Male |
| Rosie | Female |
| Hugo | Male |
| Kiki | Female |
| Leo | Male |

---

## Development

```bash
git clone https://github.com/Algiras/kitten-tts-js.git
cd kitten-tts-js
npm install
npm test              # run unit tests
npm run build:pages   # build browser bundle → docs/
```

---

## Architecture

```
src/
├── kitten-tts.js    Main class: from_pretrained, generate, stream
├── preprocess.js    Number/currency/time text normalization
├── text-cleaner.js  Phoneme → token IDs (IPA symbol table)
├── phonemizer.js    eSpeak-NG WASM phonemization
├── npz-loader.js    NumPy .npz binary parser
├── model-loader.js  HuggingFace Hub download + caching
├── audio.js         RawAudio class + WAV encoder
└── index.js         Public API re-exports
```

---

## License

[Apache 2.0](./LICENSE) — see [NOTICE](./NOTICE) for attribution to the original [KittenTTS](https://github.com/KittenML/KittenTTS) by KittenML / Stellon Labs.
