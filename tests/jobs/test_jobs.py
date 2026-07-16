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


class TestRunReduceIntegration(unittest.TestCase):
    """J1.9 wiring — a split text file is reduced into one document record end to
    end through _run_job, and the reduced input is idempotent on re-run."""

    def setUp(self):
        self.server, self.port = fake_serve.start_server(0)
        self.serve_url = f'http://127.0.0.1:{self.port}'
        fake_serve.set_behavior(hang_seconds=0, fail_count=0, fail_counter=0,
                                return_429=False, return_400_context_limit=False)
        fake_serve.set_status(interactive_active=False, last_interactive_ts=None)
        fake_serve.reset_request_count()

        self.tmp = tempfile.mkdtemp()
        self.jobs_dir = os.path.join(self.tmp, 'jobs')
        self.inputs = os.path.join(self.tmp, 'inputs')
        os.makedirs(self.inputs)
        # ~120 KB text file -> size/4 ~30k tokens > 23040 budget -> chunk split
        with open(os.path.join(self.inputs, 'big.txt'), 'w') as f:
            for i in range(2400):
                f.write("Receipt line %d: merchant Test Store total 42.50 USD.\n" % i)

        self._env = {'SAMOSA_JOBS_DIR': self.jobs_dir, 'SAMOSA_SERVE_URL': self.serve_url}
        self._old_env = {k: os.environ.get(k) for k in self._env}
        os.environ.update(self._env)

        # Neutralize the resource gate (exercised separately in test_gate) and the
        # tokenizer (no engine binary offline -> force the size/4 fallback split).
        self._orig_gate = samosa_jobs.gate_check
        self._orig_tok = samosa_jobs.get_tokenizer_cmd
        samosa_jobs.gate_check = lambda job, url: (True, None)
        samosa_jobs.get_tokenizer_cmd = lambda: None

    def tearDown(self):
        samosa_jobs.gate_check = self._orig_gate
        samosa_jobs.get_tokenizer_cmd = self._orig_tok
        for k, v in self._old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.server.shutdown()
        import shutil as _sh
        _sh.rmtree(self.tmp, ignore_errors=True)

    def _job(self):
        job = {
            'schema_version': 1,
            'job_id': 'reduce-int',
            'name': 'Reduce integration',
            'input': {'folder': self.inputs, 'types': ['text/plain'],
                      'max_file_bytes': 26214400},
            'instruction': 'Extract fields. Return ONLY JSON.',
            'output_schema': {
                'type': 'object',
                'properties': {
                    'merchant': {'type': ['string', 'null']},
                    'date': {'type': ['string', 'null']},
                    'total': {'type': ['number', 'null']},
                    'currency': {'type': ['string', 'null'], 'maxLength': 3},
                }
            },
            'resources': {'max_attempts': 2, 'run_on_battery': True, 'min_free_gb': 0},
        }
        validated, errors = samosa_jobs.validate_job(job)
        self.assertEqual(errors, [])
        return validated

    def test_split_file_reduced_to_document_and_idempotent(self):
        import pathlib
        job = self._job()
        job_dir = pathlib.Path(self.jobs_dir) / 'reduce-int'
        job_dir.mkdir(parents=True, exist_ok=True)

        rc = samosa_jobs._run_job(job, job_dir)
        self.assertEqual(rc, 0)

        log = samosa_jobs.EventLog(job_dir / 'events.jsonl')
        log.load()

        # The file split into >1 chunk unit, all sharing one reduce_group.
        planned = [e for e in log.events if e['type'] == 'item_planned']
        self.assertGreater(len(planned), 1)
        self.assertTrue(all(e['granularity'] == 'chunk' for e in planned))
        group_sha = planned[0]['input_sha256']

        # Exactly one doc_reduced, deterministic, passed; document file written.
        reduced = [e for e in log.events if e['type'] == 'doc_reduced']
        self.assertEqual(len(reduced), 1)
        self.assertEqual(reduced[0]['method'], 'deterministic')
        self.assertEqual(reduced[0]['validation'], 'passed')
        doc_path = job_dir / 'results' / 'documents' / f"{group_sha}.json"
        self.assertTrue(doc_path.exists())
        doc = json.loads(doc_path.read_text())
        self.assertEqual(doc['merchant'], 'Test Store')

        # Provenance timing (B2) is present on the unit records.
        prov_files = list((job_dir / 'results' / 'items').glob('*.provenance.json'))
        self.assertTrue(prov_files)
        prov = json.loads(prov_files[0].read_text())
        self.assertIn('wall_seconds', prov)
        self.assertIsInstance(prov['wall_seconds'], (int, float))

        # Idempotent re-run: input is processed (units terminal + doc_reduced) so
        # no new model POSTs and no duplicate doc_reduced.
        fake_serve.reset_request_count()
        rc2 = samosa_jobs._run_job(job, job_dir)
        self.assertEqual(rc2, 0)
        self.assertEqual(fake_serve.get_request_count(), 0)
        log.load()
        reduced2 = [e for e in log.events if e['type'] == 'doc_reduced']
        self.assertEqual(len(reduced2), 1)


