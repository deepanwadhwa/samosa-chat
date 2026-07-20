import json
import os
import shutil
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from dist.samosa_jobs import render_view_html, send_local_notification

class TestViewMoves(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_view_moves_escaping_and_section(self):
        hostile_file = "<img src=x onerror=alert(1)>.jpg"
        job = {
            'job_id': 'test-view-escaping',
            'name': 'test-view-escaping',
            'schema_version': 1,
            'input': {'folder': self.tmpdir}
        }
        events = [
            {
                'seq': 1,
                'type': 'move_applied',
                'src': os.path.join(self.tmpdir, hostile_file),
                'dst': os.path.join(self.tmpdir, 'JPG', hostile_file),
                'input_sha256': 'abc123'
            },
            {
                'seq': 2,
                'type': 'move_skipped',
                'src': os.path.join(self.tmpdir, 'bad.pdf'),
                'dst': os.path.join(self.tmpdir, 'PDF', 'bad.pdf'),
                'skip': 'unsafe_dest'
            }
        ]

        view_file_path = render_view_html(job, events, self.tmpdir)
        html_content = Path(view_file_path).read_text(encoding='utf-8')

        # Assert raw tag absent and escaped entity present
        self.assertNotIn('<img src=x onerror=alert(1)>.jpg', html_content)
        self.assertIn('&lt;img src=x onerror=alert(1)&gt;.jpg', html_content)

        # Bakery-test view (UI_DESIGN.md §3): plain-language outcome, the
        # never-deleted safety card, humanized skip reason, and every technical
        # detail (the move manifest) collapsed into "Details for the record".
        self.assertIn('are sorted', html_content)
        self.assertIn('Where your files are', html_content)
        self.assertIn('Nothing was deleted', html_content)
        self.assertIn('Details for the record', html_content)
        self.assertIn('safe to use', html_content)   # humanized skip reason (apostrophe escaped)
        # The J1.5/skip taxonomy string itself never appears on the page.
        self.assertNotIn('unsafe_dest', html_content)

    @patch('sys.platform', 'darwin')
    @patch('subprocess.run')
    def test_notification_invoked_without_filenames(self, mock_run):
        secret_filename = "super_secret_tax_return.pdf"
        send_local_notification("Samosa Jobs", f"Organize complete: 5 applied, 0 skipped")

        self.assertTrue(mock_run.called)
        cmd_args = mock_run.call_args[0][0]
        self.assertEqual(cmd_args[0], 'osascript')
        self.assertEqual(cmd_args[1], '-e')
        script_body = cmd_args[2]

        self.assertNotIn(secret_filename, script_body)
        self.assertIn("Organize complete: 5 applied, 0 skipped", script_body)

if __name__ == '__main__':
    unittest.main()
