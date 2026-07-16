#!/usr/bin/env python3
"""Tests for samosa_jobs.py — J1.0 (validate), J1.1 (discovery), J1.5 (output validation)."""

import json
import os
import sys
import tempfile
import unittest

# Add dist/ to path so we can import samosa_jobs
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import samosa_jobs
import fake_serve


class TestPlanner(unittest.TestCase):
    """J1.2 — granularity planner: per-file vs per-page, forced by F-J4 / context cap."""

    BUDGET = samosa_jobs.MAX_CONTEXT - 512 - samosa_jobs.SYSTEM_RESERVE  # 23040

    def _pdf(self, pages):
        return {'input_sha256': 'deadbeef', 'media_type': 'application/pdf',
                'input_path': '/nonexistent.pdf', 'size': 0, 'pages': pages}

    def test_single_image_per_file(self):
        meta = {'input_sha256': 'aa', 'media_type': 'image/png',
                'input_path': '/x.png', 'size': 1000}
        units = samosa_jobs.plan_units(meta, 'auto', self.BUDGET)
        self.assertEqual(len(units), 1)
        self.assertEqual(units[0]['plan_reason'], 'single_image')

    def test_pdf_ten_image_pages_forced_per_page(self):
        # OWNER'S ANCHOR CASE: 10 pages each with an image -> per page (F-J4 forces it).
        pages = [{'index': i, 'text_tokens': 5, 'has_raster_figure': True} for i in range(10)]
        units = samosa_jobs.plan_units(self._pdf(pages), 'auto', self.BUDGET)
        self.assertEqual(len(units), 10)
        self.assertTrue(all(u['granularity'] == 'page' for u in units))
        self.assertTrue(all(u['plan_reason'] == 'multi_image_pages' for u in units))
        self.assertTrue(all(u['reduce_group'] == 'deadbeef' for u in units))
        self.assertEqual([u['page_index'] for u in units], list(range(10)))

    def test_pdf_small_text_per_file(self):
        pages = [{'index': i, 'text_tokens': 100, 'has_raster_figure': False} for i in range(3)]
        units = samosa_jobs.plan_units(self._pdf(pages), 'auto', self.BUDGET)
        self.assertEqual(len(units), 1)
        self.assertEqual(units[0]['plan_reason'], 'fits_budget')

    def test_pdf_one_image_fits_per_file(self):
        pages = [{'index': 0, 'text_tokens': 100, 'has_raster_figure': True}]
        units = samosa_jobs.plan_units(self._pdf(pages), 'auto', self.BUDGET)
        self.assertEqual(len(units), 1)
        self.assertEqual(units[0]['plan_reason'], 'fits_budget')

    def test_pdf_over_context_forced_per_page(self):
        pages = [{'index': i, 'text_tokens': 1000, 'has_raster_figure': False} for i in range(40)]
        units = samosa_jobs.plan_units(self._pdf(pages), 'auto', self.BUDGET)
        self.assertEqual(len(units), 40)
        self.assertTrue(all(u['plan_reason'] == 'over_context' for u in units))

    def test_pdf_forced_file_warns_multi_image(self):
        pages = [{'index': i, 'text_tokens': 5, 'has_raster_figure': True} for i in range(10)]
        units = samosa_jobs.plan_units(self._pdf(pages), 'file', self.BUDGET)
        self.assertEqual(len(units), 1)
        self.assertEqual(units[0]['granularity'], 'file')
        self.assertEqual(units[0].get('warning'), 'forced_file_multi_image')

    def test_pdf_no_metadata_extractor_unavailable(self):
        meta = {'input_sha256': 'cc', 'media_type': 'application/pdf',
                'input_path': '/x.pdf', 'size': 1000}
        units = samosa_jobs.plan_units(meta, 'auto', self.BUDGET)
        self.assertEqual(units[0]['plan_reason'], 'extractor_unavailable')

    def test_text_over_context_chunks(self):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            for i in range(2000):
                f.write("This is line number %d with some words to fill the space.\n" % i)
            path = f.name
        try:
            meta = {'input_sha256': 'bb', 'media_type': 'text/plain',
                    'input_path': path, 'size': os.path.getsize(path), 'text_tokens': 50000}
            units = samosa_jobs.plan_units(meta, 'auto', self.BUDGET)
            self.assertGreater(len(units), 1)
            self.assertTrue(all(u['granularity'] == 'chunk' for u in units))
            self.assertTrue(all(u['plan_reason'] == 'over_context' for u in units))
            self.assertTrue(all(u['reduce_group'] == 'bb' for u in units))
        finally:
            os.unlink(path)