class TestChunking(unittest.TestCase):
    """J1.2/J1.3 — chunk units carry char ranges and extraction slices them."""

    def _write(self, n):
        f = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
        for i in range(n):
            f.write("Line %d: some words here to fill the available space.\n" % i)
        f.close()
        return f.name

    def test_chunks_carry_ranges_and_cover_file(self):
        path = self._write(4000)
        try:
            meta = {'input_sha256': 'bb', 'media_type': 'text/plain',
                    'input_path': path, 'size': os.path.getsize(path),
                    'text_tokens': 50000}  # force over budget
            units = samosa_jobs.plan_units(meta, 'auto', 23040)
            self.assertGreater(len(units), 1)
            self.assertEqual(units[0]['char_start'], 0)
            with open(path) as fh:
                total = len(fh.read())
            self.assertEqual(units[-1]['char_end'], total)  # last chunk reaches EOF
            for u in units:
                self.assertLess(u['char_start'], u['char_end'])
            for a, b in zip(units, units[1:]):
                self.assertLessEqual(b['char_start'], a['char_end'])  # overlap, no gap
        finally:
            os.unlink(path)

    def test_extract_slices_only_the_chunk(self):
        path = self._write(200)
        try:
            with open(path) as fh:
                text = fh.read()
            unit = {'unit_id': 'x#c0', 'chunk_index': 0, 'char_start': 10,
                    'char_end': 50, 'granularity': 'chunk'}
            meta = {'media_type': 'text/plain', 'input_path': path, 'input_sha256': 'x'}
            out = samosa_jobs.extract_unit(unit, meta)
            self.assertEqual(out['text'], text[10:50])
        finally:
            os.unlink(path)


class TestSplitUnit(unittest.TestCase):
    """J1.4 — split_text_unit halves a text unit; refuses non-text / minimal."""

    def _txt(self, content):
        f = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
        f.write(content)
        f.close()
        return f.name

    def test_split_whole_text_unit(self):
        path = self._txt("alpha beta gamma\n" * 100)  # 1700 chars
        try:
            unit = {'unit_id': 'sha', 'granularity': 'file',
                    'plan_reason': 'fits_budget', 'reduce_group': None}
            item = {'input_sha256': 'sha', 'input_path': path, 'media_type': 'text/plain'}
            halves = samosa_jobs.split_text_unit(unit, item)
            self.assertEqual(len(halves), 2)
            self.assertEqual(halves[0]['char_start'], 0)
            self.assertEqual(halves[0]['char_end'], halves[1]['char_start'])  # contiguous
            self.assertEqual(halves[1]['char_end'], os.path.getsize(path))
            self.assertTrue(all(h['reduce_group'] == 'sha' for h in halves))
            self.assertTrue(all(h['plan_reason'] == 'context_split' for h in halves))
        finally:
            os.unlink(path)

    def test_minimal_is_irreducible(self):
        path = self._txt("too small to split")  # < MIN_SPLIT_CHARS
        try:
            unit = {'unit_id': 'sha', 'granularity': 'file', 'reduce_group': None}
            item = {'input_sha256': 'sha', 'input_path': path, 'media_type': 'text/plain'}
            self.assertIsNone(samosa_jobs.split_text_unit(unit, item))
        finally:
            os.unlink(path)

    def test_non_text_not_split(self):
        unit = {'unit_id': 'sha', 'granularity': 'single_image'}
        item = {'input_sha256': 'sha', 'input_path': '/x.png', 'media_type': 'image/png'}
        self.assertIsNone(samosa_jobs.split_text_unit(unit, item))


