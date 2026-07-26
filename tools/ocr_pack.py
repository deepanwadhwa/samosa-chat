#!/usr/bin/env python3
"""Samosa OCR flat-pack format (reader-v0).

A Samosa-owned, dependency-free weight container for the `samosa-ocr` sidecar.
The export tool (tools/export_ocr_pack.py) transforms the pinned upstream
safetensors into this format offline; the C sidecar mmaps it at runtime. No
Paddle / PyTorch / ONNX ever touches a shipped machine (TASKS_READER.md
decision 3).

Binary layout (little-endian throughout):

    0   : magic       8 bytes  b"SMSAOCR\\0"
    8   : version     uint32   = 1
    12  : header_len  uint32   length of the JSON header in bytes
    16  : header      header_len bytes, UTF-8 JSON, then NUL padding so the
                      data section starts 32-byte aligned
    ... : data        tensors concatenated, each C-contiguous float32,
                      at `offset` (relative to data-section start)

Header JSON:
    { "meta": { ... free-form pack metadata ... },
      "tensors": { name: {"shape": [...], "offset": int, "nbytes": int} } }

Every tensor is float32. Keeping one dtype makes the C reader trivial; the
packs are small (small tier det+rec ~31 MB) so there is no size motive to
quantize here. The C side reads this with src/json.h + mmap; see
tools/ocr_pack.h for the mirror reader.
"""

import json
import struct

MAGIC = b"SMSAOCR\x00"
VERSION = 1
ALIGN = 32


def write_pack(path, tensors, meta):
    """tensors: dict name -> numpy float32 array (C-contiguous). meta: dict."""
    import numpy as np

    names = list(tensors)
    index = {}
    data_chunks = []
    offset = 0
    for name in names:
        arr = np.ascontiguousarray(tensors[name], dtype="<f4")
        b = arr.tobytes()
        index[name] = {"shape": list(arr.shape), "offset": offset, "nbytes": len(b)}
        data_chunks.append(b)
        offset += len(b)
        # 32-byte align every tensor start so the C side can vectorise freely
        pad = (-offset) % ALIGN
        if pad:
            data_chunks.append(b"\x00" * pad)
            offset += pad

    header = json.dumps(
        {"meta": meta, "tensors": index}, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    prefix_len = len(MAGIC) + 4 + 4 + len(header)
    header_pad = (-prefix_len) % ALIGN
    header = header + b"\x00" * header_pad

    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", VERSION))
        f.write(struct.pack("<I", len(header)))
        f.write(header)
        for c in data_chunks:
            f.write(c)


class Pack:
    """Read-only view over a Samosa OCR pack; lazy float32 numpy tensors."""

    def __init__(self, path):
        import numpy as np

        self._np = np
        with open(path, "rb") as f:
            self.raw = f.read()
        if self.raw[:8] != MAGIC:
            raise ValueError(f"{path}: bad magic")
        (self.version,) = struct.unpack_from("<I", self.raw, 8)
        (hlen,) = struct.unpack_from("<I", self.raw, 12)
        header = json.loads(self.raw[16 : 16 + hlen].split(b"\x00", 1)[0])
        self.meta = header["meta"]
        self.index = header["tensors"]
        self.data_off = 16 + hlen

    def __contains__(self, name):
        return name in self.index

    def keys(self):
        return self.index.keys()

    def get(self, name):
        e = self.index[name]
        start = self.data_off + e["offset"]
        buf = self.raw[start : start + e["nbytes"]]
        return self._np.frombuffer(buf, dtype="<f4").reshape(e["shape"]).copy()