class TestMergedOutput(unittest.TestCase):
    """J1.11 — merged output honors job.output.dir (regression: E-J1 pilot B1)."""

    def test_honors_output_dir_and_order(self):
        import pathlib

        class _Log:
            events = [
                {'type': 'item_complete', 'unit_id': 'aaa', 'input_sha256': 'aaa', 'input_path': '/z/2.txt'},
                {'type': 'item_complete', 'unit_id': 'bbb', 'input_sha256': 'bbb', 'input_path': '/z/1.txt'},
            ]

        with tempfile.TemporaryDirectory() as td:
            job_dir = pathlib.Path(td) / 'job'
            items = job_dir / 'results' / 'items'
            items.mkdir(parents=True)
            (items / 'aaa.json').write_text('{"merchant":"A","total":1}')
            (items / 'bbb.json').write_text('{"merchant":"B","total":2}')
            out_dir = pathlib.Path(td) / 'user_out'
            job = {'output': {'dir': str(out_dir), 'format': 'jsonl'},
                   'output_schema': {'properties': {'merchant': {}, 'total': {}}}}

            samosa_jobs.write_merged_output(job, str(job_dir), _Log())

            # goes to the configured dir, NOT the job dir
            self.assertTrue((out_dir / 'output.jsonl').exists())
            self.assertFalse((job_dir / 'results' / 'output.jsonl').exists())
            lines = (out_dir / 'output.jsonl').read_text().strip().split('\n')
            self.assertEqual(len(lines), 2)
            # deterministic order by input_path -> /z/1.txt (bbb) first
            self.assertEqual(json.loads(lines[0])['input_path'], '/z/1.txt')


class TestValidateJob(unittest.TestCase):
    """J1.0 — job.json validation."""

    def _valid_job(self, **overrides):
        job = {
            'schema_version': 1,
            'job_id': 'test-job-01',
            'name': 'Test Job',
            'input': {'folder': '/tmp/test-inputs', 'types': ['image/jpeg']},
            'instruction': 'Extract fields.',
            'output_schema': {
                'type': 'object',
                'required': ['total'],
                'properties': {
                    'total': {'type': ['number', 'null']},
                }
            },
        }
        job.update(overrides)
        return job

    def test_valid_job(self):
        job, errors = samosa_jobs.validate_job(self._valid_job())
        self.assertEqual(errors, [])
        self.assertIsNotNone(job)

    def test_missing_job_id(self):
        j = self._valid_job()
        del j['job_id']
        _, errors = samosa_jobs.validate_job(j)
        self.assertTrue(any('job_id' in e for e in errors))

    def test_bad_job_id(self):
        _, errors = samosa_jobs.validate_job(self._valid_job(job_id='BAD ID!'))
        self.assertTrue(any('job_id' in e for e in errors))

    def test_relative_path(self):
        _, errors = samosa_jobs.validate_job(self._valid_job(
            input={'folder': 'relative/path'}))
        self.assertTrue(any('absolute' in e for e in errors))

    def test_unknown_unit(self):
        _, errors = samosa_jobs.validate_job(self._valid_job(unit='chunk'))
        self.assertTrue(any('unit' in e for e in errors))

    def test_max_tokens_over_8192(self):
        _, errors = samosa_jobs.validate_job(self._valid_job(
            inference={'max_tokens': 9000}))
        self.assertTrue(any('max_tokens' in e for e in errors))

    def test_unknown_schema_keyword(self):
        """A typo like maxLenght must be rejected."""
        _, errors = samosa_jobs.validate_job(self._valid_job(
            output_schema={
                'type': 'object',
                'properties': {
                    'currency': {'type': 'string', 'maxLenght': 3}
                }
            }))
        self.assertTrue(any('maxLenght' in e for e in errors))

    def test_nested_schema_type(self):
        """Nested object/array types are rejected."""
        _, errors = samosa_jobs.validate_job(self._valid_job(
            output_schema={
                'type': 'object',
                'properties': {
                    'items': {'type': 'object'}
                }
            }))
        self.assertTrue(any('nested' in e for e in errors))


