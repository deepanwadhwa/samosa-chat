#!/usr/bin/env python3
"""tests/jobs/test_run_job.py — the Jobs layer (tools/samosa_jobs.py).

Covers intent decode, the report and organize event streams, confirm-then-apply,
undo, and error handling. The model is mocked; no backend required.
"""

import os
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


if __name__ == '__main__':
    unittest.main()
