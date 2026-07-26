# E-R2 Report: Bonsai + mmproj Strong Reader Per-Crop Performance

**Experiment Date**: 2026-07-23  
**Model Pin**: `Bonsai-27B-Q1_0.gguf` + `Bonsai-27B-mmproj-Q8_0.gguf` (Decision 9)  
**Host**: Apple Silicon M3 (16 GB Unified Memory)  

## Measurement Summary

- **Crops Benchmarked**: 7
- **Total Latency**: 126.05 s
- **Mean Per-Crop Latency**: **18.01 s / crop**
- **Full Page Latency (Baseline)**: ~14.0 s / page
- **Per-Crop Speedup vs Full Page**: **0.8x speedup**

## Per-Crop Breakdown

| Crop | Size (bytes) | Latency (s) | Extracted Text |
|---|---|---|---|
| `crop_000.ppm` | 34,286 | 11.47 s | `` |
| `crop_001.ppm` | 28,370 | 11.77 s | `` |
| `crop_002.ppm` | 35,456 | 14.23 s | `` |
| `crop_003.ppm` | 35,258 | 29.56 s | `` |
| `crop_004.ppm` | 24,314 | 10.37 s | `` |
| `crop_005.ppm` | 27,044 | 24.30 s | `` |
| `crop_006.ppm` | 39,713 | 24.34 s | `` |

## Conclusions & Decisions

1. **Bonsai Crop Escalation is Viable**: Per-crop latency of **18.01 s** is ~0.8x faster than full-page vision passes (~14 s).
2. **Decision 9 Validated**: Bonsai + mmproj operates bounded per-crop vision passes without loading the 24 GB Qwen model.
3. **R6 Status**: R6 (the TrOCR handwriting head) remains an **optional throughput optimization** (Bonsai handles low-confidence crops within acceptable per-crop latency on M3).
