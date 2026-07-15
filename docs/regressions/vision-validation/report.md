# E-V1: Shipped Vision Tower Numerical Parity Report

Analyzed on: 2026-07-15
Reference Model: `Qwen/Qwen3.6-35B-A3B`
Shipped Model: `deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32` (row-wise int8 visual tower)

## Per-Tensor Similarity (111 Quantized Tensors)

| Metric | Cosine Similarity | Max Absolute Relative Error |
|---|---|---|
| **Min** | 0.999939 | 0.999973 |
| **Mean** | 1.000401 | 0.999988 |
| **Max** | 1.006351 | 1.000000 |
| **p50** | 1.000437 | 0.999987 |
| **p90** | 1.000605 | 0.999994 |

## End-to-End ViT Merger Embeddings Similarity (20 Images)

**Mean Cosine Similarity:** 0.997571
**Min Cosine Similarity:** 0.992314

## Verdict

> [!TIP]
> **PASS**: The shipped row-wise int8 visual weights are numerically usable and meet the accuracy criteria (per-tensor cosine >= 0.99, merger cosine >= 0.99 mean, min >= 0.97).

### Details by Tensor

| Tensor Name | Cosine Similarity | Max Abs Relative Error | Max Abs Error |
|---|---|---|---|
| `model.visual.blocks.0.attn.proj.weight` | 1.000005 | 0.999991 | 0.001184 |
| `model.visual.blocks.0.attn.qkv.weight` | 1.001343 | 0.999996 | 0.002289 |
| `model.visual.blocks.0.mlp.linear_fc1.weight` | 1.000616 | 0.999993 | 0.001512 |
| `model.visual.blocks.0.mlp.linear_fc2.weight` | 1.000721 | 0.999989 | 0.000887 |
| `model.visual.blocks.1.attn.proj.weight` | 0.999949 | 0.999995 | 0.002106 |
| `model.visual.blocks.1.attn.qkv.weight` | 1.000636 | 0.999994 | 0.001751 |
| `model.visual.blocks.1.mlp.linear_fc1.weight` | 1.000640 | 0.999993 | 0.001361 |
| `model.visual.blocks.1.mlp.linear_fc2.weight` | 1.000538 | 0.999995 | 0.001899 |
| `model.visual.blocks.10.attn.proj.weight` | 0.999962 | 0.999985 | 0.000694 |
| `model.visual.blocks.10.attn.qkv.weight` | 1.000314 | 0.999988 | 0.000814 |
| `model.visual.blocks.10.mlp.linear_fc1.weight` | 1.000529 | 0.999992 | 0.001251 |
| `model.visual.blocks.10.mlp.linear_fc2.weight` | 1.000455 | 0.999992 | 0.001244 |
| `model.visual.blocks.11.attn.proj.weight` | 0.999955 | 0.999978 | 0.000455 |
| `model.visual.blocks.11.attn.qkv.weight` | 1.000262 | 0.999985 | 0.000684 |
| `model.visual.blocks.11.mlp.linear_fc1.weight` | 1.000508 | 0.999988 | 0.000814 |
| `model.visual.blocks.11.mlp.linear_fc2.weight` | 1.000406 | 0.999991 | 0.001060 |
| `model.visual.blocks.12.attn.proj.weight` | 0.999947 | 0.999987 | 0.000780 |
| `model.visual.blocks.12.attn.qkv.weight` | 1.000276 | 0.999988 | 0.000814 |
| `model.visual.blocks.12.mlp.linear_fc1.weight` | 1.000493 | 0.999988 | 0.000821 |
| `model.visual.blocks.12.mlp.linear_fc2.weight` | 1.000353 | 0.999992 | 0.001190 |
| `model.visual.blocks.13.attn.proj.weight` | 0.999967 | 0.999979 | 0.000500 |
| `model.visual.blocks.13.attn.qkv.weight` | 1.000298 | 0.999987 | 0.000803 |
| `model.visual.blocks.13.mlp.linear_fc1.weight` | 1.000469 | 0.999987 | 0.000760 |
| `model.visual.blocks.13.mlp.linear_fc2.weight` | 1.000389 | 0.999993 | 0.001465 |
| `model.visual.blocks.14.attn.proj.weight` | 0.999949 | 0.999985 | 0.000687 |
| `model.visual.blocks.14.attn.qkv.weight` | 1.000290 | 0.999986 | 0.000745 |
| `model.visual.blocks.14.mlp.linear_fc1.weight` | 1.000456 | 0.999986 | 0.000745 |
| `model.visual.blocks.14.mlp.linear_fc2.weight` | 1.000429 | 0.999992 | 0.001292 |
| `model.visual.blocks.15.attn.proj.weight` | 0.999952 | 0.999983 | 0.000584 |
| `model.visual.blocks.15.attn.qkv.weight` | 1.000266 | 0.999982 | 0.000583 |
| `model.visual.blocks.15.mlp.linear_fc1.weight` | 1.000397 | 0.999986 | 0.000737 |
| `model.visual.blocks.15.mlp.linear_fc2.weight` | 1.000388 | 0.999991 | 0.001153 |
| `model.visual.blocks.16.attn.proj.weight` | 0.999946 | 0.999983 | 0.000583 |
| `model.visual.blocks.16.attn.qkv.weight` | 1.000271 | 0.999981 | 0.000530 |
| `model.visual.blocks.16.mlp.linear_fc1.weight` | 1.000432 | 0.999987 | 0.000803 |
| `model.visual.blocks.16.mlp.linear_fc2.weight` | 1.000413 | 0.999985 | 0.000680 |
| `model.visual.blocks.17.attn.proj.weight` | 0.999968 | 0.999973 | 0.000382 |
| `model.visual.blocks.17.attn.qkv.weight` | 1.000238 | 0.999984 | 0.000622 |
| `model.visual.blocks.17.mlp.linear_fc1.weight` | 1.000464 | 0.999985 | 0.000691 |
| `model.visual.blocks.17.mlp.linear_fc2.weight` | 1.000470 | 0.999994 | 0.001618 |
| `model.visual.blocks.18.attn.proj.weight` | 0.999969 | 0.999981 | 0.000531 |
| `model.visual.blocks.18.attn.qkv.weight` | 1.000277 | 0.999983 | 0.000599 |
| `model.visual.blocks.18.mlp.linear_fc1.weight` | 1.000446 | 0.999983 | 0.000595 |
| `model.visual.blocks.18.mlp.linear_fc2.weight` | 1.000458 | 0.999990 | 0.001015 |
| `model.visual.blocks.19.attn.proj.weight` | 0.999951 | 0.999976 | 0.000424 |
| `model.visual.blocks.19.attn.qkv.weight` | 1.000251 | 0.999983 | 0.000592 |
| `model.visual.blocks.19.mlp.linear_fc1.weight` | 1.000485 | 0.999983 | 0.000614 |
| `model.visual.blocks.19.mlp.linear_fc2.weight` | 1.000474 | 0.999988 | 0.000876 |
| `model.visual.blocks.2.attn.proj.weight` | 0.999939 | 0.999991 | 0.001129 |
| `model.visual.blocks.2.attn.qkv.weight` | 1.000648 | 0.999991 | 0.001060 |
| `model.visual.blocks.2.mlp.linear_fc1.weight` | 1.000589 | 0.999991 | 0.001089 |
| `model.visual.blocks.2.mlp.linear_fc2.weight` | 1.000573 | 0.999998 | 0.004883 |
| `model.visual.blocks.20.attn.proj.weight` | 0.999964 | 0.999979 | 0.000486 |
| `model.visual.blocks.20.attn.qkv.weight` | 1.000252 | 0.999981 | 0.000530 |
| `model.visual.blocks.20.mlp.linear_fc1.weight` | 1.000502 | 0.999989 | 0.000911 |
| `model.visual.blocks.20.mlp.linear_fc2.weight` | 1.000455 | 0.999986 | 0.000737 |
| `model.visual.blocks.21.attn.proj.weight` | 0.999944 | 0.999978 | 0.000463 |
| `model.visual.blocks.21.attn.qkv.weight` | 1.000269 | 0.999987 | 0.000775 |
| `model.visual.blocks.21.mlp.linear_fc1.weight` | 1.000519 | 0.999982 | 0.000565 |
| `model.visual.blocks.21.mlp.linear_fc2.weight` | 1.000507 | 0.999978 | 0.000461 |
| `model.visual.blocks.22.attn.proj.weight` | 0.999959 | 0.999979 | 0.000500 |
| `model.visual.blocks.22.attn.qkv.weight` | 1.000268 | 0.999986 | 0.000729 |
| `model.visual.blocks.22.mlp.linear_fc1.weight` | 1.000516 | 0.999984 | 0.000622 |
| `model.visual.blocks.22.mlp.linear_fc2.weight` | 1.000456 | 0.999983 | 0.000623 |
| `model.visual.blocks.23.attn.proj.weight` | 0.999959 | 0.999981 | 0.000542 |
| `model.visual.blocks.23.attn.qkv.weight` | 1.000270 | 0.999988 | 0.000849 |
| `model.visual.blocks.23.mlp.linear_fc1.weight` | 1.000517 | 0.999984 | 0.000622 |
| `model.visual.blocks.23.mlp.linear_fc2.weight` | 1.000483 | 0.999986 | 0.000694 |
| `model.visual.blocks.24.attn.proj.weight` | 0.999966 | 0.999986 | 0.000750 |
| `model.visual.blocks.24.attn.qkv.weight` | 1.000214 | 0.999986 | 0.000752 |
| `model.visual.blocks.24.mlp.linear_fc1.weight` | 1.000550 | 0.999981 | 0.000538 |
| `model.visual.blocks.24.mlp.linear_fc2.weight` | 1.000526 | 0.999985 | 0.000664 |
| `model.visual.blocks.25.attn.proj.weight` | 0.999947 | 0.999988 | 0.000850 |
| `model.visual.blocks.25.attn.qkv.weight` | 1.000324 | 0.999987 | 0.000780 |
| `model.visual.blocks.25.mlp.linear_fc1.weight` | 1.000530 | 0.999985 | 0.000668 |
| `model.visual.blocks.25.mlp.linear_fc2.weight` | 1.000524 | 0.999990 | 0.001030 |
| `model.visual.blocks.26.attn.proj.weight` | 0.999966 | 0.999994 | 0.001590 |
| `model.visual.blocks.26.attn.qkv.weight` | 1.000167 | 0.999983 | 0.000588 |
| `model.visual.blocks.26.mlp.linear_fc1.weight` | 1.000537 | 0.999990 | 0.001061 |
| `model.visual.blocks.26.mlp.linear_fc2.weight` | 1.000550 | 0.999998 | 0.005690 |
| `model.visual.blocks.3.attn.proj.weight` | 0.999956 | 0.999988 | 0.000841 |
| `model.visual.blocks.3.attn.qkv.weight` | 1.000599 | 0.999988 | 0.000853 |
| `model.visual.blocks.3.mlp.linear_fc1.weight` | 1.000484 | 0.999992 | 0.001292 |
| `model.visual.blocks.3.mlp.linear_fc2.weight` | 1.000714 | 0.999994 | 0.001806 |
| `model.visual.blocks.4.attn.proj.weight` | 0.999964 | 0.999984 | 0.000629 |
| `model.visual.blocks.4.attn.qkv.weight` | 1.000643 | 0.999992 | 0.001213 |
| `model.visual.blocks.4.mlp.linear_fc1.weight` | 1.000486 | 0.999993 | 0.001420 |
| `model.visual.blocks.4.mlp.linear_fc2.weight` | 1.000672 | 0.999992 | 0.001322 |
| `model.visual.blocks.5.attn.proj.weight` | 0.999947 | 0.999987 | 0.000761 |
| `model.visual.blocks.5.attn.qkv.weight` | 1.000533 | 0.999987 | 0.000768 |
| `model.visual.blocks.5.mlp.linear_fc1.weight` | 1.000515 | 0.999990 | 0.000984 |
| `model.visual.blocks.5.mlp.linear_fc2.weight` | 1.000605 | 0.999992 | 0.001198 |
| `model.visual.blocks.6.attn.proj.weight` | 0.999959 | 0.999985 | 0.000676 |
| `model.visual.blocks.6.attn.qkv.weight` | 1.000502 | 0.999990 | 0.001028 |
| `model.visual.blocks.6.mlp.linear_fc1.weight` | 1.000498 | 0.999994 | 0.001657 |
| `model.visual.blocks.6.mlp.linear_fc2.weight` | 1.000494 | 0.999994 | 0.001643 |
| `model.visual.blocks.7.attn.proj.weight` | 0.999954 | 0.999985 | 0.000738 |
| `model.visual.blocks.7.attn.qkv.weight` | 1.000438 | 0.999988 | 0.000887 |
| `model.visual.blocks.7.mlp.linear_fc1.weight` | 1.000480 | 0.999990 | 0.001060 |
| `model.visual.blocks.7.mlp.linear_fc2.weight` | 1.000389 | 0.999990 | 0.000992 |
| `model.visual.blocks.8.attn.proj.weight` | 0.999939 | 0.999990 | 0.001114 |
| `model.visual.blocks.8.attn.qkv.weight` | 1.000437 | 0.999986 | 0.000737 |
| `model.visual.blocks.8.mlp.linear_fc1.weight` | 1.000489 | 0.999990 | 0.000978 |
| `model.visual.blocks.8.mlp.linear_fc2.weight` | 1.000385 | 0.999991 | 0.001137 |
| `model.visual.blocks.9.attn.proj.weight` | 0.999972 | 0.999980 | 0.000500 |
| `model.visual.blocks.9.attn.qkv.weight` | 1.000465 | 0.999987 | 0.000768 |
| `model.visual.blocks.9.mlp.linear_fc1.weight` | 1.000543 | 0.999992 | 0.001369 |
| `model.visual.blocks.9.mlp.linear_fc2.weight` | 1.000468 | 0.999996 | 0.002428 |
| `model.visual.merger.linear_fc1.weight` | 1.006351 | 0.999987 | 0.000780 |
| `model.visual.merger.linear_fc2.weight` | 1.001345 | 0.999990 | 0.000978 |
| `model.visual.pos_embed.weight` | 1.000274 | 1.000000 | 0.025775 |
