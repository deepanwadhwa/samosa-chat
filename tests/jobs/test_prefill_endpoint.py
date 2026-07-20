#!/usr/bin/env python3
"""tests/jobs/test_prefill_endpoint.py — Tests for prefill-only endpoint & asset snapshot API."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import samosa_jobs
import fake_serve


class TestPrefillEndpoint(unittest.TestCase):

    def setUp(self):
        self.server, self.port = fake_serve.start_server(0)
        self.serve_url = f'http://127.0.0.1:{self.port}'

    def tearDown(self):
        self.server.shutdown()

    def test_call_serve_prefill(self):
        body = {
            'messages': [
                {'role': 'system', 'content': 'System prompt context'},
                {'role': 'user', 'content': 'User prompt'}
            ]
        }
        res, err = samosa_jobs.call_serve_prefill(body, self.serve_url)
        self.assertIsNone(err)
        self.assertIsNotNone(res)
        self.assertEqual(res.get('object'), 'chat.prefill')


if __name__ == '__main__':
    unittest.main()
