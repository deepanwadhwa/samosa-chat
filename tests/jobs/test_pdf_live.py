#!/usr/bin/env python3
"""tests/jobs/test_pdf_live.py — PDF reading through the built sidecar.

Skips when samosa-extract / the tokenizer are not installed, so the portable
suite stays green on machines without PDFium; on a machine where the sidecar is
built it proves a PDF hydrates, plans, and extracts (no extractor_unavailable).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
import samosa_jobs

REPO = os.path.join(os.path.dirname(__file__), '..', '..')
FIXTURE = os.path.join(REPO, 'tests', 'fixtures', 'documents', 'hello.pdf')


class TestPdfLive(unittest.TestCase):
    def test_fixture_pdf_reads_end_to_end(self):
        if not samosa_jobs.get_pdf_extractor():
            self.skipTest('samosa-extract not installed')
        tok = samosa_jobs.get_pdf_tokenizer()
        if not (tok and os.path.isfile(tok)):
            self.skipTest('tokenizer not found')
        if not os.path.isfile(FIXTURE):
            self.skipTest('fixture PDF missing')

        meta = {'input_path': FIXTURE, 'input_sha256': 'pdf1',
                'media_type': 'application/pdf', 'size': os.path.getsize(FIXTURE)}
        err = samosa_jobs.hydrate_pdf_input(meta)
        self.assertIsNone(err, f'hydrate failed: {err}')
        self.assertTrue(meta.get('pages'))

        units = samosa_jobs.plan_units(meta, 'auto', 20000, None)
        self.assertNotEqual(units[0]['plan_reason'], 'extractor_unavailable')

        extraction = samosa_jobs.extract_unit(units[0], meta)
        self.assertIsNone(extraction.get('error'))
        self.assertIn('Hello', extraction.get('text', ''))


if __name__ == '__main__':
    unittest.main()