class TestOutputValidation(unittest.TestCase):
    """J1.5 — output validation."""

    SCHEMA = {
        'type': 'object',
        'required': ['merchant', 'date', 'total', 'currency'],
        'properties': {
            'merchant': {'type': ['string', 'null']},
            'date': {'type': ['string', 'null']},
            'subtotal': {'type': ['number', 'null']},
            'tax': {'type': ['number', 'null']},
            'total': {'type': ['number', 'null']},
            'currency': {'type': ['string', 'null'], 'maxLength': 3},
        }
    }

    def test_valid_output(self):
        content = '{"merchant":"Store","date":"2026-01-01","total":42.5,"currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['errors'], [])

    def test_missing_required(self):
        content = '{"merchant":"Store","date":"2026-01-01","currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertEqual(result['status'], 'review_required')
        self.assertIn('missing_required_field:total', result['errors'])

    def test_type_mismatch_string_for_number(self):
        content = '{"merchant":"Store","date":"2026-01-01","total":"x","currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertIn('type_mismatch:total', result['errors'])

    def test_bool_is_not_number(self):
        """total:true must fail type check for number."""
        content = '{"merchant":"Store","date":"2026-01-01","total":true,"currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertIn('type_mismatch:total', result['errors'])

    def test_maxlength_violation(self):
        content = '{"merchant":"Store","date":"2026-01-01","total":10,"currency":"USDD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertIn('constraint:currency', result['errors'])

    def test_domain_rule_pass(self):
        content = '{"merchant":"S","date":"D","subtotal":10,"tax":2,"total":12,"currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA,
                                              domain_rules=['subtotal + tax ~= total'])
        self.assertEqual(result['status'], 'passed')

    def test_domain_rule_fail(self):
        content = '{"merchant":"S","date":"D","subtotal":10,"tax":2,"total":99,"currency":"USD"}'
        result = samosa_jobs.validate_output(content, self.SCHEMA,
                                              domain_rules=['subtotal + tax ~= total'])
        self.assertTrue(any('domain:' in e for e in result['errors']))

    def test_trailing_prose(self):
        content = '{"merchant":"S","date":"D","total":10,"currency":"USD"} thanks'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertEqual(result['status'], 'passed')
        self.assertIn('trailing_prose', result['warnings'])

    def test_braces_in_strings(self):
        """Braces inside strings must not break the scanner."""
        content = '{"merchant":"use {braces}","date":"D","total":10,"currency":"USD"} thanks'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['record']['merchant'], 'use {braces}')

    def test_unparseable(self):
        content = '"sorry I cannot do that"'
        result = samosa_jobs.validate_output(content, self.SCHEMA)
        self.assertEqual(result['status'], 'review_required')
        self.assertIn('unparseable', result['errors'])

    def test_enum_json_typed(self):
        """Enum comparison must be JSON-typed: True must not match 1."""
        schema = {
            'type': 'object',
            'required': ['status'],
            'properties': {
                'status': {'type': 'integer', 'enum': [0, 1, 2]},
            }
        }
        content = '{"status":true}'
        result = samosa_jobs.validate_output(content, schema)
        # true is bool, not integer, so type_mismatch
        self.assertIn('type_mismatch:status', result['errors'])


