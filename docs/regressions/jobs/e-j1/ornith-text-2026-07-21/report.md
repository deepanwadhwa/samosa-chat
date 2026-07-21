# E-J1 Ornith Text Extraction Checkpoint — 2026-07-21

Scope: live Ornith checkpoint over the bundled 10-item labeled text receipt
corpus. This establishes the first working live model for the extraction shape.
It does not claim image/PDF, multi-page reduction, or interactive interlock
acceptance.

## Server

Gateway launched with `model-backend` set to `ornith`:

```sh
SAMOSA_HOME=/tmp/samosa-ej1-ornith.aaBImu/home \
SAMOSA_PORT=8897 \
SAMOSA_BACKEND_PORT=8898 \
SAMOSA_APP_HTML=/Users/deepanwadhwa/Documents/samosa-chat/assets/app.html \
SAMOSA_APP_LOGO=/Users/deepanwadhwa/Documents/samosa-chat/assets/samosa-chat.png \
SAMOSA_QWEN_ENGINE=/Users/deepanwadhwa/Documents/samosa-chat/qwen36b \
SAMOSA_QWEN_MODEL=/tmp/samosa-ej1-ornith.aaBImu/no-qwen-model \
SAMOSA_TOKENIZER=/Users/deepanwadhwa/Documents/samosa-chat/tokenizer_qwen36.json \
SAMOSA_ORNITH_MODEL=/Users/deepanwadhwa/.samosa/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf \
SAMOSA_BONSAI_SERVER=/Users/deepanwadhwa/.samosa/backends/prism-llama.cpp/build/bin/llama-server \
python3 tools/samosa_gateway.py
```

Health:

```json
{"gateway":true,"backend":"ornith","label":"Ornith 9B","model":"ornith-1.0-9b","supports_images":false,"ready":true,"loading":false,"generating":false,"pid":14718}
```

## Corpus

- Inputs: `tests/fixtures/jobs/e_j1_text`
- Labels: `tests/fixtures/jobs/e_j1_labels.json`
- Prompt: strict JSON extraction of `merchant`, `date`, `subtotal`, `tax`,
  `total`, and `currency`.

## Results

```text
r01_coffee.txt      15.006 s  6 / 6
r02_electronics.txt 12.565 s  6 / 6
r03_bakery.txt      11.490 s  6 / 6
r04_grocery.txt     14.356 s  6 / 6
r05_bookshop.txt    12.773 s  6 / 6
r06_pharmacy.txt    13.104 s  6 / 6
r07_transit.txt     12.379 s  6 / 6
r08_hardware.txt    13.553 s  6 / 6
r09_petstore.txt    15.732 s  6 / 6
r10_stationery.txt  24.498 s  6 / 6
```

Summary:

```text
Wall time: 145.517 s
Parsed records: 10 / 10
Field accuracy: 60 / 60 (100.0%)
Review required / failed: 0 / 0
```

## Safety

Before and after samples were captured in `report.json`.

```text
Swapins:  0 -> 0
Swapouts: 0 -> 0
Pages throttled: 0 -> 0
Memory free: 34% -> 33%
Thermal: no warning
Performance: no warning
Power: AC
```

Result: passed for the text-corpus Ornith checkpoint. Remaining E-J1 acceptance
coverage is image/PDF input, multi-page reduction, and interactive interlock.
