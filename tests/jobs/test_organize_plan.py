import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path
from dist.samosa_jobs import cmd_organize, build_organize_plan, validate_job

class TestOrganizePlan(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.jobs_root = os.path.join(self.tmpdir, 'jobs')
        os.environ['SAMOSA_JOBS_DIR'] = self.jobs_root
        self.input_folder = os.path.join(self.tmpdir, 'inputs')
        os.mkdir(self.input_folder)

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_extension_plan_deterministic(self):
        # Create test files
        f1 = os.path.join(self.input_folder, 'doc1.pdf')
        f2 = os.path.join(self.input_folder, 'photo1.jpg')
        with open(f1, 'wb') as f: f.write(b'%PDF-1.4 test content')
        with open(f2, 'wb') as f: f.write(b'\xff\xd8\xff test image')

        job = {
            'job_id': 'test-ext-plan',
            'schema_version': 1,
            'input': {'folder': self.input_folder},
            'organize': {
                'rule': {'by': 'extension', 'map': {'jpg': 'Photos'}}
            }
        }
        normalized, _ = validate_job(job)
        job_dir = os.path.join(self.jobs_root, 'test-ext-plan')
        os.makedirs(job_dir, exist_ok=True)

        moves1, _ = build_organize_plan(normalized, job_dir)
        moves2, _ = build_organize_plan(normalized, job_dir)
        self.assertEqual(moves1, moves2)

        dsts = {m['src']: m['dst'] for m in moves1 if 'dst' in m}
        self.assertTrue(dsts[f1].endswith('PDF/doc1.pdf'))
        self.assertTrue(dsts[f2].endswith('Photos/photo1.jpg'))

    def test_hostile_field_unsafe_dest(self):
        f1 = os.path.join(self.input_folder, 'receipt1.pdf')
        with open(f1, 'wb') as f: f.write(b'%PDF-1.4 receipt')

        job = {
            'job_id': 'test-hostile-field',
            'schema_version': 1,
            'input': {'folder': self.input_folder},
            'instruction': 'extract',
            'output_schema': {'type': 'object', 'properties': {'dest_dir': {'type': 'string'}}},
            'organize': {'rule': {'by': 'field', 'field': 'dest_dir'}}
        }
        normalized, _ = validate_job(job)
        job_dir = os.path.join(self.jobs_root, 'test-hostile-field')
        results_dir = os.path.join(job_dir, 'results')
        os.makedirs(results_dir, exist_ok=True)

        # output.jsonl is the flat, passed-only projection write_merged_output
        # produces: {input_sha256, input_path, <schema fields...>} — no wrapper.
        with open(os.path.join(results_dir, 'output.jsonl'), 'w') as f:
            f.write(json.dumps({
                'input_path': f1,
                'input_sha256': '123456',
                'dest_dir': '../../etc'
            }) + '\n')

        moves_or_skips, _ = build_organize_plan(normalized, job_dir)
        skips = [m for m in moves_or_skips if 'skip' in m]
        self.assertEqual(len(skips), 1)
        self.assertEqual(skips[0]['skip'], 'unsafe_dest')

    def test_unvalidated_unit_skips(self):
        f1 = os.path.join(self.input_folder, 'receipt1.pdf')
        with open(f1, 'wb') as f: f.write(b'%PDF-1.4 receipt')

        job = {
            'job_id': 'test-unvalidated',
            'schema_version': 1,
            'input': {'folder': self.input_folder},
            'instruction': 'extract',
            'output_schema': {'type': 'object', 'properties': {'dest_dir': {'type': 'string'}}},
            'organize': {'rule': {'by': 'field', 'field': 'dest_dir'}}
        }
        normalized, _ = validate_job(job)
        job_dir = os.path.join(self.jobs_root, 'test-unvalidated')
        results_dir = os.path.join(job_dir, 'results')
        os.makedirs(results_dir, exist_ok=True)

        # A review_required document never reaches output.jsonl (J1.11 emits
        # passed rows only). Its real signature is therefore *absence* from the
        # merged output — a different file passed, f1 did not.
        other = os.path.join(self.input_folder, 'other.pdf')
        with open(other, 'wb') as f: f.write(b'%PDF-1.4 other')
        with open(os.path.join(results_dir, 'output.jsonl'), 'w') as f:
            f.write(json.dumps({
                'input_path': other,
                'input_sha256': 'deadbeef',
                'dest_dir': '2027-06-05'
            }) + '\n')

        moves_or_skips, _ = build_organize_plan(normalized, job_dir)
        skips = [m for m in moves_or_skips if 'skip' in m]
        self.assertEqual(len(skips), 1)
        self.assertEqual(skips[0]['skip'], 'not_validated')

    def test_where_json_typed_comparison(self):
        f1 = os.path.join(self.input_folder, 'img1.png')
        with open(f1, 'wb') as f: f.write(b'\x89PNG test')

        job = {
            'job_id': 'test-where-type',
            'schema_version': 1,
            'input': {'folder': self.input_folder},
            'instruction': 'extract',
            'output_schema': {'type': 'object', 'properties': {'people': {'type': 'integer'}}},
            'organize': {
                'rule': {'by': 'where', 'field': 'people', 'op': 'eq', 'value': 2, 'dest': 'Two people'},
                'unmatched': 'leave'
            }
        }
        normalized, _ = validate_job(job)
        job_dir = os.path.join(self.jobs_root, 'test-where-type')
        results_dir = os.path.join(job_dir, 'results')
        os.makedirs(results_dir, exist_ok=True)

        # Extracted value is boolean True (2 == True must be False; JSON-typed)
        with open(os.path.join(results_dir, 'output.jsonl'), 'w') as f:
            f.write(json.dumps({
                'input_path': f1,
                'input_sha256': '123456',
                'people': True
            }) + '\n')

        moves_or_skips, _ = build_organize_plan(normalized, job_dir)
        skips = [m for m in moves_or_skips if 'skip' in m]
        self.assertEqual(len(skips), 1)
        self.assertEqual(skips[0]['skip'], 'unmatched')

if __name__ == '__main__':
    unittest.main()
