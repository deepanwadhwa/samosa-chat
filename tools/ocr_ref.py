#!/usr/bin/env python3
"""NumPy reference forward pass for the Samosa OCR pack (PP-OCRv6 small).

This is the golden reference the C sidecar (R2/R3) must reproduce within
tolerance. It reads a Samosa flat pack (tools/ocr_pack.py) and reimplements
the PP-OCRv6 small det + rec forward passes exactly as PaddleX defines them:

  det: PP-LCNetV4 backbone -> FPN neck -> DB head (2x transpose-conv) -> sigmoid
  rec: PP-LCNetV4 backbone -> avg_pool(3,2) -> SVTR encoder -> CTC linear -> softmax

Verified numerically against paddle's own model output (see tools/run_e_r1.py).
No paddle/torch/onnx imported here.
"""

import numpy as np
from scipy.special import erf


# ----------------------------- primitives ---------------------------------

def relu(x):
    return np.maximum(x, 0.0)

def gelu(x):  # paddle F.gelu(approximate=False): exact erf
    return x * 0.5 * (1.0 + erf(x / np.sqrt(2.0)))

def silu(x):
    return x * (1.0 / (1.0 + np.exp(-x)))

def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))

def hardsigmoid(x):  # paddle nn.Hardsigmoid default slope=1/6 offset=0.5
    return np.clip(x * (1.0 / 6.0) + 0.5, 0.0, 1.0)

ACT = {"relu": relu, "gelu": gelu, "silu": silu, None: lambda x: x}


def conv2d(x, w, b=None, stride=1, pad=0, groups=1):
    """x: (N,C,H,W), w: (O, C/groups, kh, kw). Returns (N,O,Ho,Wo)."""
    if isinstance(stride, int):
        stride = (stride, stride)
    if isinstance(pad, int):
        pad = (pad, pad)
    N, C, H, W = x.shape
    O, Cg, kh, kw = w.shape
    sh, sw = stride
    ph, pw = pad
    if ph or pw:
        x = np.pad(x, ((0, 0), (0, 0), (ph, ph), (pw, pw)))
    H2, W2 = x.shape[2], x.shape[3]
    Ho = (H2 - kh) // sh + 1
    Wo = (W2 - kw) // sw + 1
    # im2col via as_strided
    s = x.strides
    cols = np.lib.stride_tricks.as_strided(
        x, shape=(N, C, Ho, Wo, kh, kw),
        strides=(s[0], s[1], s[2] * sh, s[3] * sw, s[2], s[3]),
    )
    out = np.empty((N, O, Ho, Wo), dtype=np.float32)
    Og = O // groups
    for g in range(groups):
        c0, c1 = g * Cg, (g + 1) * Cg
        o0, o1 = g * Og, (g + 1) * Og
        col = cols[:, c0:c1].reshape(N, Cg * kh * kw, Ho * Wo)  # via copy below
        col = np.ascontiguousarray(cols[:, c0:c1]).reshape(N, Cg, Ho * Wo, kh * kw)
        col = col.transpose(0, 2, 1, 3).reshape(N, Ho * Wo, Cg * kh * kw)
        wg = w[o0:o1].reshape(Og, Cg * kh * kw)
        res = col @ wg.T  # (N, Ho*Wo, Og)
        out[:, o0:o1] = res.transpose(0, 2, 1).reshape(N, Og, Ho, Wo)
    if b is not None:
        out += b.reshape(1, O, 1, 1)
    return out


