#!/usr/bin/env python3
"""tests/jobs/test_run_job.py — the Jobs layer (tools/samosa_jobs.py).

Covers intent decode, the report and organize event streams, confirm-then-apply,
undo, and error handling. The model is mocked; no backend required.
"""

import os
import json
import plistlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
import samosa_jobs as J


def drain(gen):
    """Collect events, keyed for convenience by type -> list of events."""
    events = list(gen)
    by_type = {}
    for e in events:
        by_type.setdefault(e['type'], []).append(e)
    return events, by_type


class JobsLayerTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp()
        self.jobsroot = tempfile.mkdtemp()
        os.environ['SAMOSA_JOBS_DIR'] = self.jobsroot
        self.inbox = os.path.join(self.work, 'inbox')
        os.mkdir(self.inbox)
        specs = {'a.txt': 'hello world', 'b.pdf': b'%PDF-1.4 body',
                 'c.jpg': b'\xff\xd8\xff\xe0 jpg', 'd.png': b'\x89PNG\r\n\x1a\n png'}
        for name, data in specs.items():
            mode = 'wb' if isinstance(data, bytes) else 'w'
            with open(os.path.join(self.inbox, name), mode) as f:
                f.write(data)

    def tearDown(self):
        shutil.rmtree(self.work)
        shutil.rmtree(self.jobsroot)
        os.environ.pop('SAMOSA_JOBS_DIR', None)

    # --- intent decode -----------------------------------------------------

    def test_decode_organize_by_type(self):
        intent = J.decode_intent("organize my downloads by type", self.inbox)
        self.assertEqual(intent['kind'], 'organize')
        self.assertEqual(intent['rule'], {'by': 'extension'})

    def test_decode_report(self):
        intent = J.decode_intent("how many files are in here?", self.inbox)
        self.assertEqual(intent['kind'], 'report')

    def test_decode_find(self):
        intent = J.decode_intent("find Titli's medical record", self.inbox)
        self.assertEqual(intent['kind'], 'find')

    def test_decode_find_file_path_is_not_organize(self):
        intent = J.decode_intent("find Titli's vaccination medical record and tell me the file path",
                                 self.inbox)
        self.assertEqual(intent['kind'], 'find')

    def test_decode_ambiguous_defaults_to_report_without_model(self):
        intent = J.decode_intent("do something with these", self.inbox)
        self.assertEqual(intent['kind'], 'report')

    def test_decode_ambiguous_uses_model(self):
        intent = J.decode_intent("do something with these", self.inbox,
                                 model_call=lambda msgs: "organize")
        self.assertEqual(intent['kind'], 'organize')

    def test_decode_ambiguous_uses_model_for_find(self):
        intent = J.decode_intent("Titli medical record", self.inbox,
                                 model_call=lambda msgs: "find")
        self.assertEqual(intent['kind'], 'find')

    def test_model_cannot_upgrade_report_to_organize(self):
        # An explicit report request stays read-only even if the model says organize.
        intent = J.decode_intent("count the files", self.inbox,
                                 model_call=lambda msgs: "organize")
        self.assertEqual(intent['kind'], 'report')

        intent = J.decode_intent("count the files", self.inbox,
                                 model_call=lambda msgs: "find")
        self.assertEqual(intent['kind'], 'report')

    # --- job suggestion ----------------------------------------------------

    def test_suggest_job_compiles_shipped_template(self):
        result = J.suggest_job("sort these by file type", self.inbox)
        self.assertTrue(result['ok'])
        self.assertEqual(result['template'], 'sort-by-type')
        self.assertEqual(result['source'], 'deterministic')
        self.assertEqual(result['job']['schema_version'], 1)
        self.assertEqual(result['job']['input']['folder'], os.path.abspath(self.inbox))
        self.assertEqual(result['job']['organize']['rule'], {'by': 'extension'})
        self.assertEqual(result['estimate']['unit_count'], 4)
        self.assertEqual(result['estimate']['model_units'], 0)
        self.assertEqual(result['estimate']['estimated_wall_seconds'], 0)

    def test_suggest_job_uses_model_only_to_select_known_template(self):
        receipts = os.path.join(self.work, 'receipts')
        os.mkdir(receipts)
        with open(os.path.join(receipts, 'r.txt'), 'w') as f:
            f.write('Coffee shop total 8.37')
        result = J.suggest_job("handle my errand paperwork", receipts,
                               model_call=lambda msgs: '{"template":"receipts-by-date"}')
        self.assertTrue(result['ok'])
        self.assertEqual(result['template'], 'receipts-by-date')
        self.assertEqual(result['source'], 'model')
        self.assertIn('output_schema', result['job'])
        self.assertEqual(result['job']['organize']['rule'], {'by': 'field', 'field': 'date'})
        self.assertEqual(result['estimate']['unit_count'], 1)
        self.assertGreater(result['estimate']['input_tokens'], 0)
        self.assertEqual(result['estimate']['output_tokens'], 512)

    def test_suggest_job_rejects_unshipped_template(self):
        result = J.suggest_job("email all the PDFs to Alex", self.inbox,
                               model_call=lambda msgs: '{"template":"email-pdfs"}')
        self.assertFalse(result['ok'])
        self.assertIn('no shipped job shape', result['reason'])

    def test_estimate_job_uses_exact_token_counter_when_supplied(self):
        receipts = os.path.join(self.work, 'receipts-exact')
        os.mkdir(receipts)
        with open(os.path.join(receipts, 'r.txt'), 'w') as f:
            f.write('Coffee shop total 8.37')
        job = J.suggest_job("receipts by date", receipts)['job']
        estimate = J.estimate_job(job, token_counter=lambda text: len(text.split()) + 10)
        self.assertTrue(estimate['ok'])
        self.assertEqual(estimate['unit_count'], 1)
        self.assertTrue(estimate['token_counts_exact'])
        self.assertGreater(estimate['estimated_wall_seconds'], 0)

    def test_preview_selection_defaults_to_one_item(self):
        job = {
            'input': {'folder': self.inbox, 'recursive': False},
            'instruction': 'Extract fields',
            'output_schema': {'type': 'object'},
        }
        result = J.select_preview_items(job)
        self.assertTrue(result['ok'])
        self.assertEqual(result['artifact_dir'], 'preview')
        self.assertEqual(result['sample_count'], 1)
        self.assertEqual(len(result['items']), 1)
        self.assertEqual(os.path.basename(result['items'][0]['input_path']), 'a.txt')

    def test_preview_selection_expands_deterministically_and_diversely(self):
        job = {
            'input': {'folder': self.inbox, 'recursive': False},
            'instruction': 'Extract fields',
            'output_schema': {'type': 'object'},
        }
        first = J.select_preview_items(job, sample_count=3)
        second = J.select_preview_items(job, sample_count=3)
        self.assertEqual(first, second)
        self.assertEqual(first['sample_count'], 3)
        self.assertEqual(len({i['media_type'] for i in first['items']}), 3)
        self.assertEqual(first['artifact_dir'], 'preview')

    def test_preview_job_writes_one_unit_under_preview_only_by_default(self):
        output = os.path.join(self.work, 'job-output')
        job = {
            'job_id': 'preview-default',
            'input': {'folder': self.inbox, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract fields',
            'output_schema': {'type': 'object'},
            'output': {'dir': output},
        }
        result = J.preview_job(job)
        self.assertTrue(result['ok'])
        self.assertEqual(result['sample_count'], 1)
        self.assertFalse(result['expanded'])
        self.assertTrue(os.path.exists(os.path.join(output, 'preview', 'manifest.json')))
        self.assertTrue(os.path.exists(os.path.join(output, 'preview', 'records.jsonl')))
        self.assertFalse(os.path.exists(os.path.join(output, 'output.jsonl')))
        self.assertFalse(os.path.exists(os.path.join(self.jobsroot, 'preview-default', 'events.jsonl')))
        self.assertEqual(result['records'][0]['status'], 'preview_ready')

    def test_preview_job_expanded_writes_multiple_samples_under_preview(self):
        output = os.path.join(self.work, 'job-output-expanded')
        job = {
            'job_id': 'preview-expanded',
            'input': {'folder': self.inbox, 'recursive': False},
            'instruction': 'Extract fields',
            'output_schema': {'type': 'object'},
            'output': {'dir': output},
        }
        result = J.preview_job(job, sample_count=3)
        self.assertTrue(result['ok'])
        self.assertEqual(result['sample_count'], 3)
        self.assertTrue(result['expanded'])
        records_path = os.path.join(output, 'preview', 'records.jsonl')
        with open(records_path) as f:
            records = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(len(records), 3)
        self.assertTrue(all('/preview/items/' in r['source_path'] for r in records))
        self.assertFalse(os.path.exists(os.path.join(output, 'output.jsonl')))

    def test_preview_job_file_override_keeps_one_unit(self):
        output = os.path.join(self.work, 'job-output-file')
        target = os.path.join(self.inbox, 'a.txt')
        job = {
            'input': {'folder': self.inbox, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract fields',
            'output_schema': {'type': 'object'},
            'output': {'dir': output},
        }
        result = J.preview_job(job, file_path=target, sample_count=3)
        self.assertTrue(result['ok'])
        self.assertEqual(result['sample_count'], 1)
        self.assertEqual(result['records'][0]['input_path'], target)

    def test_preview_job_can_run_model_extraction(self):
        output = os.path.join(self.work, 'job-output-model-preview')
        job = {
            'input': {'folder': self.inbox, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract merchant and total.',
            'output_schema': {
                'type': 'object',
                'required': ['merchant', 'total'],
                'properties': {'merchant': {'type': 'string'}, 'total': {'type': 'number'}},
            },
            'output': {'dir': output},
        }
        result = J.preview_job(job, model_call=lambda _messages: '{"merchant":"A","total":1.25}')
        self.assertEqual(result['records'][0]['status'], 'passed')
        self.assertEqual(result['records'][0]['extracted']['total'], 1.25)

    # --- report ------------------------------------------------------------

    def test_report_stream(self):
        events, by = drain(J.run_job("what's in this folder?", self.inbox, mode='confirm'))
        self.assertEqual([e['type'] for e in events[:3]],
                         ['decode_intent', 'intent', 'counting'])
        self.assertEqual(by['counting'][0]['total'], 4)
        self.assertIn('report', by)
        self.assertIn('done', by)
        # a report never plans or moves
        self.assertNotIn('plan', by)
        self.assertNotIn('action', by)

    # --- find --------------------------------------------------------------

    def test_find_runs_read_only_tool_loop(self):
        scripted = [
            '{"samosa_tool":"fs_list","path":"."}',
            '{"samosa_tool":"fs_read_text","path":"a.txt"}',
            'Found it at a.txt because the file contains hello world.',
        ]

        def loop_model_call(messages):
            return scripted.pop(0)

        events, by = drain(J.run_job("find the hello world note", self.inbox,
                                     mode='confirm', loop_model_call=loop_model_call))
        self.assertEqual(by['intent'][0]['kind'], 'find')
        self.assertNotIn('plan', by)
        self.assertNotIn('action', by)
        self.assertEqual([e['tool'] for e in by['tool_call']], ['fs_list', 'fs_read_text'])
        self.assertIn('a.txt', by['done'][0]['summary'])
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'a.txt')))

        job_id = by['decode_intent'][0]['job_id']
        log_path = os.path.join(self.jobsroot, job_id, 'events.jsonl')
        with open(log_path) as f:
            logged = [line for line in f if '"type":"tool_call"' in line]
        self.assertEqual(len(logged), 2)
        with open(log_path) as f:
            result_events = [json.loads(line) for line in f if '"type":"tool_result"' in line]
        self.assertEqual([e['tool'] for e in result_events], ['fs_list', 'fs_read_text'])
        self.assertIn('preview', result_events[0])
        self.assertIn('chars', result_events[0])

    def test_find_uses_candidates_from_complete_folder_index(self):
        for i in range(60):
            with open(os.path.join(self.inbox, f'archive-{i:02d}.pdf'), 'wb') as f:
                f.write(b'%PDF-1.4 archive')
        target = 'Bill_Titli_3-17.pdf'
        with open(os.path.join(self.inbox, target), 'wb') as f:
            f.write(b'%PDF-1.4 veterinary bill')

        seen = []

        def loop_model_call(messages):
            seen.append(messages[-1]['content'])
            return f'Found a likely medical record at {target}.'

        _, by = drain(J.run_job("find my cat's medical records", self.inbox,
                                mode='confirm', loop_model_call=loop_model_call))
        self.assertIn(target, seen[0])
        self.assertIn(target, by['done'][0]['summary'])

    def test_find_empty_model_response_pauses_instead_of_completing(self):
        scripted = [
            '{"samosa_tool":"fs_list","path":".","limit":"1"}',
            '',
        ]

        _, by = drain(J.run_job("find my cat's medical records", self.inbox,
                                mode='confirm', loop_model_call=lambda _messages: scripted.pop(0)))
        self.assertNotIn('done', by)
        self.assertIn('await_user', by)
        self.assertIn("pet's name", by['await_user'][0]['question'])
        job_id = by['await_user'][0]['job_id']
        self.assertTrue(os.path.exists(os.path.join(self.jobsroot, job_id, 'convo.json')))

    def test_complete_search_checks_every_readable_file(self):
        items = []
        for index in range(75):
            path = os.path.join(self.inbox, f'note-{index:02d}.txt')
            with open(path, 'w') as handle:
                handle.write('ordinary archive')
            items.append({'input_path': path, 'name': os.path.basename(path),
                          'media_type': 'text/plain'})
        target = items[-1]['input_path']
        with open(target, 'w') as handle:
            handle.write('veterinary vaccination record for a cat')

        result = J._search_all_files("find my cat's medical records", items)
        self.assertEqual(result['total'], 75)
        self.assertEqual(result['content_checked'], 75)
        self.assertEqual(result['content_unreadable'], 0)
        self.assertEqual(result['batches'], 3)
        self.assertEqual(result['matches'][0]['name'], os.path.basename(target))

    def test_complete_search_honors_small_batch_bound(self):
        items = []
        for index in range(17):
            path = os.path.join(self.inbox, f'batch-{index:02d}.txt')
            with open(path, 'w') as handle:
                handle.write('ordinary archive')
            items.append({'input_path': path, 'name': os.path.basename(path),
                          'media_type': 'text/plain'})
        result = J._search_all_files('find medical records', items, batch_size=5)
        self.assertEqual(result['content_checked'], 17)
        self.assertEqual(result['batches'], 4)

    def test_find_ask_user_pauses_and_resumes(self):
        def first_model(_messages):
            return '{"samosa_tool":"ask_user","question":"Which pet name should I look for?"}'

        events, by = drain(J.run_job("find the vaccination record", self.inbox,
                                     mode='confirm', loop_model_call=first_model))
        self.assertEqual(by['intent'][0]['kind'], 'find')
        self.assertIn('await_user', by)
        self.assertEqual(by['await_user'][0]['question'], 'Which pet name should I look for?')
        job_id = by['await_user'][0]['job_id']
        self.assertTrue(os.path.exists(os.path.join(self.jobsroot, job_id, 'convo.json')))

        seen_answer = []

        def resume_model(messages):
            seen_answer.append(messages[-1]['content'])
            return 'Found it at titli_vaccination_2025.pdf.'

        _, resumed = drain(J.answer_job(job_id, 'Titli', loop_model_call=resume_model))
        self.assertIn('Titli', seen_answer[0])
        self.assertIn('done', resumed)
        self.assertIn('titli_vaccination_2025.pdf', resumed['done'][0]['summary'])
        self.assertFalse(os.path.exists(os.path.join(self.jobsroot, job_id, 'convo.json')))

    def test_find_final_question_pauses_for_answer(self):
        def first_model(_messages):
            return 'Which pet name should I use?'

        _, by = drain(J.run_job("find the vaccination record", self.inbox,
                                mode='confirm', loop_model_call=first_model))
        self.assertIn('await_user', by)
        job_id = by['await_user'][0]['job_id']

        def resume_model(_messages):
            return 'Found it at titli_vaccination_2025.pdf.'

        _, resumed = drain(J.answer_job(job_id, 'Titli', loop_model_call=resume_model))
        self.assertIn('done', resumed)
        self.assertIn('titli_vaccination_2025.pdf', resumed['done'][0]['summary'])

    def test_find_move_stages_then_apply_and_undo(self):
        scripted = [
            '{"samosa_tool":"fs_list","path":"."}',
            '{"samosa_tool":"fs_read_text","path":"a.txt"}',
            '{"samosa_tool":"fs_move","src":"a.txt","dst":"Found/a.txt"}',
        ]

        def loop_model_call(_messages):
            return scripted.pop(0)

        events, by = drain(J.run_job("find the hello note and move it to Found",
                                     self.inbox, mode='confirm',
                                     loop_model_call=loop_model_call))
        self.assertEqual(by['intent'][0]['kind'], 'find')
        self.assertIn('plan', by)
        self.assertIn('await_apply', by)
        self.assertNotIn('action', by)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'a.txt')))
        job_id = by['await_apply'][0]['job_id']

        _, applied = drain(J.apply_job(job_id))
        self.assertEqual(applied['applied'][0]['applied'], 1)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'Found', 'a.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.inbox, 'a.txt')))

        _, undone = drain(J.undo_job(job_id))
        self.assertEqual(undone['reverted'][0]['reverted'], 1)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'a.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.inbox, 'Found', 'a.txt')))

    # --- organize: confirm then apply -------------------------------------

    def test_confirm_pauses_then_apply_and_undo(self):
        events, by = drain(J.run_job("sort these by type", self.inbox, mode='confirm'))
        self.assertIn('plan', by)
        self.assertEqual(len(by['plan'][0]['moves']), 4)
        self.assertIn('await_apply', by)
        # confirm mode must NOT move anything yet
        self.assertNotIn('action', by)
        self.assertTrue(all(os.path.exists(os.path.join(self.inbox, n))
                            for n in ('a.txt', 'b.pdf', 'c.jpg', 'd.png')))
        job_id = by['await_apply'][0]['job_id']

        # apply
        _, aby = drain(J.apply_job(job_id))
        self.assertEqual(aby['applied'][0]['applied'], 4)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'Organized', 'TXT', 'a.txt')))
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'Organized', 'PDF', 'b.pdf')))
        self.assertFalse(os.path.exists(os.path.join(self.inbox, 'a.txt')))

        # undo
        _, uby = drain(J.undo_job(job_id))
        self.assertEqual(uby['reverted'][0]['reverted'], 4)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'a.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.inbox, 'Organized', 'TXT', 'a.txt')))

        # undo again: must not replay its own revert actions as fresh work
        _, uby2 = drain(J.undo_job(job_id))
        self.assertIn('error', uby2)
        self.assertNotIn('reverted', uby2)

    # --- organize: execute ------------------------------------------------

    def test_execute_moves_immediately(self):
        events, by = drain(J.run_job("organize by type", self.inbox, mode='execute'))
        self.assertEqual(len(by['action']), 4)
        self.assertTrue(all(a['ok'] for a in by['action']))
        self.assertEqual(by['applied'][0]['applied'], 4)
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'Organized', 'JPG', 'c.jpg')))

    def test_execute_twice_is_idempotent_second_run_noop(self):
        drain(J.run_job("organize by type", self.inbox, mode='execute'))
        # second run: everything already sorted -> nothing to move
        _, by = drain(J.run_job("organize by type", self.inbox, mode='execute'))
        self.assertEqual(by['counting'][0]['total'], 0)  # inbox top-level now empty
        self.assertIn('done', by)
        self.assertNotIn('action', by)

    # --- errors ------------------------------------------------------------

    def test_missing_folder_errors(self):
        _, by = drain(J.run_job("organize by type", os.path.join(self.work, 'nope'),
                                mode='execute'))
        self.assertIn('error', by)

    def test_events_persisted_to_log(self):
        events, by = drain(J.run_job("organize by type", self.inbox, mode='execute'))
        job_id = events[0]['job_id']
        log_path = os.path.join(self.jobsroot, job_id, 'events.jsonl')
        self.assertTrue(os.path.exists(log_path))
        with open(log_path) as f:
            lines = [l for l in f if l.strip()]
        # seq numbers are monotonic and every streamed event was written
        self.assertEqual(len(lines), len(events))

    def test_run_job_definition_extracts_with_model_and_writes_output(self):
        output = os.path.join(self.work, 'extract-output')
        job = {
            'job_id': 'extract-definition',
            'input': {'folder': self.inbox, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract merchant and total.',
            'output_schema': {
                'type': 'object',
                'required': ['merchant', 'total'],
                'properties': {'merchant': {'type': 'string'}, 'total': {'type': 'number'}},
            },
            'output': {'dir': output},
        }
        events, by = drain(J.run_job_definition(
            job, model_call=lambda _messages: '{"merchant":"Coffee Shop","total":8.37}'))
        self.assertIn('item_complete', by)
        self.assertNotIn('item_review_required', by)
        with open(os.path.join(output, 'output.jsonl')) as f:
            records = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(records[0]['status'], 'passed')
        self.assertEqual(records[0]['merchant'], 'Coffee Shop')

    def test_run_job_definition_review_required_on_bad_model_output(self):
        output = os.path.join(self.work, 'extract-review-output')
        job = {
            'job_id': 'extract-review',
            'input': {'folder': self.inbox, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract merchant and total.',
            'output_schema': {
                'type': 'object',
                'required': ['merchant', 'total'],
                'properties': {'merchant': {'type': 'string'}, 'total': {'type': 'number'}},
            },
            'output': {'dir': output},
        }
        _, by = drain(J.run_job_definition(job, model_call=lambda _messages: '{"merchant":42}'))
        self.assertIn('item_review_required', by)
        with open(os.path.join(output, 'output.jsonl')) as f:
            record = json.loads(next(f))
        self.assertEqual(record['status'], 'review_required')
        self.assertIn('missing_required_field:total', record['reasons'])
        self.assertIn('type:merchant', record['reasons'])

    def test_run_job_definition_extracts_then_organizes_by_field(self):
        receipt_dir = os.path.join(self.work, 'receipts')
        os.mkdir(receipt_dir)
        with open(os.path.join(receipt_dir, 'r.txt'), 'w') as f:
            f.write('Coffee receipt')
        output = os.path.join(self.work, 'extract-organize-output')
        job = {
            'job_id': 'extract-organize',
            'input': {'folder': receipt_dir, 'recursive': False, 'types': ['text/plain']},
            'instruction': 'Extract receipt date.',
            'output_schema': {
                'type': 'object',
                'required': ['date'],
                'properties': {'date': {'type': 'string'}},
            },
            'organize': {'rule': {'by': 'field', 'field': 'date'}},
            'output': {'dir': output},
        }
        _, by = drain(J.run_job_definition(job, model_call=lambda _messages: '{"date":"2026-07-21"}'))
        self.assertIn('item_complete', by)
        self.assertEqual(by['applied'][0]['applied'], 1)
        self.assertTrue(os.path.exists(os.path.join(receipt_dir, 'Organized', '2026-07-21', 'r.txt')))

    def make_review_job(self):
        job_id = 'review-job'
        jdir = os.path.join(self.jobsroot, job_id)
        os.makedirs(os.path.join(jdir, 'results'))
        source = os.path.join(self.inbox, 'receipt.txt')
        with open(source, 'w') as f:
            f.write('Coffee Shop\nTotal 8.37\n')
        records = [
            {
                'unit_id': 'u1',
                'status': 'review_required',
                'input_path': source,
                'input_sha256': 'sha',
                'extracted': {'merchant': 'Coffee', 'total': 8.0},
                'reasons': ['low_confidence:total'],
            },
            {
                'unit_id': 'u2',
                'status': 'passed',
                'input_path': os.path.join(self.inbox, 'a.txt'),
                'extracted': {'merchant': 'Already OK'},
            },
        ]
        with open(os.path.join(jdir, 'results', 'output.jsonl'), 'w') as f:
            for record in records:
                f.write(json.dumps(record) + '\n')
        with open(os.path.join(jdir, 'job.json'), 'w') as f:
            json.dump({'folder': self.inbox}, f)
        return job_id, source

    def test_review_correction_persists_to_output_and_marks_done(self):
        job_id, source = self.make_review_job()
        listed = J.review_items(job_id)
        self.assertEqual(listed['pending'], 1)
        self.assertEqual(listed['items'][0]['source'], 'Coffee Shop\nTotal 8.37\n')

        result = J.correct_review_item(job_id, {'unit_id': 'u1'},
                                       {'merchant': 'Coffee Shop', 'total': 8.37})
        self.assertTrue(result['ok'])
        self.assertEqual(result['pending'], 0)
        self.assertTrue(result['item']['done'])

        output = os.path.join(self.jobsroot, job_id, 'results', 'output.jsonl')
        with open(output) as f:
            records = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]['status'], 'passed')
        self.assertTrue(records[0]['reviewed'])
        self.assertEqual(records[0]['extracted']['merchant'], 'Coffee Shop')
        self.assertEqual(records[0]['extracted']['total'], 8.37)
        self.assertEqual(records[0]['merchant'], 'Coffee Shop')
        self.assertEqual(records[1]['unit_id'], 'u2')

        log_path = os.path.join(self.jobsroot, job_id, 'events.jsonl')
        with open(log_path) as f:
            events = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(events[-1]['type'], 'review_item_done')
        self.assertEqual(events[-1]['fields'], ['merchant', 'total'])

    def test_review_accept_as_is_marks_done_without_rerun(self):
        job_id, _source = self.make_review_job()
        result = J.correct_review_item(job_id, {'index': 0})
        self.assertTrue(result['ok'])
        self.assertEqual(J.review_items(job_id)['pending'], 0)
        with open(os.path.join(self.jobsroot, job_id, 'results', 'output.jsonl')) as f:
            first = json.loads(next(f))
        self.assertEqual(first['status'], 'passed')
        self.assertTrue(first['reviewed'])
        self.assertEqual(first['extracted']['total'], 8.0)

    # --- scheduler / daemon foundation ------------------------------------

    def write_schedulable_job(self, name='overnight-job'):
        job_path = os.path.join(self.work, f'{name}.json')
        with open(job_path, 'w') as f:
            json.dump({
                'job_id': name,
                'input': {'folder': self.inbox, 'recursive': False},
                'organize': {'rule': {'by': 'extension'}},
                'resources': {'run_on_battery': False},
                'output': {'dir': os.path.join(self.work, 'out')},
            }, f)
        return job_path

    def test_arm_scheduled_job_freezes_definition_and_estimates(self):
        job_path = self.write_schedulable_job()
        result = J.arm_scheduled_job(job_path, window_start='22:00', window_end='06:00')
        self.assertTrue(result['ok'], result)
        self.assertTrue(os.path.exists(result['schedule_path']))
        self.assertEqual(result['schedule']['window_start'], '22:00')
        self.assertEqual(result['schedule']['window_end'], '06:00')
        self.assertEqual(result['schedule']['review_required_policy'], 'queue')
        self.assertEqual(result['estimate']['unit_count'], 4)

        with open(job_path, 'w') as f:
            json.dump({'job_id': 'overnight-job', 'input': {'folder': self.inbox},
                       'name': 'changed'}, f)
        changed = J.arm_scheduled_job(job_path)
        self.assertFalse(changed['ok'])
        self.assertIn('different definition', changed['reason'])

    def test_scheduler_decision_cross_midnight_and_battery_policy(self):
        schedule = {
            'enabled': True,
            'window_start': '22:00',
            'window_end': '06:00',
            'run_on_battery': False,
        }
        self.assertEqual(J.scheduler_decision(schedule, now_minutes=23 * 60,
                                              power={'on_battery': False})['action'], 'run')
        self.assertEqual(J.scheduler_decision(schedule, now_minutes=3 * 60,
                                              power={'on_battery': False})['action'], 'run')
        noon = J.scheduler_decision(schedule, now_minutes=12 * 60,
                                    power={'on_battery': False})
        self.assertEqual(noon['reason'], 'outside_window')
        battery = J.scheduler_decision(schedule, now_minutes=23 * 60,
                                       power={'on_battery': True})
        self.assertEqual(battery['reason'], 'on_battery')

    def test_missed_window_policy_can_run_next_start(self):
        schedule = {
            'window_start': '22:00',
            'window_end': '06:00',
            'missed_policy': 'run_next_start',
            'run_on_battery': True,
        }
        missed = J.record_missed_window(schedule, now_minutes=12 * 60)
        self.assertTrue(missed['missed'])
        decision = J.scheduler_decision(missed, now_minutes=12 * 60,
                                        power={'on_battery': False})
        self.assertEqual(decision['action'], 'run')
        self.assertEqual(decision['reason'], 'missed_window')

    def test_caffeinate_and_launchd_helpers(self):
        self.assertEqual(J.caffeinate_command(['samosa', 'jobsd-once'],
                                              system_name='Darwin'),
                         ['caffeinate', '-dimsu', 'samosa', 'jobsd-once'])
        self.assertEqual(J.caffeinate_command(['samosa'], system_name='Linux'),
                         ['samosa'])
        plist = plistlib.loads(J.launchd_plist(['/bin/echo', 'hi']).encode('utf-8'))
        self.assertEqual(plist['Label'], 'com.samosa.jobsd')
        self.assertTrue(plist['RunAtLoad'])
        self.assertEqual(plist['ProgramArguments'], ['/bin/echo', 'hi'])

        dest = os.path.join(self.work, 'LaunchAgents', 'com.samosa.jobsd.plist')
        installed = J.install_launchd_plist(dest_path=dest, program_args=['/bin/echo', 'hi'])
        self.assertTrue(installed['ok'])
        self.assertEqual(installed['load_command'], ['launchctl', 'load', dest])
        with open(dest, 'rb') as f:
            installed_plist = plistlib.loads(f.read())
        self.assertEqual(installed_plist['ProgramArguments'], ['/bin/echo', 'hi'])

    def test_host_power_status_parses_pmset(self):
        battery = J.host_power_status(system_name='Darwin',
                                      pmset_output="Now drawing from 'Battery Power'\n")
        self.assertTrue(battery['on_battery'])
        ac = J.host_power_status(system_name='Darwin',
                                 pmset_output="Now drawing from 'AC Power'\n")
        self.assertTrue(ac['ac_power'])

    def test_jobsd_once_scans_armed_schedules(self):
        job_path = self.write_schedulable_job('daemon-job')
        armed = J.arm_scheduled_job(job_path, window_start='22:00', window_end='06:00')
        self.assertTrue(armed['ok'], armed)
        result = J.jobsd_once(now_minutes=23 * 60, power={'on_battery': False})
        self.assertTrue(result['ok'])
        self.assertEqual(result['decisions'][0]['job_id'], 'daemon-job')
        self.assertEqual(result['decisions'][0]['action'], 'run')
        self.assertEqual(result['decisions'][0]['run']['status'], 'complete')
        self.assertTrue(os.path.exists(os.path.join(self.inbox, 'Organized', 'TXT', 'a.txt')))

        schedule = J._read_json_file(armed['schedule_path'])
        self.assertFalse(schedule['enabled'])
        self.assertEqual(schedule['last_status'], 'complete')
        second = J.jobsd_once(now_minutes=23 * 60, power={'on_battery': False})
        self.assertEqual(second['decisions'][0]['reason'], 'disabled')

    def test_jobsd_once_runs_report_job_without_prompting(self):
        job_path = os.path.join(self.work, 'report-job.json')
        with open(job_path, 'w') as f:
            json.dump({
                'job_id': 'report-job',
                'input': {'folder': self.inbox, 'recursive': False},
                'resources': {'run_on_battery': True},
            }, f)
        armed = J.arm_scheduled_job(job_path, window_start='22:00', window_end='06:00')
        result = J.jobsd_once(now_minutes=23 * 60, power={'on_battery': True})
        self.assertEqual(result['decisions'][0]['run']['result']['kind'], 'report')
        log_path = os.path.join(self.jobsroot, 'report-job', 'events.jsonl')
        with open(log_path) as f:
            events = [json.loads(line) for line in f if line.strip()]
        self.assertEqual(events[0]['type'], 'scheduled_job_start')
        self.assertEqual(events[-1]['type'], 'scheduled_job_complete')

    def test_manual_overnight_flow_arms_and_returns_command(self):
        job_path = self.write_schedulable_job('manual-overnight')
        result = J.arm_overnight_job(job_path)
        self.assertTrue(result['ok'], result)
        self.assertTrue(result['overnight'])
        self.assertEqual(result['schedule']['window_start'], '22:00')
        self.assertEqual(result['schedule']['window_end'], '06:00')
        self.assertIn('jobsd-once', result['manual_run_command'])


if __name__ == '__main__':
    unittest.main()
