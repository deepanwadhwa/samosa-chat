#!/usr/bin/env python3
"""Dump golden tensors from the NumPy OCR reference for the C sidecar test.

The C forward pass (R2/R3) is validated against these, feeding *identical*
preprocessed input tensors so image-resampling differences are isolated from
the neural math. Simple binary format the C test reads:

  per tensor:  u32 name_len | name bytes | u32 ndim | u32 dims[ndim] | f32 data
  file:        u32 n_tensors | tensors...   then a trailing text blob:
               u32 text_len | utf-8 text (the expected recognizer decode)

Usage: python tools/dump_ocr_golden.py --pack <dir> --image <png> --out <dir>
"""
import argparse, os, struct, sys
import numpy as np
import cv2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ocr_ref


def write_tensors(path, tensors, text=""):
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(tensors)))
        for name, arr in tensors.items():
            a = np.ascontiguousarray(arr, dtype="<f4")
            nb = name.encode()
            f.write(struct.pack("<I", len(nb))); f.write(nb)
            f.write(struct.pack("<I", a.ndim))
            f.write(struct.pack("<%dI" % a.ndim, *a.shape))
            f.write(a.tobytes())
        tb = text.encode("utf-8")
        f.write(struct.pack("<I", len(tb))); f.write(tb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rdr = ocr_ref.Reader(args.pack)
    img = cv2.imread(args.image)

    # --- detector: input tensor -> prob map ---
    det_in, shape = ocr_ref.preprocess_det(img, rdr.det_meta)
    det_prob = rdr.det.forward(det_in)
    write_tensors(os.path.join(args.out, "det.gold"),
                  {"det_in": det_in, "det_prob": det_prob})
    print(f"det.gold: in{det_in.shape} -> prob{det_prob.shape}")

    # --- recognizer: take the first detected crop, dump its input + softmax + text ---
    boxes, _, _ = rdr.detect(img)
    crop = ocr_ref.minarea_crop(img, np.array(boxes[0][0]))
    base = rdr.rec_meta["preprocess"]["max_wh_ratio"]
    mwr = max(base, crop.shape[1] / float(crop.shape[0]))
    rec_in = ocr_ref.preprocess_rec(crop, mwr)[None]
    rec_prob = rdr.rec.forward(rec_in.astype(np.float32))
    text, conf = ocr_ref.ctc_decode(rec_prob[0], rdr.charset)
    # store argmax + maxprob per timestep (compact) instead of full 18710-wide logits
    idx = rec_prob[0].argmax(-1).astype(np.float32)[None]
    mx = rec_prob[0].max(-1).astype(np.float32)[None]
    write_tensors(os.path.join(args.out, "rec.gold"),
                  {"rec_in": rec_in.astype(np.float32),
                   "rec_argmax": idx, "rec_maxprob": mx},
                  text=text)
    print(f"rec.gold: in{rec_in.shape} -> T={rec_prob.shape[1]} text={text!r} conf={conf:.4f}")


if __name__ == "__main__":
    main()