def conv_transpose2d(x, w, b=None, stride=1):
    """Paddle Conv2DTranspose, padding=0. w: (Cin, Cout, kh, kw)."""
    if isinstance(stride, int):
        stride = (stride, stride)
    sh, sw = stride
    N, Cin, H, W = x.shape
    Cin2, Cout, kh, kw = w.shape
    Ho = (H - 1) * sh + kh
    Wo = (W - 1) * sw + kw
    out = np.zeros((N, Cout, Ho, Wo), dtype=np.float32)
    # scatter-add: for each input pixel, add weight patch
    for i in range(H):
        for j in range(W):
            oi, oj = i * sh, j * sw
            # x[:, :, i, j]: (N, Cin); w: (Cin, Cout, kh, kw)
            patch = np.tensordot(x[:, :, i, j], w, axes=([1], [0]))  # (N,Cout,kh,kw)
            out[:, :, oi:oi + kh, oj:oj + kw] += patch
    if b is not None:
        out += b.reshape(1, Cout, 1, 1)
    return out


def batchnorm(x, g, beta, mean, var, eps=1e-5):
    scale = g / np.sqrt(var + eps)
    return x * scale.reshape(1, -1, 1, 1) + (beta - mean * scale).reshape(1, -1, 1, 1)


def adaptive_avgpool1(x):
    return x.mean(axis=(2, 3), keepdims=True)

def interp_nearest2x(x):
    return np.repeat(np.repeat(x, 2, axis=2), 2, axis=3)

def maxpool_k2_s1_ceil(x):
    # paddle MaxPool2D(kernel=2, stride=1, ceil_mode=True), no padding.
    # For integer (H-2), ceil==floor => output size H-1, windows all valid.
    N, C, H, W = x.shape
    Ho, Wo = H - 1, W - 1
    s = x.strides
    win = np.lib.stride_tricks.as_strided(
        x, shape=(N, C, Ho, Wo, 2, 2), strides=(s[0], s[1], s[2], s[3], s[2], s[3]))
    return win.max(axis=(4, 5))

def layernorm(x, g, b, eps):
    m = x.mean(-1, keepdims=True)
    v = x.var(-1, keepdims=True)
    return (x - m) / np.sqrt(v + eps) * g + b

def softmax(x, axis=-1):
    x = x - x.max(axis=axis, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis=axis, keepdims=True)


# --------------------------- PP-LCNetV4 backbone --------------------------

