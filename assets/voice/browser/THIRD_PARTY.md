# Browser-local TTS runtime attributions

Samosa ships browser JavaScript/WASM adapters so optional TTS models can run
locally in a browser without shipping Python.

- MOSS browser ONNX reader/runtime: OpenMOSS, from
  https://github.com/OpenMOSS/MOSS-TTS-Nano-Reader
- Kitten browser bundle: `kitten-tts-js`, from
  https://github.com/KittenML/kitten-tts-js
- ONNX Runtime Web is included by the upstream browser bundles. Its license
  and notices are included by those upstream distributions.

The model weights are not bundled in the application. Clicking Download
fetches the model files from the model publisher and stores them in the
browser's origin-managed storage.
