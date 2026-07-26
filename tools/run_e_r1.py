#!/usr/bin/env python3
"""E-R1 — export + numeric validation of the PP-OCRv6 small OCR pack.

RUN-FIRST experiment for docs/TASKS_READER.md (the E-V1 pattern). Deliverables:

  (a) pack format frozen                    -> tools/ocr_pack.py + a built pack
  (b) NumPy port matches PaddleOCR           -> tensor max-abs-diff + line-for-line
  (c) T_ACCEPT / T_DECIDE calibrated         -> from correct/incorrect conf histograms
  (d) accuracy at 768 vs 1536 px long edge   -> the render-cap decision, measured
  (e) small vs medium tier decision          -> which fixture lines medium fixes

Fixtures are synthetic *printed* pages with known ground truth, spanning clean
-> degraded (small, low-contrast, noisy, blurred, rotated) so the confidence
histograms have both correct and incorrect lines to separate. This is honest
about scope: printed text + Latin diacritics is what E-R1 measures; photographed
receipts and handwriting are E-R2 / R5 territory (do not claim them here).

Usage:
  python tools/run_e_r1.py --pack <pack_dir> --src <hf_src_dir> \
      --out docs/regressions/reader [--medium-src <dir>]
"""

import argparse
import difflib
import json
import os
import sys
import time

import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ocr_ref  # noqa: E402

FONTS = "/System/Library/Fonts/Supplemental/"
FONT_FILES = ["Arial.ttf", "Times New Roman.ttf", "Courier New.ttf",
              "Georgia.ttf", "Verdana.ttf", "Trebuchet MS.ttf"]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


def render_page(lines, fname, fnt="Arial.ttf", size=34, pad=28, lh=1.55,
                bg=255, fg=0, width=820):
    n = len(lines)
    H = int(pad * 2 + n * size * lh)
    img = Image.new("RGB", (width, H), (bg, bg, bg))
    d = ImageDraw.Draw(img)
    f = font(fnt, size)
    y = pad
    for ln in lines:
        d.text((pad, y), ln, font=f, fill=(fg, fg, fg))
        y += int(size * lh)
    img.save(fname)
    return fname


def degrade(fname, out, scale=1.0, noise=0.0, blur=0, rotate=0.0, jpeg=0):
    img = cv2.imread(fname)
    if scale != 1.0:
        img = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
        img = cv2.resize(img, None, fx=1 / scale, fy=1 / scale, interpolation=cv2.INTER_LINEAR)
    if rotate:
        h, w = img.shape[:2]
        M = cv2.getRotationMatrix2D((w / 2, h / 2), rotate, 1.0)
        img = cv2.warpAffine(img, M, (w, h), borderValue=(255, 255, 255))
    if blur:
        img = cv2.GaussianBlur(img, (blur, blur), 0)
    if noise:
        img = np.clip(img.astype(np.float32) + np.random.normal(0, noise * 255, img.shape), 0, 255).astype(np.uint8)
    if jpeg:
        ok, enc = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, jpeg])
        img = cv2.imdecode(enc, cv2.IMREAD_COLOR)
    cv2.imwrite(out, img)
    return out


