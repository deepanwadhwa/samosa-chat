# E-I1: Tool-Call JSON Reliability Evaluation Report

Analyzed on: 2026-07-15
Model evaluated: `qwen/qwen3.6-35b-a3b` (Upstream FP8 on `AkashML`)

## Summary Statistics

| Metric | Count | Rate |
|---|---|---|
| **Total Runs** | 60 | 100.0% |
| **Successful Tool Calls** | 60 | 100.00% |
| **Malformed Tool Calls / JSON** | 0 | 0.00% |
| **Wrong Tool Selected** | 0 | 0.00% |
| **Hallucinated / Missing Arguments** | 0 | 0.00% |

## Verdict

> [!TIP]
> **PASS**: The malformed-JSON rate (0.00%) is within the 20% threshold. We can proceed to design and scope the C engine support for model-initiated tool calling (A3.3).

## Run Details

| Case ID | Seed | Expected Tool | Tool Called | Passed | Malformed | Wrong Tool | Hallucinated Args |
|---|---|---|---|---|---|---|---|
| `tool_document_1` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_1` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_1` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_2` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_2` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_2` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_3` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_3` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_3` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_4` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_4` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_4` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_5` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_5` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_5` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_6` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_6` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_6` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_7` | 11 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_7` | 29 | `read_document` | `read_document` | True | False | False | False |
| `tool_document_7` | 47 | `read_document` | `read_document` | True | False | False | False |
| `tool_fetch_1` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_1` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_1` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_2` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_2` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_2` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_3` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_3` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_3` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_4` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_4` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_4` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_5` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_5` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_5` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_6` | 11 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_6` | 29 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_fetch_6` | 47 | `fetch_url` | `fetch_url` | True | False | False | False |
| `tool_search_1` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_1` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_1` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_2` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_2` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_2` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_3` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_3` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_3` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_4` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_4` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_4` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_5` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_5` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_5` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_6` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_6` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_6` | 47 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_7` | 11 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_7` | 29 | `web_search` | `web_search` | True | False | False | False |
| `tool_search_7` | 47 | `web_search` | `web_search` | True | False | False | False |
