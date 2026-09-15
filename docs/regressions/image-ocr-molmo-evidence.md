# Image OCR and Molmo evidence regression

## Failure

An ordinary image attachment from the app was tagged `analysis_depth=fast`.
The gateway interpreted that as OCR-only and explicitly disabled the visual
specialist. In addition, the image harness expected a top-level `text` value
from OCR even though the native Tesseract adapter returns a `lines` array.
Successful OCR was therefore treated as unavailable. The final chat model saw
only a fast-scan status message and described that internal message instead of
the image.

## Contract

Every image turn now gathers both kinds of evidence. The deterministic routing
floor sets `read_text=true` and `inspect_visual=true`; neither the model planner
nor an API `analysis_depth=fast` request may remove either operation. When the
verified Molmo2 package is installed, it is the visual specialist. VisionPsy or
native chat vision remains the existing fallback when Molmo2 is absent.

The final chat model receives independent sections:

```text
OCR:
<literal text recognized by Tesseract, or an explicit empty/failure status>

MOLMO IMAGE DESCRIPTION:
<Molmo's description of scene, layout, objects, and relationships>
```

OCR line objects are joined in reading order into bounded text. An empty OCR
result is distinguished from reader failure; neither is interpreted as proof
that the image contains no text.

## Gate

`make test-molmo2-gateway` supplies a line-array OCR fixture and a Molmo fixture,
then verifies that even an explicit fast image request invokes Molmo exactly
once and that both labelled sections reach the selected final text model.
`node tests/test_composer_ui.mjs` verifies that the app reserves automatic fast
mode for document-only turns.
