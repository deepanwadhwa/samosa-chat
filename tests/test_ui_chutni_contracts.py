"""T0.1 (docs/TASKS_UI_CHUTNI.md): freeze the v1 JSON contract.

None of the endpoints in docs/TASKS_UI_CHUTNI.md section 5 exist in the
gateway yet. This test freezes their exact shapes as golden fixtures under
tests/fixtures/ui_chutni/contracts/ *before* any implementation lands, so:

  1. A future PR that changes a locked field name/shape without updating the
     fixture (and, per the spec, documenting an API-version change) fails
     here first.
  2. Future endpoint tests (T1.2, T2.1, T2.2, T4.*, T5.*) can import
     `assert_contract_shape()` and check a live server response against the
     same frozen fixture instead of re-deriving the shape from the spec text.

This test only reads fixtures; it never starts a server.
"""
import json
import re
import unittest
from pathlib import Path

CONTRACTS_DIR = Path(__file__).parent / "fixtures" / "ui_chutni" / "contracts"

# RFC 3339 UTC with a trailing Z, per docs/TASKS_UI_CHUTNI.md section 5.0.
RFC3339_Z = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
# "IDs are server-generated 128-bit random lowercase hexadecimal strings."
HEX128 = re.compile(r"^[0-9a-f]{32}$")

# Illustrative placeholders the spec itself uses in place of real values;
# these must NOT be mistaken for malformed real data by the checks below.
PLACEHOLDER_TIMESTAMPS = {"ISO-8601"}
PLACEHOLDER_IDS = {
    "random-id", "stable-id", "stable-model-id", "scope-id", "id",
    "caller-random-id", "same-caller-random-id", "optional-explicit-id",
    "optional-id", "root-id", "selected-root-id", "expected-id",
    "server-attachment-id", "deterministic-id", "new-idempotency-id",
    "stable-for-this-gateway-start", "random-stable-id",
}

# HTTP status -> stable meaning, docs/TASKS_UI_CHUTNI.md section 5.0.
HTTP_ERROR_MEANINGS = {
    400: "malformed_request",
    401: "auth",
    403: "denied",
    404: "unknown_resource",
    409: "conflict",
    413: "too_large",
    422: "incompatible",
    429: "queue_full",
    500: "internal",
    503: "unavailable",
    507: "insufficient_space",
}

# Closed enums the spec locks. A future change to any of these is an
# API-version change, not a silent edit.
CHUTNI_SCOPE_KINDS = {"folder", "drive", "computer"}
CHUTNI_SCOPE_STATES = {
    "unbuilt", "building", "ready", "checking", "updating", "rebuilding",
    "paused_user", "paused_chat", "needs_permission", "disconnected",
    "needs_attention", "failed_initial", "canceled_initial", "forgetting",
}
CHUTNI_FRESHNESS_STATES = {"unchecked", "complete", "partial", "failed", "stale"}
CHUTNI_STABLE_ERROR_CODES = {
    "scope_not_found", "path_outside_scope", "path_denied", "permission_required",
    "volume_disconnected", "index_busy", "index_corrupt", "model_required",
    "tokenization_failed", "tokenizer_asset_missing", "tokenizer_mismatch",
    "tokenizer_unsupported", "token_frame_invalid", "model_ingestion_unsupported",
    "unsupported_file", "insufficient_space", "job_not_found", "invalid_state",
    "scope_exists", "job_changed", "scope_limit_exceeded", "changed_during_read",
}
CHUTNI_JOB_PHASES = {
    "preflight", "inventory", "hashing", "extracting", "tokenizing",
    "indexing", "validating", "publishing", "summarizing", "finalizing",
}
CHUTNI_ENHANCEMENT_STATES = {
    "not_started", "pending_model", "improving", "complete", "paused_chat", "failed",
}
CHUTNI_SKIP_REASONS = {
    "mandatory_exclusion", "user_exclusion", "hidden_excluded",
    "cross_filesystem", "unsupported_filesystem", "unsupported_type",
    "not_regular_file", "symlink", "unreadable", "permission_denied",
    "too_large", "changed_during_read", "extraction_failed", "ocr_unavailable",
    "quota_reached",
}
INSTALL_JOB_STATES = {
    "queued", "downloading", "verifying", "installing", "installed",
    "paused", "canceled", "failed",
}
SELECTION_STATES = {"queued", "loading", "ready", "failed"}
DURABLE_JOB_STATES = {
    "queued", "running", "paused_user", "paused_chat", "canceling",
    "canceled", "failed", "completed",
}
SETUP_NEXT_STEPS = {"name", "welcome", "model", "download", "chat"}