def make_fixtures(outdir):
    """Return list of {file, lines(gt), tags}. Deterministic (fixed seed)."""
    np.random.seed(1234)
    os.makedirs(outdir, exist_ok=True)
    fx = []

    # The concrete Bonsai failure cases: diacritic names + printed years.
    diac = ["Poličar Jana 1987", "Zoë Brontë résumé", "Håkon Ærøskøbing 2019",
            "François Peña über", "Søren Kaffée 1923", "Måns Öström naïve"]
    render_page(diac, f"{outdir}/f01_diacritics.png", "Georgia.ttf", 36)
    fx.append({"file": "f01_diacritics.png", "lines": diac, "tags": ["diacritics", "clean"]})

    # A receipt-like column layout.
    receipt = ["GROCERY MART #418", "Date 2023-04-17 14:22", "Milk 2% 1gal    4.29",
               "Sourdough loaf     5.50", "Eggs dozen large   3.99", "Coffee beans 12oz 12.75",
               "SUBTOTAL          26.53", "TAX 8.25%          2.19", "TOTAL             28.72"]
    render_page(receipt, f"{outdir}/f02_receipt.png", "Courier New.ttf", 30)
    fx.append({"file": "f02_receipt.png", "lines": receipt, "tags": ["receipt", "clean"]})

    # A form: label / value pairs.
    form = ["PATIENT INTAKE FORM", "Name: John Q. Doe", "DOB: 03/14/1987",
            "MRN: 8842190", "Phone: (415) 555-0182", "Allergies: penicillin",
            "Primary: Dr. Sarah Whitfield"]
    render_page(form, f"{outdir}/f03_form.png", "Arial.ttf", 32)
    fx.append({"file": "f03_form.png", "lines": form, "tags": ["form", "clean"]})

    # A dense paragraph page (small text, many lines).
    dense = [
        "The quick brown fox jumps over the lazy dog while 12 storks fly south.",
        "Invoice number 44821 was issued on 2022-11-03 for the amount of 1,204.55.",
        "Please remit payment to account 000-1234-5678 within thirty (30) days.",
        "Contact support@example.com or call +1-800-555-0199 for any questions.",
        "Reference codes: AB-7743, XG-0091, and QQ-1288 must appear on the check.",
        "Late fees of 1.5% per month accrue on balances past the due date shown.",
        "Terms and conditions apply; see section 4.2 of the master agreement.",
        "Shipment tracking 1Z999AA10123456784 delivered on Friday at 3:47 PM.",
    ]
    render_page(dense, f"{outdir}/f04_dense.png", "Times New Roman.ttf", 26, width=980)
    fx.append({"file": "f04_dense.png", "lines": dense, "tags": ["dense", "clean"]})

    # Numbers / codes / punctuation stress.
    codes = ["SN: 4F2A-99B1-7C03-DDE8", "IBAN GB29 NWBK 6016 1331 9268 19",
             "$1,299.00  €1,150.75  £980.20", "Lat 37.7749 Lon -122.4194",
             "Order #A-5567/2024 x3 @ 19.99", "ISBN 978-3-16-148410-0"]
    render_page(codes, f"{outdir}/f05_codes.png", "Verdana.ttf", 30)
    fx.append({"file": "f05_codes.png", "lines": codes, "tags": ["codes", "clean"]})

    # Mixed fonts, clean.
    for i, fn in enumerate(["Trebuchet MS.ttf", "Georgia.ttf", "Arial.ttf"]):
        lines = [f"Sample {fn.split('.')[0]} line one 2021",
                 "Recognition test of printed English text",
                 "Amount due: 3,417.60 on 09/30/2024"]
        render_page(lines, f"{outdir}/f0{6+i}_font{i}.png", fn, 34)
        fx.append({"file": f"f0{6+i}_font{i}.png", "lines": lines, "tags": ["fonts", "clean"]})

    # --- Degraded variants (populate the low-confidence / error region) ---
    base_small = ["Fine print clause 7 dated 2019", "micro id 88a2f rev 4",
                  "balance 1042.18 usd net 30", "see note 12 subsection b"]
    render_page(base_small, f"{outdir}/_base_small.png", "Arial.ttf", 16, width=560)
    base_form = ["Claim 55810 approved 2020", "Adjuster: M. Delacroix",
                 "Payout 4,210.00 net", "Policy PX-7742-Q"]
    render_page(base_form, f"{outdir}/_base_form.png", "Times New Roman.ttf", 30)

    degset = [
        ("f09_tiny", "_base_small.png", base_small, dict(scale=0.45), ["degraded", "small"]),
        ("f10_blur", "_base_form.png", base_form, dict(blur=5), ["degraded", "blur"]),
        ("f11_noise", "_base_form.png", base_form, dict(noise=0.16), ["degraded", "noise"]),
        ("f12_rot", "_base_form.png", base_form, dict(rotate=5.0), ["degraded", "rotate"]),
        ("f13_jpeg", "_base_small.png", base_small, dict(jpeg=18, scale=0.6), ["degraded", "jpeg"]),
        ("f14_lowcon", "_base_form.png", base_form, dict(), ["degraded", "lowcontrast"]),
        ("f15_blurnoise", "_base_small.png", base_small, dict(blur=3, noise=0.10), ["degraded", "blur", "noise"]),
        ("f16_smallrot", "_base_small.png", base_small, dict(scale=0.55, rotate=3.5), ["degraded", "small", "rotate"]),
        # aggressive: these are meant to *fail* / read low-confidence, giving the
        # calibration an incorrect-line population to separate.
        ("f22_microblur", "_base_small.png", base_small, dict(scale=0.30, blur=5), ["degraded", "hard"]),
        ("f23_heavyblur", "_base_form.png", base_form, dict(blur=9), ["degraded", "hard"]),
        ("f24_heavynoise", "_base_form.png", base_form, dict(noise=0.32), ["degraded", "hard"]),
        ("f25_jpegtiny", "_base_small.png", base_small, dict(jpeg=6, scale=0.4), ["degraded", "hard"]),
        ("f26_blurnoisehard", "_base_small.png", base_small, dict(scale=0.5, blur=5, noise=0.22), ["degraded", "hard"]),
        ("f27_bigrot", "_base_form.png", base_form, dict(rotate=9.0, blur=3), ["degraded", "hard"]),
    ]
    for name, src, gt, kw, tags in degset:
        out = f"{outdir}/{name}.png"
        if "lowcontrast" in tags:
            img = cv2.imread(f"{outdir}/{src}")
            img = (img.astype(np.float32) * 0.35 + 150).clip(0, 255).astype(np.uint8)
            cv2.imwrite(out, img)
        else:
            degrade(f"{outdir}/{src}", out, **kw)
        fx.append({"file": name + ".png", "lines": gt, "tags": tags})

    # A couple more clean pages to comfortably exceed 20 fixtures.
    misc1 = ["MONTHLY STATEMENT", "Opening balance 12,004.19", "Deposits 3,200.00",
             "Withdrawals 4,118.44", "Closing balance 11,085.75", "As of 2024-06-30"]
    render_page(misc1, f"{outdir}/f17_statement.png", "Courier New.ttf", 28)
    fx.append({"file": "f17_statement.png", "lines": misc1, "tags": ["statement", "clean"]})

    misc2 = ["Meeting notes 2025-01-08", "Attendees: Ana, Bjorn, Chloe",
             "Action: ship v2 by Q2", "Budget cap 45,000 EUR", "Next sync Thursday 10am"]
    render_page(misc2, f"{outdir}/f18_notes.png", "Trebuchet MS.ttf", 32)
    fx.append({"file": "f18_notes.png", "lines": misc2, "tags": ["notes", "clean"]})

    misc3 = ["Café Ménu du Jour", "Crème brûlée — 8.50", "Naïve salad — 11.00",
             "Jalapeño soup — 7.25", "Total ~ 26.75 incl. tip"]
    render_page(misc3, f"{outdir}/f19_menu.png", "Georgia.ttf", 34)
    fx.append({"file": "f19_menu.png", "lines": misc3, "tags": ["menu", "diacritics", "clean"]})

    misc4 = ["Shipping label 2023", "To: 742 Evergreen Terrace", "Springfield, USA 49007",
             "Weight 3.4 kg  Zone 5", "Tracking 9400 1000 0000 1234"]
    render_page(misc4, f"{outdir}/f20_label.png", "Arial.ttf", 30)
    fx.append({"file": "f20_label.png", "lines": misc4, "tags": ["label", "clean"]})

    misc5 = ["Prescription Rx 55129", "Amoxicillin 500mg", "Take 1 capsule 3x daily",
             "Refills: 2  Dr. Okonkwo", "Filled 2024-02-11"]
    render_page(misc5, f"{outdir}/f21_rx.png", "Verdana.ttf", 30)
    fx.append({"file": "f21_rx.png", "lines": misc5, "tags": ["medical", "clean"]})

    # remove scratch bases from the fixture dir listing (keep files, not in fx)
    return fx


