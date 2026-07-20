import json
import os
import shutil
import tempfile
import unittest
from unittest.mock import patch
from dist.samosa_jobs import load_and_validate_job, cmd_report, cmd_organize, build_organize_plan, validate_job

class TestIntents(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.jobs_root = os.path.join(self.tmpdir, 'jobs')
        os.environ['SAMOSA_JOBS_DIR'] = self.jobs_root

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_all_example_templates_validate(self):
        examples_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), 'docs', 'examples', 'jobs')
        templates = ['sort-by-type.job.json', 'folder-report.job.json', 'photos-two-people.job.json', 'receipts-by-date.job.json']
        for name in templates:
            path = os.path.join(examples_dir, name)
            self.assertTrue(os.path.exists(path), f"Template {name} missing")
            normalized, errors = load_and_validate_job(path)
            self.assertEqual(errors, [], f"Template {name} failed validation: {errors}")

    def test_sort_by_type_end_to_end_offline_zero_posts(self):
        input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(input_folder)

        # Create multi-format fixture files:
        # pdf, jpg, jpeg, png, docx-as-zip (PK), csv, extensionless png, txt renamed .jpg
        f_pdf = os.path.join(input_folder, 'doc.pdf')
        f_jpg = os.path.join(input_folder, 'img.jpg')
        f_jpeg = os.path.join(input_folder, 'img2.jpeg')
        f_png = os.path.join(input_folder, 'graphic.png')
        f_docx = os.path.join(input_folder, 'paper.docx')
        f_csv = os.path.join(input_folder, 'data.csv')
        f_extless = os.path.join(input_folder, 'photo_no_ext')
        f_fake_jpg = os.path.join(input_folder, 'text_renamed.jpg')

        with open(f_pdf, 'wb') as f: f.write(b'%PDF-1.4 pdf header')
        with open(f_jpg, 'wb') as f: f.write(b'\xff\xd8\xff\xe0 jpeg header')
        with open(f_jpeg, 'wb') as f: f.write(b'\xff\xd8\xff\xe0 jpeg header 2')
        with open(f_png, 'wb') as f: f.write(b'\x89PNG\r\n\x1a\n png header')
        with open(f_docx, 'wb') as f: f.write(b'PK\x03\x04 zip/docx header')
        with open(f_csv, 'w') as f: f.write('a,b,c\n1,2,3')
        with open(f_extless, 'wb') as f: f.write(b'\x89PNG\r\n\x1a\n png no ext')
        with open(f_fake_jpg, 'w') as f: f.write('plain text content inside jpg extension')

        job_file = os.path.join(self.tmpdir, 'sort_job.json')
        with open(job_file, 'w') as f:
            f.write(json.dumps({
                'job_id': 'test-sort-by-type',
                'schema_version': 1,
                'input': {'folder': input_folder},
                'organize': {'rule': {'by': 'extension'}}
            }))

        with patch('urllib.request.urlopen') as mock_urlopen:
            code = cmd_organize([job_file])
            self.assertEqual(code, 0)
            # Assert 0 HTTP POST calls to model backend
            self.assertFalse(mock_urlopen.called)

        plan_file = os.path.join(self.jobs_root, 'test-sort-by-type', 'results', 'organize_plan.jsonl')
        self.assertTrue(os.path.exists(plan_file))

        lines = [json.loads(line) for line in open(plan_file)]
        moves = [l for l in lines if 'dst' in l]

        dest_map = {m['src']: m['dst'] for m in moves}
        self.assertTrue(dest_map[f_pdf].endswith('PDF/doc.pdf'))
        self.assertTrue(dest_map[f_jpg].endswith('JPG/img.jpg'))
        self.assertTrue(dest_map[f_jpeg].endswith('JPEG/img2.jpeg'))
        self.assertTrue(dest_map[f_png].endswith('PNG/graphic.png'))
        self.assertTrue(dest_map[f_docx].endswith('DOCX/paper.docx'))
        self.assertTrue(dest_map[f_csv].endswith('CSV/data.csv'))
        # extensionless file mapped by magic-bytes (PNG -> PNG/)
        self.assertTrue(dest_map[f_extless].endswith('PNG/photo_no_ext'))
        # extension rule takes priority for folder name (.jpg -> JPG/)
        self.assertTrue(dest_map[f_fake_jpg].endswith('JPG/text_renamed.jpg'))

    def test_report_command_runs_offline_creates_no_plan(self):
        input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(input_folder)
        with open(os.path.join(input_folder, 'f1.txt'), 'w') as f: f.write('hello world')
        with open(os.path.join(input_folder, 'f2.pdf'), 'wb') as f: f.write(b'%PDF-1.4 pdf')

        job_file = os.path.join(self.tmpdir, 'report.json')
        with open(job_file, 'w') as f:
            f.write(f'{{"job_id": "test-report", "schema_version": 1, "input": {{"folder": "{input_folder}"}}}}')

        code = cmd_report([job_file])
        self.assertEqual(code, 0)

        plan_file = os.path.join(self.jobs_root, 'test-report', 'results', 'organize_plan.jsonl')
        self.assertFalse(os.path.exists(plan_file))

    def test_photos_two_people_canned_response(self):
        input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(input_folder)
        img1 = os.path.join(input_folder, 'img1.jpg')
        img2 = os.path.join(input_folder, 'img2.jpg')
        with open(img1, 'wb') as f: f.write(b'\xff\xd8\xff photo 1')
        with open(img2, 'wb') as f: f.write(b'\xff\xd8\xff photo 2')

        job = {
            'job_id': 'photos-two-people-test',
            'schema_version': 1,
            'input': {'folder': input_folder},
            'instruction': 'Count people',
            'output_schema': {'type': 'object', 'properties': {'people': {'type': 'integer'}}},
            'organize': {
                'rule': {'by': 'where', 'field': 'people', 'op': 'eq', 'value': 2, 'dest': 'Two people'},
                'unmatched': 'leave'
            }
        }
        normalized, errors = validate_job(job)
        self.assertEqual(errors, [])

        job_dir = os.path.join(self.jobs_root, 'photos-two-people-test')
        results_dir = os.path.join(job_dir, 'results')
        os.makedirs(results_dir, exist_ok=True)

        # Canned run output in the real flat shape write_merged_output emits:
        # img1 has people=2, img2 has people=1.
        with open(os.path.join(results_dir, 'output.jsonl'), 'w') as f:
            f.write(json.dumps({'input_path': img1, 'input_sha256': '1111', 'people': 2}) + '\n')
            f.write(json.dumps({'input_path': img2, 'input_sha256': '2222', 'people': 1}) + '\n')

        moves_or_skips, err = build_organize_plan(normalized, job_dir)
        self.assertIsNone(err)

        moves = [m for m in moves_or_skips if 'dst' in m]
        skips = [m for m in moves_or_skips if 'skip' in m]

        self.assertEqual(len(moves), 1)
        self.assertEqual(moves[0]['src'], img1)
        self.assertTrue(moves[0]['dst'].endswith('Two people/img1.jpg'))

        self.assertEqual(len(skips), 1)
        self.assertEqual(skips[0]['src'], img2)
        self.assertEqual(skips[0]['skip'], 'unmatched')

if __name__ == '__main__':
    unittest.main()