class TestDiscovery(unittest.TestCase):
    """J1.1 — input discovery."""

    def test_basic_discovery(self):
        with tempfile.TemporaryDirectory() as d:
            # Create test files
            (img_path := os.path.join(d, 'test.jpg'))
            with open(img_path, 'wb') as f:
                f.write(b'\xff\xd8\xff\xe0' + b'\x00' * 100)

            (txt_path := os.path.join(d, 'test.txt'))
            with open(txt_path, 'w') as f:
                f.write('Hello world')

            config = {'folder': d, 'types': ['image/jpeg', 'text/plain'],
                      'max_file_bytes': 26214400}
            items, skipped = samosa_jobs.discover_inputs(config)
            self.assertEqual(len(items), 2)
            types = {it['media_type'] for it in items}
            self.assertIn('image/jpeg', types)
            self.assertIn('text/plain', types)

    def test_duplicate_excluded(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'a.txt'), 'w') as f:
                f.write('Same content')
            with open(os.path.join(d, 'b.txt'), 'w') as f:
                f.write('Same content')

            config = {'folder': d, 'types': ['text/plain'],
                      'max_file_bytes': 26214400}
            items, skipped = samosa_jobs.discover_inputs(config)
            self.assertEqual(len(items), 1)
            self.assertEqual(len(skipped), 1)
            self.assertIn('duplicate', skipped[0][1])

    def test_binary_excluded(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'blob.bin'), 'wb') as f:
                f.write(bytes(range(256)) * 4)

            config = {'folder': d, 'types': ['text/plain'],
                      'max_file_bytes': 26214400}
            items, skipped = samosa_jobs.discover_inputs(config)
            self.assertEqual(len(items), 0)
            self.assertTrue(any('unsupported' in s[1] for s in skipped))

    def test_oversize_excluded(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'big.txt'), 'w') as f:
                f.write('x' * 1000)

            config = {'folder': d, 'types': ['text/plain'],
                      'max_file_bytes': 100}
            items, skipped = samosa_jobs.discover_inputs(config)
            self.assertEqual(len(items), 0)
            self.assertTrue(any('exceeds' in s[1] for s in skipped))

    def test_mislabeled_extension(self):
        """A .jpg file that is actually text should be detected as text/plain."""
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'not_image.jpg'), 'w') as f:
                f.write('This is plain text')

            config = {'folder': d, 'types': ['text/plain'],
                      'max_file_bytes': 26214400}
            items, skipped = samosa_jobs.discover_inputs(config)
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0]['media_type'], 'text/plain')


class TestEventLog(unittest.TestCase):
    """J1.7 — event log."""

    def test_append_and_load(self):
        with tempfile.TemporaryDirectory() as d:
            log_path = os.path.join(d, 'events.jsonl')
            log = samosa_jobs.EventLog(log_path)
            log.append('job_created', job_id='test')
            log.append('item_discovered', input_sha256='abc')

            log2 = samosa_jobs.EventLog(log_path)
            log2.load()
            self.assertEqual(len(log2.events), 2)
            self.assertEqual(log2.events[0]['type'], 'job_created')
            self.assertEqual(log2.seq, 2)

    def test_torn_write_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            log_path = os.path.join(d, 'events.jsonl')
            log = samosa_jobs.EventLog(log_path)
            log.append('job_created', job_id='test')

            # Append a torn line
            with open(log_path, 'a') as f:
                f.write('{"seq":2,"type":"item_disc\n')

            log2 = samosa_jobs.EventLog(log_path)
            log2.load()
            self.assertEqual(len(log2.events), 1)

    def test_terminal_units(self):
        with tempfile.TemporaryDirectory() as d:
            log_path = os.path.join(d, 'events.jsonl')
            log = samosa_jobs.EventLog(log_path)
            log.append('item_complete', unit_id='u1')
            log.append('item_review_required', unit_id='u2')
            log.append('item_running', unit_id='u3')

            terminal = log.get_terminal_units()
            self.assertIn('u1', terminal)
            self.assertIn('u2', terminal)
            self.assertNotIn('u3', terminal)


class TestJobLock(unittest.TestCase):
    """J1.7 — process lock."""

    def test_lock_exclusive(self):
        with tempfile.TemporaryDirectory() as d:
            lock_path = os.path.join(d, 'job.lock')
            lock1 = samosa_jobs.JobLock(lock_path)
            self.assertTrue(lock1.acquire())

            lock2 = samosa_jobs.JobLock(lock_path)
            self.assertFalse(lock2.acquire())

            lock1.release()
            lock3 = samosa_jobs.JobLock(lock_path)
            self.assertTrue(lock3.acquire())
            lock3.release()