# ------------------------------- scoring ----------------------------------

def norm(s):
    return " ".join(s.split()).strip()


def nospace(s):
    return "".join(s.split())


def line_correct(pred_text, gt_page):
    """Whitespace-insensitive substring: robust to detection splitting a GT
    line into cells and to missing spaces after colons, while still flagging
    genuine character substitutions (e.g. AE for Ae)."""
    t = nospace(pred_text)
    return len(t) > 0 and t in nospace(gt_page)


def ratio(a, b):
    return difflib.SequenceMatcher(None, a, b).ratio()


def page_text(lines):
    return norm(" ".join(lines))


def match_lines(pred, gt):
    """Greedy best-match each predicted line to a GT line. Returns list of
    (pred_text, conf, best_gt, best_ratio, exact)."""
    out = []
    for p in pred:
        best, br = "", 0.0
        for g in gt:
            r = ratio(norm(p["text"]), norm(g))
            if r > br:
                br, best = r, g
        out.append((p["text"], p["conf"], best, br, norm(p["text"]) == norm(best)))
    return out


# ------------------------------ paddle ref --------------------------------

def make_paddle(det_dir, rec_dir, tier="small"):
    from paddleocr import PaddleOCR
    return PaddleOCR(
        text_detection_model_name=f"PP-OCRv6_{tier}_det",
        text_detection_model_dir=det_dir,
        text_recognition_model_name=f"PP-OCRv6_{tier}_rec",
        text_recognition_model_dir=rec_dir,
        use_doc_orientation_classify=False, use_doc_unwarping=False,
        use_textline_orientation=False)


