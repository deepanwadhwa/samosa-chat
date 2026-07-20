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