class TestReduce(unittest.TestCase):
    """J1.9 — page reduction."""

    SCHEMA = {
        'type': 'object',
        'properties': {
            'name': {'type': ['string', 'null']},
            'dob': {'type': ['string', 'null']},
            'total': {'type': ['number', 'null']},
        }
    }

    def test_deterministic_merge(self):
        units = [
            {'record': {'name': 'Alice', 'dob': None, 'total': None}, 'status': 'passed', 'errors': []},
            {'record': {'name': None, 'dob': '1990-01-01', 'total': None}, 'status': 'passed', 'errors': []},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.SCHEMA, {'mode': 'deterministic', 'model_fields': []})
        self.assertEqual(method, 'deterministic')
        self.assertEqual(merged['name'], 'Alice')
        self.assertEqual(merged['dob'], '1990-01-01')

    def test_conflict(self):
        units = [
            {'record': {'name': 'Alice', 'dob': None, 'total': 10}, 'status': 'passed', 'errors': []},
            {'record': {'name': 'Bob', 'dob': None, 'total': 20}, 'status': 'passed', 'errors': []},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.SCHEMA, {'mode': 'deterministic', 'model_fields': []})
        self.assertIsNone(merged['name'])  # Conflict
        self.assertTrue(any('reduce_conflict:name' in e for e in validation['errors']))

    def test_unparseable_page(self):
        units = [
            {'record': {'name': 'Alice'}, 'status': 'passed', 'errors': []},
            {'record': None, 'status': 'review_required', 'errors': ['unparseable'], 'unit_id': 'u2'},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.SCHEMA, {'mode': 'deterministic', 'model_fields': []})
        self.assertEqual(validation['status'], 'review_required')
        self.assertTrue(any('missing_pages' in e for e in validation['errors']))

    # --- J1.9 model reduce path ---

    NARRATIVE_SCHEMA = {
        'type': 'object',
        'properties': {
            'name': {'type': ['string', 'null']},
            'total': {'type': ['number', 'null']},
            'summary': {'type': ['string', 'null']},
        }
    }

    def test_model_fields_one_call_scalars_deterministic(self):
        """model_fields:['summary'] -> exactly one model call for summary; scalars merged deterministically."""
        calls = []

        def fake_model_call(payload, fields):
            calls.append((payload, fields))
            return '{"summary":"Two receipts totalling 30."}'

        units = [
            {'record': {'name': 'Alice', 'total': 10, 'summary': 'p1'}, 'status': 'passed', 'errors': []},
            {'record': {'name': 'Alice', 'total': 20, 'summary': 'p2'}, 'status': 'passed', 'errors': []},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.NARRATIVE_SCHEMA,
            {'mode': 'deterministic', 'model_fields': ['summary']},
            model_call=fake_model_call)

        self.assertEqual(method, 'model')
        self.assertEqual(len(calls), 1)                       # exactly one model POST
        self.assertEqual(calls[0][1], ['summary'])            # only the narrative field
        self.assertEqual(merged['name'], 'Alice')             # scalar deterministic
        self.assertEqual(merged['summary'], 'Two receipts totalling 30.')
        # total conflicts (10 vs 20) -> deterministic conflict still detected
        self.assertIsNone(merged['total'])
        self.assertTrue(any('reduce_conflict:total' in e for e in validation['errors']))
        # payload carries page status/provenance so the model cannot silently drop a page
        self.assertEqual(len(calls[0][0]), 2)
        self.assertTrue(all('status' in p for p in calls[0][0]))

    def test_mode_model_sends_whole_set(self):
        """mode:'model' routes every field through the model."""
        seen_fields = []

        def fake_model_call(payload, fields):
            seen_fields.append(fields)
            return '{"name":"Alice","total":30,"summary":"merged"}'

        units = [
            {'record': {'name': 'Alice', 'total': 10, 'summary': 'p1'}, 'status': 'passed', 'errors': []},
            {'record': {'name': 'Bob', 'total': 20, 'summary': 'p2'}, 'status': 'passed', 'errors': []},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.NARRATIVE_SCHEMA, {'mode': 'model', 'model_fields': []},
            model_call=fake_model_call)

        self.assertEqual(method, 'model')
        self.assertEqual(len(seen_fields), 1)
        self.assertEqual(set(seen_fields[0]), {'name', 'total', 'summary'})
        self.assertEqual(merged['total'], 30)                 # from model, not deterministic conflict

    def test_model_reduce_unavailable_when_no_callable(self):
        """A model reduce with no injected callable flags review, never fabricates."""
        units = [
            {'record': {'name': 'Alice', 'total': 10, 'summary': 'p1'}, 'status': 'passed', 'errors': []},
        ]
        merged, validation, method = samosa_jobs.reduce_units(
            units, self.NARRATIVE_SCHEMA, {'mode': 'model', 'model_fields': []},
            model_call=None)
        self.assertEqual(method, 'model')
        self.assertEqual(validation['status'], 'review_required')
        self.assertIn('model_reduce_unavailable', validation['errors'])
        self.assertIsNone(merged['summary'])