def paddle_read(ocr, path):
    lines = []
    for r in ocr.predict(path):
        for t, s in zip(r["rec_texts"], r["rec_scores"]):
            lines.append({"text": t, "conf": float(s)})
    return lines


# ------------------------- numerical validation ---------------------------

def numeric_validation(pack_dir, src_dir, fixture_path):
    """Hook paddle to capture model in/out on a real fixture; compare to NumPy."""
    import gc
    from paddleocr import PaddleOCR
    cap = {}

    ocr = PaddleOCR(
        text_detection_model_name="PP-OCRv6_small_det", text_detection_model_dir=f"{src_dir}/det",
        text_recognition_model_name="PP-OCRv6_small_rec", text_recognition_model_dir=f"{src_dir}/rec",
        use_doc_orientation_classify=False, use_doc_unwarping=False, use_textline_orientation=False)

    def hook(m, name):
        orig = m.forward
        def wrapped(x):
            inp = np.array(x[0] if isinstance(x, (list, tuple)) else x)
            out = orig(x)
            oarr = np.array(out[0] if isinstance(out, (list, tuple)) else out)
            cap.setdefault(name, []).append((inp, oarr))
            return out
        m.forward = wrapped

    for obj in gc.get_objects():
        cn = type(obj).__name__
        if cn == "PPOCRV6SmallDet" and "det" not in cap:
            hook(obj, "det")
        elif cn == "PPOCRV6SmallRec" and "rec" not in cap:
            hook(obj, "rec")
    ocr.predict(fixture_path)

    from ocr_pack import Pack
    det = ocr_ref.Detector(Pack(f"{pack_dir}/det.bin"))
    rec = ocr_ref.Recognizer(Pack(f"{pack_dir}/rec.bin"))
    res = {}
    din, dout = cap["det"][0]
    my = det.forward(din.astype(np.float32))
    res["det_max_abs_diff"] = float(np.abs(my - dout).max())
    res["det_shape"] = list(dout.shape)
    rin, rout = cap["rec"][0]
    myr = rec.forward(rin.astype(np.float32))
    res["rec_max_abs_diff"] = float(np.abs(myr - rout).max())
    res["rec_argmax_agree"] = float((myr.argmax(-1) == rout.argmax(-1)).mean())
    res["rec_shape"] = list(rout.shape)
    return res


