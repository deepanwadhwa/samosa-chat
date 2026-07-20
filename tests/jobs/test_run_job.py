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

    def test_decode_ambiguous_defaults_to_report_without_model(self):
        intent = J.decode_intent("do something with these", self.inbox)
        self.assertEqual(intent['kind'], 'report')

    def test_decode_ambiguous_uses_model(self):
        intent = J.decode_intent("do something with these", self.inbox,
                                 model_call=lambda msgs: "organize")
        self.assertEqual(intent['kind'], 'organize')

    def test_model_cannot_upgrade_report_to_organize(self):
        # An explicit report request stays read-only even if the model says organize.
        intent = J.decode_intent("count the files", self.inbox,
                                 model_call=lambda msgs: "organize")
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