class TestDeriveTiming(unittest.TestCase):
    """B2 — provenance timing: serve stats when present, else runner wall-clock."""

    def test_tps_present_splits_prefill_decode(self):
        resp = {'samosa': {'tokens_per_second': 10.0}, 'usage': {'completion_tokens': 21}}
        t = samosa_jobs.derive_timing(resp, wall_seconds=5.0)
        self.assertEqual(t['wall_seconds'], 5.0)
        self.assertEqual(t['decode_seconds'], 2.0)   # (21-1)/10
        self.assertEqual(t['prefill_seconds'], 3.0)  # 5.0 - 2.0

    def test_no_samosa_records_wall_only(self):
        resp = {'usage': {'completion_tokens': 50}}
        t = samosa_jobs.derive_timing(resp, wall_seconds=12.5)
        self.assertEqual(t['wall_seconds'], 12.5)
        self.assertIsNone(t['prefill_seconds'])
        self.assertIsNone(t['decode_seconds'])

    def test_completion_one_cannot_split(self):
        resp = {'samosa': {'tokens_per_second': 10.0}, 'usage': {'completion_tokens': 1}}
        t = samosa_jobs.derive_timing(resp, wall_seconds=4.0)
        self.assertIsNone(t['decode_seconds'])
        self.assertIsNone(t['prefill_seconds'])

    def test_zero_tps_is_ignored(self):
        resp = {'samosa': {'tokens_per_second': 0}, 'usage': {'completion_tokens': 50}}
        t = samosa_jobs.derive_timing(resp, wall_seconds=4.0)
        self.assertIsNone(t['decode_seconds'])

    def test_prefill_clamped_nonnegative(self):
        # decode derived larger than wall -> prefill clamped to 0, never negative
        resp = {'samosa': {'tokens_per_second': 1.0}, 'usage': {'completion_tokens': 101}}
        t = samosa_jobs.derive_timing(resp, wall_seconds=5.0)
        self.assertEqual(t['decode_seconds'], 100.0)
        self.assertEqual(t['prefill_seconds'], 0.0)


class TestCallServe(unittest.TestCase):
    """J1.4 — model call: headers, error-code mapping, oversize short-circuit."""

    def setUp(self):
        self.server, self.port = fake_serve.start_server(0)
        self.serve_url = f'http://127.0.0.1:{self.port}'
        fake_serve.set_behavior(hang_seconds=0, fail_count=0, fail_counter=0,
                                return_429=False, return_400_context_limit=False)
        fake_serve.reset_request_count()

    def tearDown(self):
        self.server.shutdown()

    def _body(self):
        return {'messages': [{'role': 'user', 'content': 'extract'}]}

    def test_success_returns_dict(self):
        resp, err = samosa_jobs.call_serve(self._body(), self.serve_url)
        self.assertIsNone(err)
        self.assertIn('choices', resp)

    def test_background_header_sent(self):
        samosa_jobs.call_serve(self._body(), self.serve_url, is_background=True)
        headers = {k.lower(): v for k, v in fake_serve.get_last_headers().items()}
        self.assertEqual(headers.get('x-samosa-priority'), 'background')

    def test_no_background_header_when_disabled(self):
        samosa_jobs.call_serve(self._body(), self.serve_url, is_background=False)
        headers = {k.lower(): v for k, v in fake_serve.get_last_headers().items()}
        self.assertNotIn('x-samosa-priority', headers)

    def test_oversize_body_short_circuits_no_post(self):
        big = {'messages': [{'role': 'user', 'content': 'x' * (5 * 1024 * 1024)}]}
        fake_serve.reset_request_count()
        resp, err = samosa_jobs.call_serve(big, self.serve_url)
        self.assertIsNone(resp)
        self.assertEqual(err, 'image_too_large')
        self.assertEqual(fake_serve.get_request_count(), 0)  # F-J5: no POST

    def test_429_maps_to_queue_full(self):
        fake_serve.set_behavior(return_429=True)
        resp, err = samosa_jobs.call_serve(self._body(), self.serve_url)
        self.assertIsNone(resp)
        self.assertEqual(err, 'queue_full')

    def test_400_maps_to_context_limit(self):
        fake_serve.set_behavior(return_400_context_limit=True)
        resp, err = samosa_jobs.call_serve(self._body(), self.serve_url)
        self.assertIsNone(resp)
        self.assertEqual(err, 'context_limit')

    def test_500_surfaces_error(self):
        fake_serve.set_behavior(fail_count=1)
        resp, err = samosa_jobs.call_serve(self._body(), self.serve_url)
        self.assertIsNone(resp)
        self.assertEqual(err, '500')


if __name__ == '__main__':
    unittest.main()
