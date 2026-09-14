# PDF reading plan

Status: native text/image reading and the Tesseract OCR replacement are implemented.
Scanned, mixed, dense-text and multipage fixtures pass through the document reader.
See [OCR_TESSERACT.md](OCR_TESSERACT.md) for architecture, measured limits and
clean-install verification. Full Chat planning/answer quality is a separate gate.

## Required processing

1. Extract the PDF's native, structured text first. If structured text exists, read and retain it.
2. Independently check for embedded images. Read those images as well, even when the PDF also contains structured text. Recover their text through OCR.
3. Keep the two sources separate and explicitly labelled:
   - **STRUCTURED TEXT:** text extracted directly from the PDF's native text layer.
   - **OCR:** text recovered from images through OCR.
4. If the PDF has no structured text at all, OCR its scanned page images. Leave the document's **STRUCTURED TEXT** field empty and put the recovered text in **OCR**.
5. Combine the available structured-text and OCR evidence, preserving those labels, and send it to the LLM together with the user's question. The LLM answers from that combined evidence.

The presence of structured text must not cause embedded images to be skipped. OCR output must never be presented as structured text. A filename is not a substitute for the required document reading.

## Input to the LLM

For a PDF containing native text and images:

```text
QUESTION: [user's question]
STRUCTURED TEXT: [native text extracted from the PDF]
OCR: [text recovered from the PDF's images]
```

For a scanned PDF with no native text:

```text
QUESTION: [user's question]
STRUCTURED TEXT:
OCR: [text recovered from scanned page images]
```

## Original failure report

Before the Tesseract replacement, the user reported this error after `make install`:

> The document reader failed (ocr_timeout). This does not mean the requested information is absent from the file.

The slow custom OCR implementation has been removed. The direct PDF OCR command
uses Tesseract without a timeout; the gateway retains its cancellation/watchdog
controls. Dense-page and clean-install tests supplement the tiny fixture.
Failed OCR must never be interpreted as evidence that requested information is absent.

## Verification required

- A native-text PDF supplies its extracted text in **STRUCTURED TEXT**.
- A PDF with both native text and embedded images supplies both **STRUCTURED TEXT** and **OCR**, without skipping the images.
- A scanned PDF supplies **OCR** and leaves **STRUCTURED TEXT** empty.
- The LLM receives the question and the combined, labelled evidence.
- OCR reliability is verified beyond the small synthetic fixture, including the unresolved timeout behavior.

Do not open or read the user's previously referenced PDF for this work. Use independent test fixtures.
