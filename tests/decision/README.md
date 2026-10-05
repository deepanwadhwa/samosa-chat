# Decision model evaluation fixtures

`gold.jsonl` is a hand-authored, English-only Samosa decision set. It is a
development gate, not evidence that a model is accurate. Cases cover Jobs
intent, inbox classification, evidence support and relation, adversarial
instructions inside documents, ambiguous evidence that must be reviewed,
class counts from 2 through 12, and decisive evidence placed at the beginning,
middle, and end of longer inputs.

`run_model.py` runs a pinned local checkpoint on Apple MPS and writes one JSON
object per case. Install the isolated dependencies in
`requirements-macos-arm64.txt`, download the exact snapshot revisions listed at
the top of `run_model.py`, then run for one supported budget at a time:

```sh
python3 -m venv /tmp/samosa-decision-eval
/tmp/samosa-decision-eval/bin/pip install -r tests/decision/requirements-macos-arm64.txt
hf download MoritzLaurer/ModernBERT-large-zeroshot-v2.0 \
  --revision a51e07b524299e309dd2b88d48b0cfa2bd9ec598 \
  --include model.safetensors config.json tokenizer.json tokenizer_config.json special_tokens_map.json
hf download convaiinnovations/laya-typed-decisions \
  --revision f9ab0b228f0fc0f14d873dbc99038f135c2da1b2 \
  --include model.safetensors encoder/config.json rl_agent_config.json 'tokenizer/*'
USE_TF=0 /tmp/samosa-decision-eval/bin/python tests/decision/run_model.py \
  modernbert --budget 512 --output /tmp/modernbert-512.jsonl
/tmp/samosa-decision-eval/bin/python tests/decision/evaluate.py \
  tests/decision/gold.jsonl /tmp/modernbert-512.jsonl --token-budget 512
```

Each prediction has `case_id`, `selected_option`, `scores` (option ID to
probability in `[0,1]`), `model_revision`, `token_budget`, `input_tokens`,
`latency_ms`, `peak_rss_bytes`, `swap_delta_bytes`, `thermal_celsius`, and
`thermal_observed`, and `memory_pressure` (`normal`, `warn`, or `critical`).
When the OS does not expose temperature, use JSON `null` and
`thermal_observed: false`; never estimate it from another metric. A measured
input may be shorter than the budget, but may not exceed it. For the three
placement cases, the runner adds neutral text around the authored evidence and
uses paired-token counts for ModernBERT and the model-reported count for Laya.
Review/abstention is represented by `selected_option: "review"` and a nonempty
`abstention_reason`. The evaluator reports accuracy, macro-F1, expected
calibration error, abstention precision/recall, p50/p95 latency, peak RSS,
swap growth, thermal maximum, memory-pressure counts, and results by
class-count and evidence position. Macro-F1 is macro-averaged over task and
option-count groups so intent, relation, and inbox labels are not mixed into
one class vocabulary.
It rejects missing, duplicate, extra, malformed, or mismatched-revision rows.

This fixture does not replace the plan's larger adversarial and language gold
set, native-runtime reference-logit parity, or the full 16 GiB M3 runtime
bakeoff. The committed evidence directory is an initial development run only;
thermal telemetry is unavailable to this process on the tested host.