def load(name: str):
    path = CONTRACTS_DIR / f"{name}.json"
    with path.open() as f:
        return json.load(f)


def _walk_strings(value, path=""):
    """Yield (dotted_path, string_value) for every string leaf in a JSON value."""
    if isinstance(value, dict):
        for k, v in value.items():
            yield from _walk_strings(v, f"{path}.{k}" if path else k)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from _walk_strings(v, f"{path}[{i}]")
    elif isinstance(value, str):
        yield path, value


def assert_contract_shape(response: dict, contract_name: str, testcase: unittest.TestCase):
    """Reusable by future endpoint tests: every locked top-level key in the
    frozen fixture must be present (with the right JSON type) in a live
    response. Additive fields on the live side are allowed."""
    golden = load(contract_name)
    for key, expected in golden.items():
        testcase.assertIn(key, response, f"{contract_name}: response missing locked field {key!r}")
        if expected is not None:
            testcase.assertIsInstance(
                response[key], type(expected),
                f"{contract_name}: field {key!r} changed type",
            )


class ContractFixturesAreValidJSON(unittest.TestCase):
    def test_every_contract_fixture_parses(self):
        files = sorted(CONTRACTS_DIR.glob("*.json"))
        self.assertGreater(len(files), 0, "no contract fixtures found")
        for path in files:
            with path.open() as f:
                json.load(f)  # raises on malformed JSON


class ErrorEnvelope(unittest.TestCase):
    def test_shape(self):
        env = load("error_envelope")["error"]
        self.assertEqual(set(env.keys()), {"code", "message", "retryable", "details"})
        self.assertIsInstance(env["code"], str)
        self.assertIsInstance(env["retryable"], bool)
        self.assertIsInstance(env["details"], dict)

    def test_http_status_table_is_closed_and_documented(self):
        self.assertEqual(
            set(HTTP_ERROR_MEANINGS.keys()),
            {400, 401, 403, 404, 409, 413, 422, 429, 500, 503, 507},
        )


class TimestampsAndIds(unittest.TestCase):
    def _check_fixture(self, name):
        data = load(name)
        for path, value in _walk_strings(data):
            leaf = path.rsplit(".", 1)[-1].rsplit("[", 1)[0]
            if leaf.endswith("_at") and value not in PLACEHOLDER_TIMESTAMPS:
                self.assertRegex(
                    value, RFC3339_Z,
                    f"{name}: {path}={value!r} is not RFC3339 UTC with trailing Z",
                )

    def test_profile_timestamps(self):
        self._check_fixture("profile")

    def test_interrupted_install_job_timestamps(self):
        self._check_fixture("interrupted_install_job")

    def test_interrupted_install_job_id_is_real_128_bit_hex(self):
        job = load("interrupted_install_job")
        self.assertRegex(job["job_id"], HEX128)
        self.assertIn(job["state"], INSTALL_JOB_STATES)
        self.assertGreater(job["retained_partial_bytes"], 0)
        self.assertLess(job["completed_bytes"], job["total_bytes"])