class TestRunContextSplit(unittest.TestCase):
    """J1.4 wiring — a `400 context_limit` splits the unit and re-enqueues it."""

    def setUp(self):
        self.server, self.port = fake_serve.start_server(0)
        self.serve_url = f'http://127.0.0.1:{self.port}'
        fake_serve.set_behavior(hang_seconds=0, fail_count=0, fail_counter=0,
                                return_429=False, return_400_context_limit=False,
                                context_limit_count=0, context_limit_counter=0)
        fake_serve.set_status(interactive_active=False, last_interactive_ts=None)
        fake_serve.reset_request_count()

        self.tmp = tempfile.mkdtemp()
        self.inputs = os.path.join(self.tmp, 'inputs')
        os.makedirs(self.inputs)
        with open(os.path.join(self.inputs, 'small.txt'), 'w') as f:
            f.write("Merchant Test Store total 42.50 USD.\n" * 20)  # ~740 chars, 1 unit

        self._env = {'SAMOSA_JOBS_DIR': os.path.join(self.tmp, 'jobs'),
                     'SAMOSA_SERVE_URL': self.serve_url}
        self._old_env = {k: os.environ.get(k) for k in self._env}
        os.environ.update(self._env)
        self._orig_gate = samosa_jobs.gate_check
        self._orig_tok = samosa_jobs.get_tokenizer_cmd
        samosa_jobs.gate_check = lambda job, url: (True, None)
        samosa_jobs.get_tokenizer_cmd = lambda: None

    def tearDown(self):
        samosa_jobs.gate_check = self._orig_gate
        samosa_jobs.get_tokenizer_cmd = self._orig_tok
        for k, v in self._old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.server.shutdown()
        import shutil as _sh
        _sh.rmtree(self.tmp, ignore_errors=True)

    def _job(self):
        job = {
            'schema_version': 1, 'job_id': 'ctx-split', 'name': 'Context split',
            'input': {'folder': self.inputs, 'types': ['text/plain'],
                      'max_file_bytes': 26214400},
            'instruction': 'Extract. Return ONLY JSON.',
            'output_schema': {'type': 'object', 'properties': {
                'merchant': {'type': ['string', 'null']},
                'total': {'type': ['number', 'null']}}},
            'resources': {'max_attempts': 2, 'run_on_battery': True, 'min_free_gb': 0},
        }
        validated, errors = samosa_jobs.validate_job(job)
        self.assertEqual(errors, [])
        return validated

    def test_context_limit_splits_and_completes(self):
        import pathlib
        job = self._job()
        job_dir = pathlib.Path(self._env['SAMOSA_JOBS_DIR']) / 'ctx-split'
        job_dir.mkdir(parents=True, exist_ok=True)

        fake_serve.set_behavior(context_limit_count=1)  # only the whole-file POST 400s
        rc = samosa_jobs._run_job(job, job_dir)
        self.assertEqual(rc, 0)

        log = samosa_jobs.EventLog(job_dir / 'events.jsonl')
        log.load()

        splits = [e for e in log.events if e['type'] == 'item_split']
        self.assertEqual(len(splits), 1)                     # the whole-file unit split
        pieces = [e for e in log.events if e['type'] == 'item_planned'
                  and e.get('plan_reason') == 'context_split']
        self.assertEqual(len(pieces), 2)
        completes = [e for e in log.events if e['type'] == 'item_complete']
        self.assertEqual(len(completes), 2)                  # both pieces completed
        self.assertFalse([e for e in log.events if e['type'] == 'item_review_required'])
        # one document, reduced from the two pieces
        reduced = [e for e in log.events if e['type'] == 'doc_reduced']
        self.assertEqual(len(reduced), 1)
        self.assertEqual(reduced[0]['validation'], 'passed')
        # 1 rejected whole-file POST + 2 piece POSTs
        self.assertEqual(fake_serve.get_request_count(), 3)

    def test_interrupted_context_split_resumes_unfinished_piece(self):
        import pathlib
        job = self._job()
        job_dir = pathlib.Path(self._env['SAMOSA_JOBS_DIR']) / 'ctx-split'
        job_dir.mkdir(parents=True, exist_ok=True)

        fake_serve.set_behavior(context_limit_count=1)
        original_call = samosa_jobs.call_serve
        calls = 0

        def stop_before_second_piece(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise RuntimeError('simulated process death')
            return original_call(*args, **kwargs)

        samosa_jobs.call_serve = stop_before_second_piece
        try:
            with self.assertRaisesRegex(RuntimeError, 'simulated process death'):
                samosa_jobs._run_job(job, job_dir)
        finally:
            samosa_jobs.call_serve = original_call

        self.assertEqual(fake_serve.get_request_count(), 2)
        fake_serve.set_behavior(context_limit_count=0)
        self.assertEqual(samosa_jobs._run_job(job, job_dir), 0)
        self.assertEqual(fake_serve.get_request_count(), 3)

        log = samosa_jobs.EventLog(job_dir / 'events.jsonl')
        log.load()
        pieces = [e for e in log.events
                  if e['type'] == 'item_planned'
                  and e.get('plan_reason') == 'context_split']
        self.assertEqual(len(pieces), 2)
        self.assertTrue(all('char_start' in e and 'char_end' in e
                            for e in pieces))
        completes = [e for e in log.events if e['type'] == 'item_complete']
        self.assertEqual(len(completes), 2)
        reduced = [e for e in log.events if e['type'] == 'doc_reduced']
        self.assertEqual(len(reduced), 1)


class TestOrphanRecovery(unittest.TestCase):
    """J1.6 recovery step 4 — an artifact with no terminal event is reconciled,
    not reprocessed (absence of an event != absence of output)."""

    def setUp(self):
        self.server, self.port = fake_serve.start_server(0)
        self.serve_url = f'http://127.0.0.1:{self.port}'
        fake_serve.set_behavior(hang_seconds=0, fail_count=0, fail_counter=0,
                                return_429=False, return_400_context_limit=False,
                                context_limit_count=0, context_limit_counter=0)
        fake_serve.set_status(interactive_active=False, last_interactive_ts=None)
        fake_serve.reset_request_count()
        self.tmp = tempfile.mkdtemp()
        self.inputs = os.path.join(self.tmp, 'inputs')
        os.makedirs(self.inputs)
        self.path = os.path.join(self.inputs, 'r.txt')
        with open(self.path, 'w') as f:
            f.write("Merchant X total 1 USD.\n")
        self._env = {'SAMOSA_JOBS_DIR': os.path.join(self.tmp, 'jobs'),
                     'SAMOSA_SERVE_URL': self.serve_url}
        self._old_env = {k: os.environ.get(k) for k in self._env}
        os.environ.update(self._env)
        self._orig_gate = samosa_jobs.gate_check
        self._orig_tok = samosa_jobs.get_tokenizer_cmd
        samosa_jobs.gate_check = lambda job, url: (True, None)
        samosa_jobs.get_tokenizer_cmd = lambda: None

    def tearDown(self):
        samosa_jobs.gate_check = self._orig_gate
        samosa_jobs.get_tokenizer_cmd = self._orig_tok
        for k, v in self._old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.server.shutdown()
        import shutil as _sh
        _sh.rmtree(self.tmp, ignore_errors=True)

    def test_orphaned_artifact_reconciled_no_repost(self):
        import pathlib
        with open(self.path, 'rb') as f:
            sha = samosa_jobs.sha256_bytes(f.read())
        job_dir = pathlib.Path(self._env['SAMOSA_JOBS_DIR']) / 'orphan'
        items = job_dir / 'results' / 'items'
        items.mkdir(parents=True)
        # Artifact + provenance on disk (rename succeeded)...
        (items / f'{sha}.json').write_text(json.dumps({'merchant': 'X', 'total': 1}))
        (items / f'{sha}.provenance.json').write_text(json.dumps({'unit_id': sha, 'wall_seconds': 3.0}))
        # ...but the terminal event was never appended (crash window).
        log = samosa_jobs.EventLog(job_dir / 'events.jsonl')
        log.append('job_created', job_id='orphan', job_sha256='x')
        log.append('item_discovered', input_sha256=sha, input_path=self.path, media_type='text/plain')
        log.append('item_planned', unit_id=sha, input_sha256=sha, granularity='file', plan_reason='fits_budget')
        log.append('item_ingested', unit_id=sha)
        log.append('item_running', unit_id=sha, attempt=1)

        job = {
            'schema_version': 1, 'job_id': 'orphan', 'name': 'Orphan',
            'input': {'folder': self.inputs, 'types': ['text/plain'], 'max_file_bytes': 26214400},
            'instruction': 'x', 'output_schema': {'type': 'object', 'properties': {
                'merchant': {'type': ['string', 'null']}, 'total': {'type': ['number', 'null']}}},
            'resources': {'max_attempts': 2, 'run_on_battery': True, 'min_free_gb': 0},
        }
        job, errs = samosa_jobs.validate_job(job)
        self.assertEqual(errs, [])

        rc = samosa_jobs._run_job(job, job_dir)
        self.assertEqual(rc, 0)

        log.load()
        completes = [e for e in log.events if e['type'] == 'item_complete' and e.get('unit_id') == sha]
        self.assertEqual(len(completes), 1)                  # missing event appended
        self.assertEqual(fake_serve.get_request_count(), 0)  # never re-POSTed
        # the stored record is untouched
        self.assertEqual(json.loads((items / f'{sha}.json').read_text())['merchant'], 'X')

    def test_orphaned_review_artifact_restores_review_copy(self):
        import pathlib
        with open(self.path, 'rb') as f:
            sha = samosa_jobs.sha256_bytes(f.read())
        job_dir = pathlib.Path(self._env['SAMOSA_JOBS_DIR']) / 'orphan'
        items = job_dir / 'results' / 'items'
        items.mkdir(parents=True)
        (items / f'{sha}.json').write_text(json.dumps({'merchant': 'X'}))
        (items / f'{sha}.provenance.json').write_text(
            json.dumps({'unit_id': sha, 'wall_seconds': 3.0}))

        log = samosa_jobs.EventLog(job_dir / 'events.jsonl')
        log.append('job_created', job_id='orphan', job_sha256='x')
        log.append('item_discovered', input_sha256=sha, input_path=self.path,
                   media_type='text/plain')
        log.append('item_planned', unit_id=sha, input_sha256=sha,
                   granularity='file', plan_reason='fits_budget')
        log.append('item_running', unit_id=sha, attempt=1)

        job = {
            'schema_version': 1, 'job_id': 'orphan', 'name': 'Orphan',
            'input': {'folder': self.inputs, 'types': ['text/plain'],
                      'max_file_bytes': 26214400},
            'instruction': 'x',
            'output_schema': {
                'type': 'object', 'required': ['merchant', 'total'],
                'properties': {
                    'merchant': {'type': ['string', 'null']},
                    'total': {'type': ['number', 'null']},
                },
            },
            'resources': {'max_attempts': 2, 'run_on_battery': True,
                          'min_free_gb': 0},
        }
        job, errs = samosa_jobs.validate_job(job)
        self.assertEqual(errs, [])

        self.assertEqual(samosa_jobs._run_job(job, job_dir), 0)
        log.load()
        reviews = [e for e in log.events
                   if e['type'] == 'item_review_required'
                   and e.get('unit_id') == sha]
        self.assertEqual(len(reviews), 1)
        self.assertIn('missing_required_field:total', reviews[0]['reasons'])
        self.assertEqual(reviews[0]['model_call_seconds'], 3.0)
        self.assertTrue((job_dir / 'results' / 'review' / f'{sha}.json').exists())
        self.assertEqual(fake_serve.get_request_count(), 0)


class TestView(unittest.TestCase):
    """J1.12 — static view: review-first, active-inference time, escaping."""

    def test_review_first_active_time_and_escaping(self):
        import pathlib
        with tempfile.TemporaryDirectory() as td:
            job_dir = pathlib.Path(td)
            items = job_dir / 'results' / 'items'
            items.mkdir(parents=True)
            (items / 'aaa.provenance.json').write_text(json.dumps({'wall_seconds': 2.5}))
            (items / 'bbb.provenance.json').write_text(json.dumps({'wall_seconds': 1.5}))
            events = [
                {'type': 'job_created', 'ts': '2026-07-16T18:00:00Z'},
                {'type': 'item_discovered', 'input_sha256': 'aaa',
                 'input_path': '/x/ok.txt', 'ts': '2026-07-16T18:00:01Z'},
                {'type': 'item_planned', 'unit_id': 'aaa', 'input_sha256': 'aaa',
                 'granularity': 'file', 'ts': '2026-07-16T18:00:02Z'},
                {'type': 'item_retry_wait', 'unit_id': 'aaa', 'attempt': 1,
                 'error': 'timeout', 'model_call_seconds': 1.25,
                 'ts': '2026-07-16T18:00:03Z'},
                {'type': 'item_complete', 'unit_id': 'aaa', 'ts': '2026-07-16T18:00:05Z',
                 'input_path': '/x/ok.txt', 'model_call_seconds': 2.5},
                {'type': 'item_discovered', 'input_sha256': 'bbb',
                 'input_path': '<img src=x onerror=alert(1)>.jpg',
                 'ts': '2026-07-16T18:00:06Z'},
                {'type': 'item_planned', 'unit_id': 'bbb', 'input_sha256': 'bbb',
                 'granularity': 'single_image', 'ts': '2026-07-16T18:00:07Z'},
                {'type': 'item_review_required', 'unit_id': 'bbb', 'ts': '2026-07-16T18:00:10Z',
                 'input_path': '<img src=x onerror=alert(1)>.jpg',
                 'reasons': ['unparseable']},
                {'type': 'doc_reduced', 'input_sha256': 'doc',
                 'validation': 'passed', 'method': 'model',
                 'model_call_seconds': 0.75, 'ts': '2026-07-16T18:00:10Z'},
            ]
            path = samosa_jobs.render_view_html({'name': 'View test', 'job_id': 'v'},
                                                events, str(job_dir))
            out = pathlib.Path(path).read_text()

            # Hostile filename escaped; raw tag absent (J1.12 / HR-10).
            self.assertIn('&lt;img', out)
            self.assertNotIn('<img src=x', out)
            # Active inference = retry 1.25 + success 2.5 + legacy provenance
            # fallback 1.5 + model reduction .75 = 6.0s; wall = 10s.
            self.assertIn('6.0s', out)
            self.assertIn('10.0s', out)
            self.assertIn('single_image', out)
            # REVIEW_REQUIRED queue is shown before the full item table.
            self.assertIn('Needs review', out)
            self.assertLess(out.index('Needs review'), out.index('All items'))


if __name__ == '__main__':
    unittest.main()