class Backbone:
    def __init__(self, pack, prefix, cfg):
        self.p = pack
        self.pre = prefix  # e.g. "model.backbone.encoder"
        self.cfg = cfg
        self.hidden_act = cfg.get("hidden_act", "relu")
        self.reduction = cfg.get("reduction", 4)
        self.stem_type = cfg.get("stem_type", "large")

    def g(self, name):
        return self.p.get(self.pre + "." + name)

    def has(self, name):
        return (self.pre + "." + name) in self.p

    def conv_bn(self, x, name, act, stride=1, groups=1):
        w = self.g(name + ".convolution.weight")
        kh, kw = w.shape[2], w.shape[3]
        x = conv2d(x, w, None, stride=stride, pad=((kh - 1) // 2, (kw - 1) // 2), groups=groups)
        x = batchnorm(x, self.g(name + ".normalization.weight"),
                      self.g(name + ".normalization.bias"),
                      self.g(name + ".normalization.running_mean"),
                      self.g(name + ".normalization.running_var"))
        return ACT[act](x) if act else x

    def se(self, x, name):
        # PPLCNetV4SqueezeExcitationModule: reduce(relu) -> expand(hardsigmoid)
        r = x
        y = adaptive_avgpool1(x)
        y = conv2d(y, self.g(name + ".convolutions.0.weight"), self.g(name + ".convolutions.0.bias"))
        y = relu(y)
        y = conv2d(y, self.g(name + ".convolutions.2.weight"), self.g(name + ".convolutions.2.bias"))
        y = hardsigmoid(y)
        return r * y

    def large_stem(self, x):
        c = self.cfg["stem_channels"]
        st = self.cfg.get("stem_strides", [2, 1, 1, 2, 1])
        act = self.hidden_act
        s = "convolution"
        x1 = self.conv_bn(x, s + ".stem1", act, stride=st[0])
        x1 = np.pad(x1, ((0, 0), (0, 0), (0, 1), (0, 1)))
        x2 = self.conv_bn(x1, s + ".stem2a", act, stride=st[1])
        x2 = np.pad(x2, ((0, 0), (0, 0), (0, 1), (0, 1)))
        x2 = self.conv_bn(x2, s + ".stem2b", act, stride=st[2])
        pooled = maxpool_k2_s1_ceil(x1)
        x = np.concatenate([pooled, x2], axis=1)
        x = self.conv_bn(x, s + ".stem3", act, stride=st[3])
        x = self.conv_bn(x, s + ".stem4", act, stride=st[4])
        return x

    def ds_block(self, x, bname, in_ch, out_ch, ksz, stride, use_se):
        rep_dw = (stride == 1 or stride == [1, 1]) and in_ch == out_ch
        st = stride if isinstance(stride, (list, tuple)) else (stride, stride)
        if rep_dw:
            w = self.g(bname + ".token_conv.weight")
            x = conv2d(x, w, self.g(bname + ".token_conv.bias"),
                       stride=1, pad=(w.shape[2] // 2, w.shape[3] // 2), groups=in_ch)
        else:
            x = self.conv_bn(x, bname + ".token_conv", None, stride=tuple(st), groups=in_ch)
        if use_se:
            x = self.se(x, bname + ".token_squeeze_excitation")
        residual = x
        x = self.conv_bn(x, bname + ".channel_conv1", None)
        x = gelu(x)
        x = self.conv_bn(x, bname + ".channel_conv2", None)
        if in_ch == out_ch and (stride == 1 or stride == [1, 1]):
            x = residual + x
        return x

    def forward(self, x):
        if self.stem_type == "large":
            x = self.large_stem(x)
        else:
            raise NotImplementedError("small stem not needed for v6 small")
        outs = []
        for si, blocks in enumerate(self.cfg["block_configs"]):
            for bi, bc in enumerate(blocks):
                ksz, in_ch, out_ch, stride, use_se = bc
                x = self.ds_block(x, f"blocks.{si}.blocks.{bi}", in_ch, out_ch, ksz, stride, use_se)
            outs.append(x)
        return outs


# ------------------------------ detector ----------------------------------

class Detector:
    def __init__(self, pack):
        self.p = pack
        self.cfg = pack.meta["arch"]
        self.backbone = Backbone(pack, "model.backbone.encoder", self.cfg["backbone_config"])

    def g(self, n):
        return self.p.get(n)

    def se_neck(self, x, name):
        # PPOCRV6SmallDetSqueezeExcitationModule: conv1 relu conv2 clip(0.2x+0.5)
        r = x
        y = adaptive_avgpool1(x)
        y = relu(conv2d(y, self.g(name + ".conv1.weight"), self.g(name + ".conv1.bias")))
        y = conv2d(y, self.g(name + ".conv2.weight"), self.g(name + ".conv2.bias"))
        y = np.clip(0.2 * y + 0.5, 0.0, 1.0)
        return r * y

    def neck(self, feats):
        cfg = self.cfg
        n = len(cfg["layer_list_out_channels"])
        fused = []
        for i in range(n):
            # ResidualSqueezeExcitationLayer: in_conv(1x1) then + SE
            pre = f"model.neck.insert_conv.{i}"
            h = conv2d(feats[i], self.g(pre + ".in_conv.weight"), None, pad=0)
            h = h + self.se_neck(h, pre + ".squeeze_excitation_block")
            fused.append(h)
        for i in range(2, -1, -1):
            fused[i] = fused[i] + interp_nearest2x(fused[i + 1])
        fed = []
        for i in range(n):
            pre = f"model.neck.input_conv.{i}"
            h = fused[i]
            # DepthwiseSeparableConvLayer: dwconv(k=dilated) -> pwconv(1x1, out/4) -> + SE(out/4)
            k = cfg["dilated_kernel_size"]
            dw = conv2d(h, self.g(pre + ".depthwise_convolution.weight"),
                        self.g(pre + ".depthwise_convolution.bias"),
                        pad=k // 2, groups=h.shape[1])
            pw = conv2d(dw, self.g(pre + ".pointwise_convolution.weight"), None)
            pw = pw + self.se_neck(pw, pre + ".squeeze_excitation_module")
            fed.append(pw)
        processed = []
        for feat, scale in zip(fed, [1, 2, 4, 8]):
            if scale != 1:
                r = feat
                for _ in range(int(np.log2(scale))):
                    r = interp_nearest2x(r)
                processed.append(r)
            else:
                processed.append(feat)
        return np.concatenate(processed[::-1], axis=1)

    def head(self, x):
        # conv_down (ConvBatchnormLayer relu), conv_up (transpose+BN+relu), conv_final (transpose) sigmoid
        w = self.g("head.conv_down.convolution.weight")
        x = conv2d(x, w, None, pad=w.shape[2] // 2)
        x = batchnorm(x, self.g("head.conv_down.norm.weight"), self.g("head.conv_down.norm.bias"),
                      self.g("head.conv_down.norm.running_mean"), self.g("head.conv_down.norm.running_var"))
        x = relu(x)
        x = conv_transpose2d(x, self.g("head.conv_up.convolution.weight"),
                             self.g("head.conv_up.convolution.bias"), stride=2)
        x = batchnorm(x, self.g("head.conv_up.norm.weight"), self.g("head.conv_up.norm.bias"),
                      self.g("head.conv_up.norm.running_mean"), self.g("head.conv_up.norm.running_var"))
        x = relu(x)
        x = conv_transpose2d(x, self.g("head.conv_final.weight"), self.g("head.conv_final.bias"), stride=2)
        return sigmoid(x)

    def forward(self, x):
        feats = self.backbone.forward(x)
        neck = self.neck(feats)
        return self.head(neck)


# ----------------------------- recognizer ---------------------------------

class Recognizer:
    def __init__(self, pack):
        self.p = pack
        self.cfg = pack.meta["arch"]
        self.backbone = Backbone(pack, "model.backbone.encoder", self.cfg["backbone_config"])
        self.eps = 1e-6
        self.nheads = 8

    def g(self, n):
        return self.p.get(n)

    def conv_bn_act(self, x, name, k, groups, act="silu"):
        w = self.g(name + ".convolution.weight")
        x = conv2d(x, w, None, pad=(w.shape[2] // 2, w.shape[3] // 2), groups=groups)
        x = batchnorm(x, self.g(name + ".normalization.weight"), self.g(name + ".normalization.bias"),
                      self.g(name + ".normalization.running_mean"), self.g(name + ".normalization.running_var"))
        return ACT[act](x)

    def attn(self, x, pre):
        B, T, D = x.shape
        qkv = x @ self.g(pre + ".qkv.weight").T + self.g(pre + ".qkv.bias")
        qkv = qkv.reshape(B, T, 3, self.nheads, D // self.nheads).transpose(2, 0, 3, 1, 4)
        q, k, v = qkv[0], qkv[1], qkv[2]
        scale = (D // self.nheads) ** -0.5
        aw = softmax((q @ k.transpose(0, 1, 3, 2)) * scale, axis=-1)
        o = (aw @ v).transpose(0, 2, 1, 3).reshape(B, T, D)
        return o @ self.g(pre + ".projection.weight").T + self.g(pre + ".projection.bias")

    def svtr_block(self, x, pre):
        h = layernorm(x, self.g(pre + ".layer_norm1.weight"), self.g(pre + ".layer_norm1.bias"), self.eps)
        x = x + self.attn(h, pre + ".self_attn")
        h = layernorm(x, self.g(pre + ".layer_norm2.weight"), self.g(pre + ".layer_norm2.bias"), self.eps)
        h = h @ self.g(pre + ".mlp.fc1.weight").T + self.g(pre + ".mlp.fc1.bias")
        h = silu(h)
        h = h @ self.g(pre + ".mlp.fc2.weight").T + self.g(pre + ".mlp.fc2.bias")
        return x + h

    def head(self, x):
        # x: backbone last stage output -> avg_pool2d(3,2)
        N, C, H, W = x.shape
        # avg_pool2d kernel (3,2) stride (3,2) no pad
        Ho, Wo = H // 3, W // 2
        s = x.strides
        win = np.lib.stride_tricks.as_strided(
            x, shape=(N, C, Ho, Wo, 3, 2),
            strides=(s[0], s[1], s[2] * 3, s[3] * 2, s[2], s[3]))
        x = win.mean(axis=(4, 5)).astype(np.float32)
        # encoder
        residual = self.conv_bn_act(x, "head.encoder.conv_block.0", (1, 1), 1)
        h = self.conv_bn_act(x, "head.encoder.conv_block.1", (1, 1), 1)
        hs = self.conv_bn_act(h, "head.encoder.conv_block.2", (1, 7), h.shape[1])
        h = h + hs
        B, Cc, Hh, Ww = h.shape
        seq = h.reshape(B, Cc, Hh * Ww).transpose(0, 2, 1)
        for i in range(self.cfg["depth"]):
            seq = self.svtr_block(seq, f"head.encoder.svtr_block.{i}")
        seq = layernorm(seq, self.g("head.encoder.norm.weight"), self.g("head.encoder.norm.bias"), self.eps)
        seq = seq.reshape(B, Hh, Ww, Cc).transpose(0, 3, 1, 2)
        seq = seq + residual
        seq = seq.squeeze(2).transpose(0, 2, 1)  # (B, W, C)
        logits = seq @ self.g("head.head.weight").T + self.g("head.head.bias")
        return softmax(logits, axis=2)

    def forward(self, x):
        feats = self.backbone.forward(x)
        return self.head(feats[-1])


# --------------------------- pipeline glue --------------------------------
# cv2 + pyclipper are used here to match paddle's DB postprocess and crop
# byte-for-byte in the E-R1 reference environment. The C sidecar (R2/R3)
# reimplements these deterministically; this module defines the target.

import cv2
import pyclipper


def preprocess_det(img_bgr, meta):
    """Replicates DetResizeForTest(limit_type=max, 960) + NormalizeImage."""
    pre = meta["preprocess"]
    h, w = img_bgr.shape[:2]
    L = pre["limit_side_len"]
    ratio = (L / max(h, w)) if max(h, w) > L else 1.0
    rh, rw = int(h * ratio), int(w * ratio)
    m = pre["size_multiple"]
    rh = max(int(round(rh / m) * m), m)
    rw = max(int(round(rw / m) * m), m)
    if (rh, rw) != (h, w):
        img = cv2.resize(img_bgr, (rw, rh))
    else:
        img = img_bgr
    x = img.astype(np.float32)
    mean = np.array(pre["mean"], np.float32)
    std = np.array(pre["std"], np.float32)
    x = (x * pre["scale"] - mean) / std          # per channel (BGR read order)
    x = x.transpose(2, 0, 1)[None]               # NCHW
    ratio_h, ratio_w = rh / float(h), rw / float(w)
    return np.ascontiguousarray(x, np.float32), (h, w, ratio_h, ratio_w)


def _mini_boxes(contour):
    box = cv2.minAreaRect(contour)
    pts = sorted(list(cv2.boxPoints(box)), key=lambda p: p[0])
    i1, i2, i3, i4 = 0, 1, 2, 3
    if pts[1][1] > pts[0][1]:
        i1, i4 = 0, 1
    else:
        i1, i4 = 1, 0
    if pts[3][1] > pts[2][1]:
        i2, i3 = 2, 3
    else:
        i2, i3 = 3, 2
    return [pts[i1], pts[i2], pts[i3], pts[i4]], min(box[1])


def _box_score_fast(bitmap, box):
    h, w = bitmap.shape[:2]
    b = box.copy()
    import math
    xmin = max(0, min(math.floor(b[:, 0].min()), w - 1))
    xmax = max(0, min(math.ceil(b[:, 0].max()), w - 1))
    ymin = max(0, min(math.floor(b[:, 1].min()), h - 1))
    ymax = max(0, min(math.ceil(b[:, 1].max()), h - 1))
    mask = np.zeros((ymax - ymin + 1, xmax - xmin + 1), np.uint8)
    b[:, 0] -= xmin
    b[:, 1] -= ymin
    cv2.fillPoly(mask, b.reshape(1, -1, 2).astype(np.int32), 1)
    return cv2.mean(bitmap[ymin:ymax + 1, xmin:xmax + 1], mask)[0]


def _unclip(box, unclip_ratio):
    area = cv2.contourArea(box)
    length = cv2.arcLength(box, True)
    distance = area * unclip_ratio / length
    off = pyclipper.PyclipperOffset()
    off.AddPath(box, pyclipper.JT_ROUND, pyclipper.ET_CLOSEDPOLYGON)
    return np.array(off.Execute(distance))


def db_postprocess(prob, shape, post):
    """prob: (1,1,H,W). Returns list of (box 4x2 int, score). Mirrors DBPostProcess quad."""
    pred = prob[0, 0]
    src_h, src_w, ratio_h, ratio_w = shape
    thresh = post.get("thresh", 0.3)
    box_thresh = post.get("box_thresh", 0.6)
    unclip_ratio = post.get("unclip_ratio", 2.0)
    max_candidates = post.get("max_candidates", 1000)
    min_size = post.get("min_size", 3)
    bitmap = (pred > thresh).astype(np.uint8)
    H, W = bitmap.shape
    ws, hs = src_w / W, src_h / H
    contours, _ = cv2.findContours(bitmap * 255, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    out = []
    for c in contours[:max_candidates]:
        pts, sside = _mini_boxes(c)
        if sside < min_size:
            continue
        pts = np.array(pts)
        score = _box_score_fast(pred, pts.reshape(-1, 2))
        if box_thresh > score:
            continue
        box = _unclip(pts, unclip_ratio).reshape(-1, 1, 2)
        box, sside = _mini_boxes(box)
        if sside < min_size + 2:
            continue
        box = np.array(box)
        box[:, 0] = np.clip(np.round(box[:, 0] * ws), 0, src_w)
        box[:, 1] = np.clip(np.round(box[:, 1] * hs), 0, src_h)
        out.append((box.astype(np.int16), float(score)))
    return out


def sort_quad_boxes(boxes):
    if not boxes:
        return boxes
    b = sorted(boxes, key=lambda x: (x[0][0][1], x[0][0][0]))
    for i in range(len(b) - 1):
        for j in range(i, -1, -1):
            if abs(b[j + 1][0][0][1] - b[j][0][0][1]) < 10 and b[j + 1][0][0][0] < b[j][0][0][0]:
                b[j], b[j + 1] = b[j + 1], b[j]
            else:
                break
    return b


def get_rotate_crop(img, points):
    points = points.astype(np.float32)
    wA = np.linalg.norm(points[0] - points[1])
    wB = np.linalg.norm(points[2] - points[3])
    hA = np.linalg.norm(points[0] - points[3])
    hB = np.linalg.norm(points[1] - points[2])
    cw = int(max(wA, wB))
    ch = int(max(hA, hB))
    if cw <= 0 or ch <= 0:
        return None
    std = np.float32([[0, 0], [cw, 0], [cw, ch], [0, ch]])
    M = cv2.getPerspectiveTransform(points, std)
    dst = cv2.warpPerspective(img, M, (cw, ch), borderMode=cv2.BORDER_REPLICATE, flags=cv2.INTER_CUBIC)
    if dst.shape[0] * 1.0 / dst.shape[1] >= 1.5:
        dst = np.rot90(dst)
    return dst


def minarea_crop(img, box):
    rect = cv2.minAreaRect(np.array(box).astype(np.int32))
    pts = sorted(list(cv2.boxPoints(rect)), key=lambda p: p[0])
    i1, i2, i3, i4 = 0, 1, 2, 3
    if pts[1][1] > pts[0][1]:
        i1, i4 = 0, 1
    else:
        i1, i4 = 1, 0
    if pts[3][1] > pts[2][1]:
        i2, i3 = 2, 3
    else:
        i2, i3 = 3, 2
    ordered = np.array([pts[i1], pts[i2], pts[i3], pts[i4]])
    return get_rotate_crop(img, ordered)


def preprocess_rec(crop_bgr, max_wh_ratio, img_h=48, max_img_w=3200):
    import math
    h, w = crop_bgr.shape[:2]
    imgW = int(img_h * max_wh_ratio)
    if imgW > max_img_w:
        resized = cv2.resize(crop_bgr, (max_img_w, img_h))
        resized_w, imgW = max_img_w, max_img_w
    else:
        ratio = w / float(h)
        resized_w = imgW if math.ceil(img_h * ratio) > imgW else int(math.ceil(img_h * ratio))
        resized = cv2.resize(crop_bgr, (resized_w, img_h))
    r = resized.astype(np.float32).transpose(2, 0, 1) / 255.0
    r = (r - 0.5) / 0.5
    pad = np.zeros((3, img_h, imgW), np.float32)
    pad[:, :, :resized_w] = r
    return pad


def ctc_decode(prob, charset):
    """prob: (T, C) softmax. Returns (text, mean_conf). Blank=index 0, dedup."""
    idx = prob.argmax(-1)
    conf = prob.max(-1)
    sel = np.ones(len(idx), bool)
    sel[1:] = idx[1:] != idx[:-1]
    sel &= idx != 0
    chars = [charset[i] for i in idx[sel]]
    confs = conf[sel]
    text = "".join(chars)
    mc = float(np.mean(confs)) if len(confs) else 0.0
    return text, mc


class Reader:
    """Full tier-0/1 read pipeline matching PaddleOCR PP-OCRv6 small."""

    def __init__(self, pack_dir):
        import os
        from ocr_pack import Pack
        self.det = Detector(Pack(os.path.join(pack_dir, "det.bin")))
        self.rec = Recognizer(Pack(os.path.join(pack_dir, "rec.bin")))
        self.det_meta = self.det.p.meta
        self.rec_meta = self.rec.p.meta
        self.charset = [l.rstrip("\n").replace("\\n", "\n")
                        for l in open(os.path.join(pack_dir, "charset.txt"), encoding="utf-8")]

    def detect(self, img_bgr):
        x, shape = preprocess_det(img_bgr, self.det_meta)
        prob = self.det.forward(x)
        boxes = db_postprocess(prob, shape, self.det_meta["postprocess"])
        return sort_quad_boxes(boxes), prob, shape

    def recognize_crops(self, crops):
        """Batch crops through rec exactly like paddle (shared max_wh_ratio)."""
        if not crops:
            return []
        base = self.rec_meta["preprocess"]["max_wh_ratio"]
        mwr = base
        for c in crops:
            mwr = max(mwr, c.shape[1] / float(c.shape[0]))
        tensors = [preprocess_rec(c, mwr) for c in crops]
        maxw = max(t.shape[2] for t in tensors)
        batch = np.stack([np.pad(t, ((0, 0), (0, 0), (0, maxw - t.shape[2]))) for t in tensors])
        prob = self.rec.forward(batch.astype(np.float32))
        return [ctc_decode(prob[i], self.charset) for i in range(len(crops))]

    def read(self, img_bgr):
        boxes, prob, shape = self.detect(img_bgr)
        crops = []
        kept = []
        for box, score in boxes:
            crop = minarea_crop(img_bgr, box)
            if crop is None or crop.size == 0:
                continue
            crops.append(crop)
            kept.append((box, score))
        recs = self.recognize_crops(crops)
        lines = []
        for (box, dscore), (text, conf) in zip(kept, recs):
            lines.append({"box": box.tolist(), "text": text, "conf": conf, "det_score": dscore})
        return lines