# ----------------------------------- main ---------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--src", required=True, help="pinned small safetensors dir (det/, rec/)")
    ap.add_argument("--medium-src", default=None, help="pinned medium safetensors dir")
    ap.add_argument("--out", required=True, help="regressions output dir")
    ap.add_argument("--fixtures", default=None)
    args = ap.parse_args()

    fixdir = args.fixtures or os.path.join(args.out, "fixtures")
    os.makedirs(args.out, exist_ok=True)
    print("[fixtures] generating ...")
    fx = make_fixtures(fixdir)
    json.dump(fx, open(f"{fixdir}/ground_truth.json", "w"), indent=1, ensure_ascii=False)
    print(f"[fixtures] {len(fx)} pages, {sum(len(f['lines']) for f in fx)} GT lines")

    reader = ocr_ref.Reader(args.pack)
    ocr = make_paddle(f"{args.src}/det", f"{args.src}/rec")

    print("[numeric] validating NumPy forward vs paddle on a fixture ...")
    numeric = numeric_validation(args.pack, args.src, f"{fixdir}/{fx[3]['file']}")
    print("  det max_abs_diff=%.2e  rec max_abs_diff=%.2e  rec argmax=%.4f"
          % (numeric["det_max_abs_diff"], numeric["rec_max_abs_diff"], numeric["rec_argmax_agree"]))

    per_line = []       # (conf, correct_vs_gt) for MY reader, for calibration
    incorrect_examples = []
    page_agree = []     # paddle-vs-mine page text agreement
    line_agree_num = line_agree_den = 0
    clean_agree_num = clean_agree_den = 0   # line agreement, clean fixtures only
    per_fixture = []
    t0 = time.time()
    for f in fx:
        path = f"{fixdir}/{f['file']}"
        img = cv2.imread(path)
        mine = reader.read(img)
        pad = paddle_read(ocr, path)
        # (b) line-for-line agreement between mine and paddle (same weights)
        mt = [norm(x["text"]) for x in mine]
        pt = [norm(x["text"]) for x in pad]
        sm = difflib.SequenceMatcher(None, mt, pt)
        matched_lines = sum(b.size for b in sm.get_matching_blocks())
        line_agree_num += matched_lines
        line_agree_den += max(len(mt), len(pt), 1)
        if not any(t in f["tags"] for t in ("degraded", "hard")):
            clean_agree_num += matched_lines
            clean_agree_den += max(len(mt), len(pt), 1)
        page_agree.append(ratio(page_text([x["text"] for x in mine]),
                                page_text([x["text"] for x in pad])))
        # accuracy vs GT for MY reader -> conf calibration
        gt_page_str = page_text(f["lines"])
        for x in mine:
            ok = line_correct(x["text"], gt_page_str)
            per_line.append((x["conf"], ok))
            if not ok:
                incorrect_examples.append({"file": f["file"], "conf": round(x["conf"], 3),
                                           "read": x["text"], "tags": f["tags"]})
        my_page = page_text([x["text"] for x in mine])
        gt_page = page_text(f["lines"])
        per_fixture.append({"file": f["file"], "tags": f["tags"],
                            "my_lines": len(mine), "gt_lines": len(f["lines"]),
                            "paddle_lines": len(pad),
                            "page_char_acc_vs_gt": round(ratio(my_page, gt_page), 4),
                            "page_agree_vs_paddle": round(page_agree[-1], 4)})
    dt = time.time() - t0

    confs = np.array([c for c, _ in per_line])
    correct = np.array([e for _, e in per_line])
    n_corr, n_inc = int(correct.sum()), int((~correct).sum())

    # (c) calibrate T_ACCEPT and T_DECIDE from the histograms.
    #  T_ACCEPT: escalate below it. Choose the threshold maximising Youden's J
    #  (separates correct vs incorrect best).  T_DECIDE: gate file actions;
    #  choose the lowest conf giving >= 99% precision that a line is correct.
    grid = np.round(np.arange(0.30, 0.999, 0.01), 3)
    best_j, t_accept = -1, 0.80
    for t in grid:
        acc = confs >= t
        tp = int((acc & correct).sum()); fn = int((~acc & correct).sum())
        tn = int((~acc & ~correct).sum()); fp = int((acc & ~correct).sum())
        tpr = tp / max(tp + fn, 1); fpr = fp / max(fp + tn, 1)
        j = tpr - fpr
        if j > best_j:
            best_j, t_accept = j, float(t)
    t_decide = 0.999
    for t in grid[::-1]:
        acc = confs >= t
        prec = int((acc & correct).sum()) / max(int(acc.sum()), 1)
        if prec >= 0.99 and int(acc.sum()) >= 10:
            t_decide = float(t)
            break

    # histogram summary (deciles)
    edges = np.round(np.arange(0.0, 1.01, 0.1), 1)
    hist = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        m = (confs >= lo) & (confs < hi if hi < 1.0 else confs <= hi)
        hist.append({"bin": f"[{lo:.1f},{hi:.1f})", "n": int(m.sum()),
                     "correct": int(correct[m].sum()), "incorrect": int((~correct[m]).sum())})

    calib = {
        "n_lines": len(per_line), "n_correct": n_corr, "n_incorrect": n_inc,
        "t_accept": t_accept, "t_accept_youden_j": round(best_j, 4),
        "t_decide": t_decide,
        "max_conf_of_incorrect": float(confs[~correct].max()) if n_inc else None,
        "min_conf_of_correct": float(confs[correct].min()) if n_corr else None,
        "histogram": hist,
        "incorrect_examples": sorted(incorrect_examples, key=lambda e: e["conf"])[:25],
    }

    # (b) roll-up
    validation_b = {
        "numeric": numeric,
        "line_agreement_vs_paddle": round(line_agree_num / max(line_agree_den, 1), 4),
        "line_agreement_clean_only": round(clean_agree_num / max(clean_agree_den, 1), 4),
        "mean_page_agreement_vs_paddle": round(float(np.mean(page_agree)), 4),
        "seconds_for_all_fixtures": round(dt, 1),
    }

    results = {"fixtures": len(fx), "gt_lines": sum(len(f["lines"]) for f in fx),
               "validation_b": validation_b, "calibration_c": calib,
               "per_fixture": per_fixture}

    # (d) resolution study — run separately below and merge
    results["resolution_d"] = resolution_study(reader, fixdir, fx)

    # (e) tier study
    if args.medium_src:
        results["tier_e"] = tier_study(ocr, args.medium_src, fixdir, fx, args.src)
    else:
        results["tier_e"] = {"status": "skipped: no --medium-src"}

    json.dump(results, open(f"{args.out}/e_r1_results.json", "w"), indent=2, ensure_ascii=False)
    print(f"[done] wrote {args.out}/e_r1_results.json in {dt:.1f}s")
    print(f"  (b) line-agree-vs-paddle={validation_b['line_agreement_vs_paddle']:.3f}  "
          f"page-agree={validation_b['mean_page_agreement_vs_paddle']:.3f}")
    print(f"  (c) T_ACCEPT={t_accept}  T_DECIDE={t_decide}  "
          f"lines={len(per_line)} correct={n_corr} incorrect={n_inc}")
    return results


