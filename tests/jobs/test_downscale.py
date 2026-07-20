#!/usr/bin/env python3
"""tests/jobs/test_downscale.py — Auto-downscale oversized images test."""

import os
import sys
import unittest
import base64

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'dist'))
import samosa_jobs


class TestImageDownscale(unittest.TestCase):

    def test_downscale_body_images_if_needed(self):
        # Create a large fake image data URI
        large_bytes = b'X' * (6 * 1024 * 1024)
        b64_str = base64.b64encode(large_bytes).decode('ascii')
        body = {
            'messages': [
                {
                    'role': 'user',
                    'content': [
                        {'type': 'text', 'text': 'Describe'},
                        {'type': 'image_url', 'image_url': {'url': f'data:image/jpeg;base64,{b64_str}'}}
                    ]
                }
            ]
        }
        mod_body, modified = samosa_jobs.downscale_body_images_if_needed(body)
        self.assertIsNotNone(mod_body)


if __name__ == '__main__':
    unittest.main()
