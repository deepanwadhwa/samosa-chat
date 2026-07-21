#!/usr/bin/env python3
"""tests/jobs/test_tools.py — the Tool layer (tools/samosa_tools.py).

Covers the permission boundary (jail + mode gating), the filesystem tools, and
the shared agent loop driven by a mocked model.
"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
import samosa_tools as T


class TestPermissionBoundary(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, 'sub'))
        with open(os.path.join(self.root, 'a.txt'), 'w') as f:
            f.write('hi')

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_resolve_inside_ok(self):
        ctx = T.ToolContext(self.root, mode='preview')
        # resolve() returns realpath-anchored paths (the jail root is realpath'd
        # for symlink safety), so compare against the canonical root.
        real_root = os.path.realpath(self.root)
        self.assertEqual(ctx.resolve('a.txt', must_exist=True), os.path.join(real_root, 'a.txt'))
        # a not-yet-existing destination inside root resolves fine
        self.assertEqual(ctx.resolve('PDF/x.pdf'), os.path.join(real_root, 'PDF', 'x.pdf'))

    def test_resolve_rejects_dotdot_escape(self):
        ctx = T.ToolContext(self.root)
        with self.assertRaises(T.ToolError):
            ctx.resolve('../evil.txt')

    def test_resolve_rejects_absolute_outside(self):
        ctx = T.ToolContext(self.root)
        with self.assertRaises(T.ToolError):
            ctx.resolve('/etc/hosts')

    def test_resolve_rejects_symlink_escape(self):
        outside = tempfile.mkdtemp()
        try:
            with open(os.path.join(outside, 'secret'), 'w') as f:
                f.write('x')
            os.symlink(outside, os.path.join(self.root, 'link'))
            ctx = T.ToolContext(self.root)
            with self.assertRaises(T.ToolError):
                ctx.resolve('link/secret', must_exist=True)
        finally:
            shutil.rmtree(outside)


class TestModeGating(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        with open(os.path.join(self.root, 'a.txt'), 'w') as f:
            f.write('hi')

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_mutating_tool_refused_in_preview(self):
        ctx = T.ToolContext(self.root, mode='preview')
        tools = T.REGISTRY.subset(['fs_mkdir', 'fs_move'])
        out = T.execute_tool({'samosa_tool': 'fs_mkdir', 'path': 'NewDir'}, ctx, tools)
        self.assertIn('not allowed in preview', out)
        self.assertFalse(os.path.exists(os.path.join(self.root, 'NewDir')))

    def test_mutating_tool_can_be_staged_by_opt_in_loop(self):
        ctx = T.ToolContext(self.root, mode='preview', stage_mutations=True)
        tools = T.REGISTRY.subset(['fs_move'])

        def model_call(_messages):
            return '{"samosa_tool":"fs_move","src":"a.txt","dst":"Keep/a.txt"}'

        events = list(T.iter_tool_loop(model_call, [{'role': 'user', 'content': 'move a'}],
                                       tools, ctx))
        self.assertEqual(events[0]['type'], 'await_apply')
        self.assertEqual(events[0]['call']['samosa_tool'], 'fs_move')
        self.assertTrue(os.path.exists(os.path.join(self.root, 'a.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.root, 'Keep', 'a.txt')))

    def test_mutating_tool_runs_in_execute(self):
        ctx = T.ToolContext(self.root, mode='execute')
        tools = T.REGISTRY.subset(['fs_mkdir'])
        out = T.execute_tool({'samosa_tool': 'fs_mkdir', 'path': 'NewDir'}, ctx, tools)
        self.assertIn('created', out)
        self.assertTrue(os.path.isdir(os.path.join(self.root, 'NewDir')))

    def test_unknown_tool_reported(self):
        ctx = T.ToolContext(self.root)
        out = T.execute_tool({'samosa_tool': 'nope'}, ctx, T.REGISTRY.subset(['fs_survey']))
        self.assertIn('unknown tool', out)

    def test_ctx_none_runs_nonmutating_tools_but_refuses_mutating(self):
        # Chat's toolset has no working folder to jail (ctx=None); a
        # non-mutating tool still runs fine, a mutating one is refused rather
        # than crashing on ctx.mode.
        register = T.Tool('_test_stateless_echo', 'echo', [], lambda a, c: 'ok', mutating=False)
        T.REGISTRY.register(register)
        out = T.execute_tool({'samosa_tool': '_test_stateless_echo'}, None, [register])
        self.assertEqual(out, 'ok')

        out = T.execute_tool({'samosa_tool': 'fs_mkdir', 'path': 'x'}, None,
                             T.REGISTRY.subset(['fs_mkdir']))
        self.assertIn('not allowed here', out)


class TestFsTools(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        specs = {'a.txt': 'hello', 'b.pdf': b'%PDF-1.4', 'c.jpg': b'\xff\xd8\xff\xe0'}
        for name, data in specs.items():
            mode = 'wb' if isinstance(data, bytes) else 'w'
            with open(os.path.join(self.root, name), mode) as f:
                f.write(data)

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_survey_counts_by_type(self):
        events = []
        ctx = T.ToolContext(self.root, emit=lambda t, **k: events.append((t, k)))
        out = T.REGISTRY.get('fs_survey').run({}, ctx)
        self.assertIn('3 files', out)
        survey = [k for t, k in events if t == 'survey'][0]
        self.assertEqual(survey['total'], 3)
        self.assertEqual(survey['by_type']['text/plain'], 1)

    def test_metadata_reports_size_mtime_and_type(self):
        ctx = T.ToolContext(self.root, mode='preview')
        out = T.REGISTRY.get('fs_metadata').run({'path': 'a.txt'}, ctx)
        self.assertIn('path\ta.txt', out)
        self.assertIn('type\ttext/plain', out)
        self.assertIn('size\t5 bytes', out)
        self.assertIn('mtime\t', out)
        self.assertIn('sha256\t', out)

    def test_notes_are_jailed_to_job_dir(self):
        job_dir = tempfile.mkdtemp()
        try:
            ctx = T.ToolContext(self.root, mode='preview', job_dir=job_dir)
            out = T.REGISTRY.get('notes_append').run({'text': 'candidate: a.txt'}, ctx)
            self.assertIn('saved', out)
            self.assertFalse(os.path.exists(os.path.join(self.root, 'notes.txt')))
            self.assertTrue(os.path.exists(os.path.join(job_dir, 'notes.txt')))
            out = T.REGISTRY.get('notes_read').run({}, ctx)
            self.assertIn('candidate: a.txt', out)
        finally:
            shutil.rmtree(job_dir)

    def test_move_is_jailed_and_atomic(self):
        events = []
        ctx = T.ToolContext(self.root, mode='execute', emit=lambda t, **k: events.append((t, k)))
        out = T.REGISTRY.get('fs_move').run(
            {'src': 'a.txt', 'dst': 'TEXT/a.txt'}, ctx)
        self.assertIn('moved', out)
        self.assertTrue(os.path.exists(os.path.join(self.root, 'TEXT', 'a.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.root, 'a.txt')))
        self.assertTrue(any(t == 'move' and k['ok'] for t, k in events))

    def test_move_rejects_escape(self):
        ctx = T.ToolContext(self.root, mode='execute')
        with self.assertRaises(T.ToolError):
            T.REGISTRY.get('fs_move').run({'src': 'a.txt', 'dst': '../a.txt'}, ctx)

    def test_is_valid_reldir(self):
        self.assertTrue(T.is_valid_reldir('PDF'))
        self.assertTrue(T.is_valid_reldir('Two people'))
        self.assertFalse(T.is_valid_reldir('../x'))
        self.assertFalse(T.is_valid_reldir('/abs'))
        self.assertFalse(T.is_valid_reldir(''))


class TestReadDocumentTool(unittest.TestCase):
    """fs_read_document through the full registry/jail stack, real PDF."""

    FIXTURES = os.path.join(os.path.dirname(__file__), '..', 'fixtures', 'documents')

    def setUp(self):
        import jobs_fs
        if jobs_fs.find_extractor() is None:
            self.skipTest('samosa-extract sidecar not built in this environment')
        self.root = tempfile.mkdtemp()
        shutil.copy(os.path.join(self.FIXTURES, 'hello.pdf'), os.path.join(self.root, 'hello.pdf'))

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_reads_real_pdf_through_the_registry(self):
        ctx = T.ToolContext(self.root, mode='preview')
        out = T.REGISTRY.get('fs_read_document').run({'path': 'hello.pdf'}, ctx)
        self.assertIn('Hello PDFium', out)

    def test_unsupported_format_raises_tool_error_not_garbage(self):
        with open(os.path.join(self.root, 'fake.docx'), 'wb') as f:
            f.write(b'PK\x03\x04 not really a docx')
        ctx = T.ToolContext(self.root, mode='preview')
        with self.assertRaises(T.ToolError):
            T.REGISTRY.get('fs_read_document').run({'path': 'fake.docx'}, ctx)

    def test_via_execute_tool_returns_message_not_raise(self):
        # execute_tool() catches ToolError and returns text, so a bad-format
        # read never aborts an agent loop or a job.
        ctx = T.ToolContext(self.root, mode='preview')
        with open(os.path.join(self.root, 'fake.rtf'), 'wb') as f:
            f.write(b'{\\rtf1 not really rtf')
        out = T.execute_tool({'samosa_tool': 'fs_read_document', 'path': 'fake.rtf'},
                             ctx, T.REGISTRY.subset(['fs_read_document']))
        self.assertIn('refused', out)
        self.assertIn('rtf', out)

    def test_jailed_like_other_fs_tools(self):
        ctx = T.ToolContext(self.root, mode='preview')
        with self.assertRaises(T.ToolError):
            T.REGISTRY.get('fs_read_document').run({'path': '../../etc/hosts'}, ctx)


class TestToolLoop(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        with open(os.path.join(self.root, 'a.txt'), 'w') as f:
            f.write('hello')
        with open(os.path.join(self.root, 'b.pdf'), 'wb') as f:
            f.write(b'%PDF-1.4')

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_loop_runs_tool_then_answers(self):
        calls = []
        ctx = T.ToolContext(self.root, mode='preview',
                            emit=lambda t, **k: calls.append((t, k)))
        tools = T.REGISTRY.subset(['fs_survey'])

        scripted = [
            '{"samosa_tool":"fs_survey"}',
            'There are 2 files: one text and one PDF.',
        ]

        def model_call(messages):
            # The tool result must have been fed back before the final turn.
            if len(scripted) == 1:
                last = messages[-1]['content']
                self.assertIn('SAMOSA_TOOL_RESULT', last)
                self.assertIn('2 files', last)
            return scripted.pop(0)

        answer = T.run_tool_loop(model_call, [{'role': 'user', 'content': 'how many files?'}],
                                 tools, ctx)
        self.assertIn('2 files', answer)
        self.assertTrue(any(t == 'tool_call' and k['tool'] == 'fs_survey' for t, k in calls))

    def test_ability_prompt_lists_registered_tools(self):
        prompt = T.ability_prompt(T.REGISTRY.subset(['fs_survey', 'fs_move']))
        self.assertIn('fs_survey', prompt)
        self.assertIn('fs_move', prompt)
        self.assertIn('samosa_tool', prompt)


if __name__ == '__main__':
    unittest.main()