def resolution_study(reader, fixdir, fx):
    """(d) Render the dense/small fixtures at 768 vs 1536 px long edge; measure
    char accuracy vs GT. The det model caps input long-edge at 960, so this
    mainly probes whether higher render helps the *recognizer* on small text."""
    targets = [f for f in fx if any(t in f["tags"] for t in ("dense", "small", "codes"))]
    rows = []
    for f in targets:
        img = cv2.imread(f"{fixdir}/{f['file']}")
        h, w = img.shape[:2]
        gt = page_text(f["lines"])
        r = {"file": f["file"], "orig_long_edge": max(h, w)}
        for cap in (768, 1536):
            sc = cap / max(h, w)
            im2 = cv2.resize(img, (max(1, int(w * sc)), max(1, int(h * sc))),
                             interpolation=cv2.INTER_CUBIC if sc > 1 else cv2.INTER_AREA)
            lines = reader.read(im2)
            r[f"acc_{cap}"] = round(ratio(page_text([x["text"] for x in lines]), gt), 4)
        rows.append(r)
    mean768 = round(float(np.mean([r["acc_768"] for r in rows])), 4)
    mean1536 = round(float(np.mean([r["acc_1536"] for r in rows])), 4)
    return {"note": "det caps long-edge at 960; higher render mainly feeds rec crops",
            "mean_acc_768": mean768, "mean_acc_1536": mean1536, "rows": rows}


def tier_study(ocr_small, medium_src, fixdir, fx, small_src):
    """(e) small vs medium accuracy on the fixtures (PaddleOCR runs both from the
    pinned, verified safetensors). Promote to medium only if it fixes lines
    small reads wrong."""
    ocr_med = make_paddle(f"{medium_src}/det", f"{medium_src}/rec", tier="medium")
    fixed, regressed, rows = 0, 0, []
    for f in fx:
        path = f"{fixdir}/{f['file']}"
        gt = page_text(f["lines"])
        s = ratio(page_text([x["text"] for x in paddle_read(ocr_small, path)]), gt)
        m = ratio(page_text([x["text"] for x in paddle_read(ocr_med, path)]), gt)
        if m > s + 0.02:
            fixed += 1
        if s > m + 0.02:
            regressed += 1
        rows.append({"file": f["file"], "small_acc": round(s, 4), "medium_acc": round(m, 4)})
    return {"pages_medium_better": fixed, "pages_small_better": regressed,
            "mean_small": round(float(np.mean([r["small_acc"] for r in rows])), 4),
            "mean_medium": round(float(np.mean([r["medium_acc"] for r in rows])), 4),
            "rows": rows}


if __name__ == "__main__":
    main()
