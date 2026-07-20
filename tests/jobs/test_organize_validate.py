import os
import shutil
import tempfile
import unittest
from dist.samosa_jobs import load_and_validate_job, validate_job

class TestOrganizeValidate(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_valid_metadata_extension_job_with_nulls(self):
        job = {
            'job_id': 'sort-by-type',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'instruction': None,
            'output_schema': None,
            'inference': None,
            'organize': {
                'rule': {'by': 'extension', 'map': {'jpg': 'Photos', 'pdf': 'PDFs'}},
                'dest_root': None,
                'on_collision': 'skip',
                'unmatched': 'leave'
            }
        }
        normalized, errors = validate_job(job)
        self.assertEqual(errors, [])
        self.assertIsNotNone(normalized)

    def test_malformed_1_dest_root_outside_folder(self):
        outside = os.path.dirname(self.tmpdir)
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extension'},
                'dest_root': outside,
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('inside input.folder' in e for e in errors), f"Expected dest_root error, got: {errors}")

    def test_malformed_2_dest_root_symlink(self):
        sub = os.path.join(self.tmpdir, 'sub')
        os.mkdir(sub)
        sym = os.path.join(self.tmpdir, 'sym_dir')
        os.symlink(sub, sym)

        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extension'},
                'dest_root': sym,
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('symlink' in e for e in errors), f"Expected symlink error, got: {errors}")

    def test_malformed_3_by_typo(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extenson'},
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('unknown rule type' in e for e in errors), f"Expected rule type error, got: {errors}")

    def test_malformed_4_unknown_op(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'instruction': 'Extract count',
            'output_schema': {
                'type': 'object',
                'properties': {'people': {'type': 'integer'}}
            },
            'organize': {
                'rule': {'by': 'where', 'field': 'people', 'op': 'regex', 'value': 2, 'dest': 'TwoPeople'}
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('unknown op' in e for e in errors), f"Expected op error, got: {errors}")

    def test_malformed_5_field_not_in_schema(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'instruction': 'Extract count',
            'output_schema': {
                'type': 'object',
                'properties': {'other_field': {'type': 'integer'}}
            },
            'organize': {
                'rule': {'by': 'field', 'field': 'missing_field'}
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('not in output_schema properties' in e for e in errors), f"Expected field missing error, got: {errors}")

    def test_malformed_6_map_value_invalid(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extension', 'map': {'jpg': '../up'}}
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('not a valid folder name' in e for e in errors), f"Expected map value error, got: {errors}")

    def test_malformed_7_unmatched_value_hidden(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extension'},
                'unmatched': '.hidden'
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('not a valid folder name' in e for e in errors), f"Expected unmatched error, got: {errors}")

    def test_malformed_8_unknown_organize_key(self):
        job = {
            'job_id': 'test-job',
            'schema_version': 1,
            'input': {'folder': self.tmpdir},
            'organize': {
                'rule': {'by': 'extension'},
                'unknown_key': 123
            }
        }
        _, errors = validate_job(job)
        self.assertTrue(any('unknown key' in e for e in errors), f"Expected unknown key error, got: {errors}")

if __name__ == '__main__':
    unittest.main()