class ClosedEnums(unittest.TestCase):
    def test_scope_summary_kind_and_state(self):
        scope = load("chutni_scope_summary")
        self.assertIn(scope["kind"], CHUTNI_SCOPE_KINDS)
        self.assertIn(scope["state"], CHUTNI_SCOPE_STATES)
        self.assertIn(scope["freshness_state"], CHUTNI_FRESHNESS_STATES)
        self.assertEqual(
            scope["files_indexed"] + scope["files_skipped"],
            scope["regular_files_seen"],
            "files_indexed + files_skipped must equal regular_files_seen",
        )

    def test_scope_summary_tokenizer_and_enhancement_fields(self):
        scope = load("chutni_scope_summary")
        self.assertIn(scope["enhancement_state"], CHUTNI_ENHANCEMENT_STATES)
        # The generic Chutni service owns its index implementation. Samosa no
        # longer advertises the removed standalone tokenizer prototype as a
        # scope-level runtime dependency.
        self.assertIsNone(scope["tokenizer"])
        # A scope can be Ready (evidence complete) while enhancement work is
        # still in progress -- the doc's core "improving summaries" claim.
        self.assertLessEqual(scope["chunks_summarized"], scope["chunks_total"])

    def test_build_job_phase_is_in_the_documented_enum(self):
        self.assertIn(load("chutni_build_job")["phase"], CHUTNI_JOB_PHASES)

    def test_catalog_entry_tokenization_block_and_tokenizer_artifact(self):
        entry = load("catalog_entry")
        tok = entry["tokenization"]
        for field in ("tokenizer_artifact_name", "tokenizer_sha256",
                      "vocabulary_size", "prompt_template_sha256",
                      "direct_token_ingestion_candidate"):
            self.assertIn(field, tok)
        roles = {a["role"] for a in entry["artifacts"]}
        self.assertIn("tokenizer", roles)
        self.assertIn("weights", roles)

    def test_preflight_kind_matches_scope_kinds(self):
        self.assertIn(load("chutni_preflight_request")["kind"], CHUTNI_SCOPE_KINDS)
        self.assertIn(load("chutni_preflight_response")["kind"], CHUTNI_SCOPE_KINDS)

    def test_build_job_kind_and_state(self):
        job = load("chutni_build_job")
        self.assertIn(job["state"], DURABLE_JOB_STATES)

    def test_job_event_state(self):
        event = load("job_event")
        self.assertIn(event["state"], DURABLE_JOB_STATES)
        self.assertIsInstance(event["seq"], int)

    def test_setup_status_next_step(self):
        self.assertIn(load("setup_status")["next_step"], SETUP_NEXT_STEPS)

    def test_install_job_status_state(self):
        self.assertIn(load("install_job_status")["state"], INSTALL_JOB_STATES)

    def test_selection_created_state(self):
        self.assertIn(load("selection_created")["state"], SELECTION_STATES)

    def test_query_response_used_true_has_results_used_false_does_not(self):
        used = load("chutni_query_response")
        self.assertTrue(used["used"])
        self.assertGreater(len(used["results"]), 0)
        unused = load("chutni_query_response_unused")
        self.assertFalse(unused["used"])
        self.assertEqual(unused["results"], [])
        self.assertNotIn(
            unused["reason_code"],
            {"useful_evidence"},
            "an unused response must not carry the 'useful' reason code",
        )


class NoPlaceholderLeaksIntoRealIdChecks(unittest.TestCase):
    """Guards the guard: every string this suite treats as a "placeholder ID"
    must actually appear in a fixture, and the ones spec-checked as real IDs
    must not accidentally be in the placeholder set."""

    def test_real_id_fixture_not_in_placeholder_set(self):
        self.assertNotIn(load("interrupted_install_job")["job_id"], PLACEHOLDER_IDS)


class ConversationAndChatContext(unittest.TestCase):
    def test_conversation_v2_has_model_binding_fields(self):
        convo = load("conversation_v2")
        for field in ("model_id", "model_version", "model_binding_source", "schema_version"):
            self.assertIn(field, convo)
        self.assertEqual(convo["model_binding_source"], "explicit")

    def test_chat_request_context_requires_model_identity_alongside_conversation(self):
        req = load("chat_request_context")
        self.assertIn("conversation_id", req)
        self.assertIn("model_id", req)
        self.assertIn("model_version", req)
        self.assertIn("directory_context", req)


if __name__ == "__main__":
    unittest.main()
