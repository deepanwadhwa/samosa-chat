#!/usr/bin/env python3
"""Jobs decision checks using only generated files and repository fixtures."""

import os
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import samosa_decision as decision


VETERINARY_INTENT = {"subject": "medical record", "kind": "name", "target": "a veterinary medical record for Titli the cat", "constraints": []}

class FakeModel:
    def __init__(self):
        self.calls = []

    def choice(self, *, state, instructions, criteria):
        self.calls.append((state, instructions, criteria))
        if criteria == decision.DOCUMENT_ROLES:
            return {"choice": "form", "probabilities": {"form": .95, "instructions": .02, "notice": .02, "other": .01}}
        if "Folder name:" in state:
            plausible = "possible" in state
        elif "File name:" in state:
            plausible = "Veterinary" in state
        else:
            plausible = True
        return {
            "choice": "plausible" if plausible else "unrelated",
            "probabilities": {"plausible": 0.9 if plausible else 0.23,
                              "unrelated": 0.1 if plausible else 0.77},
        }

    choice_fast = choice


class DecisionChecks(unittest.TestCase):
    def test_shared_pdf_cache_requires_complete_matching_evidence(self):
        with tempfile.TemporaryDirectory(prefix="samosa-shared-preview-") as temporary:
            root = Path(temporary)
            source = root / 'record.pdf'
            source.write_bytes(b'current PDF bytes')
            cache = root / 'cache'
            key = hashlib.sha256(source.read_bytes()).hexdigest()
            entry_path = cache / key[:2] / (key + '.json')
            entry_path.parent.mkdir(parents=True)
            result = {'ok': True, 'page_count': 2, 'pages': [
                {'index': 1, 'source': 'text_layer', 'lines': [{'text': 'First-page evidence'}]},
                {'index': 2, 'source': 'ocr', 'lines': [{'text': 'Other-page evidence'}]}]}
            entry = {'contract_version': 'reader-v3', 'pack_fingerprint': 'matching-reader',
                     'result': json.dumps(result)}

            def preview():
                info = source.stat()
                row = {'rel_path': source.name, 'dev': info.st_dev, 'ino': info.st_ino,
                       'size': info.st_size, 'mtime_ns': str(info.st_mtime_ns)}
                descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try:
                    return decision.excerpt(descriptor, row, '', '', 400)
                finally:
                    os.close(descriptor)

            with mock.patch.object(decision, 'READ_CACHE', cache), \
                 mock.patch.object(decision, 'READER_FINGERPRINT', 'matching-reader'), \
                 mock.patch.object(decision, 'pdf_text', return_value=('Fresh extraction', 'pdf_text')) as extract:
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('First-page evidence', 'shared_pdf_cache'))
                extract.assert_not_called()
                entry['pack_fingerprint'] = 'obsolete-reader'
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                entry['pack_fingerprint'] = 'matching-reader'
                result['retryable'] = True
                entry['result'] = json.dumps(result)
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                result.pop('retryable')
                result['pages'][0]['inspection'] = {'incomplete': True}
                entry['result'] = json.dumps(result)
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                result['pages'][0].pop('inspection')
                result['pages'].pop()
                entry['result'] = json.dumps(result)
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                result['page_count'] = 1
                result['pages'][0]['needs_review'] = True
                entry['result'] = json.dumps(result)
                entry_path.write_text(json.dumps(entry))
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                source.write_bytes(b'changed PDF bytes')
                self.assertEqual(preview(), ('Fresh extraction', 'pdf_text'))
                self.assertEqual(extract.call_count, 6)

    def test_source_changed_during_extraction_is_not_used(self):
        with tempfile.TemporaryDirectory(prefix="samosa-source-read-") as temporary:
            root = Path(temporary)
            path = root / "record.pdf"
            path.write_bytes(b"fixture")
            info = path.stat()
            row = {"rel_path": path.name, "dev": info.st_dev, "ino": info.st_ino,
                   "size": info.st_size, "mtime_ns": str(info.st_mtime_ns)}

            def changed_pdf(*_):
                path.write_bytes(b"changed during extraction")
                return "Stale extracted evidence", "pdf_text"

            root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                with mock.patch.object(decision, "pdf_text", side_effect=changed_pdf):
                    self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                     ("", "changed_source"))
            finally:
                os.close(root_fd)

    def test_saved_preview_refuses_changed_missing_and_replaced_sources(self):
        with tempfile.TemporaryDirectory(prefix="samosa-selection-source-") as temporary:
            root = Path(temporary)
            path = root / "record.txt"
            path.write_text("Original record", encoding="utf-8")
            info = path.stat()
            row = {"rel_path": path.name, "dev": info.st_dev, "ino": info.st_ino,
                   "size": info.st_size, "mtime_ns": str(info.st_mtime_ns)}
            root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                 ("Original record", "text"))
                # Same inode and byte count, different evidence: identity alone
                # must not authorize silently answering from the new version.
                path.write_text("Modified record", encoding="utf-8")
                os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000_000))
                self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                 ("", "changed_source"))
                path.rename(root / "old-record.txt")
                self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                 ("", "unreadable"))
                path.write_text("Original record", encoding="utf-8")
                self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                 ("", "changed_source"))
                path.unlink()
                path.symlink_to(root / "old-record.txt")
                self.assertEqual(decision.excerpt(root_fd, row, "", "", 400),
                                 ("", "unreadable"))
            finally:
                os.close(root_fd)

    def test_file_choice_includes_request_name_and_preview(self):
        model = FakeModel()
        item = {"path": "notice.pdf", "excerpt": "Immigration Services receipt notice",
                "source": "pdf_text", "excerpt_chars": 100}
        decision.score(model, VETERINARY_INTENT, [item])
        state, instructions, criteria = model.calls[0]
        self.assertEqual(state, "File name: notice.pdf\n"
                         "Preview: Immigration Services receipt notice")
        self.assertEqual(instructions, "Classify the document itself.")
        self.assertIn(VETERINARY_INTENT["target"], criteria["plausible"])
        self.assertNotIn("User request:", state)
        self.assertEqual(item["model_choice"], "unrelated")
        self.assertAlmostEqual(item["score"], 0.23)

    def test_missing_intent_never_scans_folder(self):
        with mock.patch.object(decision, "inventory") as scan:
            with self.assertRaises(decision.DecisionError):
                decision.start(FakeModel(), {"folder": "/unopened", "goal": "Find files"}, "", "", "", None)
            scan.assert_not_called()

    def test_document_type_uses_literal_evidence_and_dynamic_hypothesis(self):
        intent = {"subject": "purchase orders", "kind": "document_type", "target": "a purchase order", "constraints": []}
        model = FakeModel()
        item = {"path": "purchase-order.pdf", "excerpt": "A school research paper.", "source": "pdf_text"}
        decision.score(model, intent, [item])
        state, _, criteria = model.calls[1]
        self.assertEqual(state, item["excerpt"])
        self.assertNotIn(item["path"], state)
        self.assertEqual(criteria["plausible"], "This document is a purchase order.")
        # A filename without content cannot establish the document type, even
        # when the model assigns an artificially strong positive score.
        item = {"path": "purchase-order.pdf", "excerpt": "", "source": "name_only"}
        decision.score(model, intent, [item])
        self.assertEqual(item["decision"], "needs_check")

    def test_invalid_model_scores_fail_instead_of_claiming_match(self):
        for value in (float("nan"), float("inf"), -1, 2):
            model = mock.Mock()
            model.choice_fast.return_value = {"choice": "plausible", "probabilities": {"plausible": value, "unrelated": .1}}
            with self.assertRaises(decision.DecisionError):
                decision.score(model, VETERINARY_INTENT, [{"path": "record", "excerpt": "Literal record", "source": "text"}])

    def test_type_match_requires_the_requested_document_role(self):
        intent = {"subject": "forms", "kind": "document_type", "target": "an application form", "constraints": [],
            "document_role": {"choice": "form", "probabilities": {"form": .96, "instructions": .02, "notice": .01, "other": .01}}}
        model = mock.Mock()
        model.choice_fast.side_effect = [
            {"choice": "plausible", "probabilities": {"plausible": .91, "unrelated": .09}},
            {"choice": "notice", "probabilities": {"form": .02, "instructions": .01, "notice": .96, "other": .01}}]
        item = {"path": "opaque.pdf", "excerpt": "Notice confirming receipt of the application.", "source": "pdf_text"}
        decision.score(model, intent, [item])
        self.assertEqual(item["decision"], "rejected")
        self.assertEqual(item["document_role"]["choice"], "notice")
        self.assertLessEqual(item["score"], .2)

    def test_explicit_request_role_is_stable_with_weak_target_role_scores(self):
        # An opaque identifier does not tell NLI what role the user requested.
        # The query decision sets that scope; actual source role is independent.
        intent = {"subject": "ZX902", "kind": "document_type", "target": "ZX902 documents",
                  "constraints": [], "requested_role": "form"}
        model = mock.Mock()
        model.choice_fast.side_effect = [
            {"choice": "notice", "probabilities": {"form": .3, "instructions": .1, "notice": .5, "other": .1}},
            {"choice": "plausible", "probabilities": {"plausible": .7, "unrelated": .3}},
            {"choice": "form", "probabilities": {"form": .95, "instructions": .02, "notice": .02, "other": .01}}]
        interpreted = decision.interpret_document_role(model, decision.search_intent({"search_intent": intent}))
        item = {"path": "opaque.pdf", "excerpt": "Form ZX-902. Application fields. Applicant name: Example.", "source": "pdf_text"}
        decision.score(model, interpreted, [item])
        self.assertEqual(item["decision"], "match")
        self.assertEqual(interpreted["document_role"]["probabilities"]["form"], .3)
        # Rescoring the same evidence uses the saved query scope, not a new
        # weak guess from the abbreviated target phrase.
        model.choice_fast.side_effect = [
            {"choice": "plausible", "probabilities": {"plausible": .7, "unrelated": .3}},
            {"choice": "form", "probabilities": {"form": .95, "instructions": .02, "notice": .02, "other": .01}}]
        decision.score(model, interpreted, [item])
        self.assertEqual(item["decision"], "match")

    def test_identifier_proof_does_not_bypass_additional_conditions(self):
        intent = {"subject": "AB123", "kind": "document_type", "target": "an application form", "constraints": ["approved"],
            "document_role": {"choice": "form", "probabilities": {"form": .96, "instructions": .02, "notice": .01, "other": .01}}}
        for condition_score, expected in ((.9, "match"), (.5, "needs_check"), (.1, "rejected")):
            model = mock.Mock()
            model.choice_fast.side_effect = [
                {"choice": "unrelated", "probabilities": {"plausible": .02, "unrelated": .98}},
                {"choice": "form", "probabilities": {"form": .96, "instructions": .02, "notice": .01, "other": .01}},
                {"choice": "plausible" if condition_score >= .5 else "unrelated", "probabilities": {"plausible": condition_score, "unrelated": 1-condition_score}}]
            item = {"path": "opaque.pdf", "excerpt": "Form AB-123. Applicant information and approval status.", "source": "pdf_text"}
            decision.score(model, intent, [item])
            self.assertEqual(item["decision"], expected)

    def test_inclusive_type_variants_require_literal_and_semantic_coverage(self):
        intent = {"kind": "document_type", "document_role": {"choice": "form"},
                  "constraints": ["blank or completed", "approved or signed", "blank"]}
        model = mock.Mock()
        model.choice_fast.return_value = {"choice": "covered", "probabilities": {"covered": .95, "additional": .05}}
        self.assertEqual(set(decision.covered_type_variants(model, intent)), {"blank or completed"})
        self.assertEqual(model.choice_fast.call_count, 1)
        model.choice_fast.return_value = {"choice": "covered", "probabilities": {"covered": .6, "additional": .4}}
        self.assertEqual(decision.covered_type_variants(model, intent), {})

    def test_form_identifier_requires_complete_identity(self):
        intent = {"subject": "AB123", "document_role": {"choice": "form"}}
        self.assertEqual(decision.identifier_decision(intent, "Form AB-123. Applicant fields."), "supported")
        self.assertEqual(decision.identifier_decision(intent, "Form AB-123A. Supplement."), "different_identifier")
        self.assertEqual(decision.identifier_decision(intent, "Form CD456. AB123 is mentioned in a field."), "different_identifier")
        self.assertEqual(decision.identifier_decision(intent, "The identifier could not be read."), "identifier_not_readable")
        # Notices can identify a case using a different notice-form identifier.
        intent["document_role"]["choice"] = "notice"
        self.assertEqual(decision.identifier_decision(intent, "Form CD456. Case type AB-123."), "supported")

    def test_later_reference_cannot_replace_primary_form_identifier(self):
        intent = {"subject": "ZX902", "kind": "document_type", "target": "ZX902 application forms",
                  "constraints": [], "requested_role": "form"}
        model = FakeModel()
        text = "Form CD-456. Application fields. " + "x" * 1000 + " Form ZX-902 is referenced in an attachment."
        item = {"path": "opaque.pdf", "excerpt": text, "source": "pdf_text"}
        decision.score(model, intent, [item])
        self.assertEqual(item["decision"], "rejected")
        self.assertEqual(item["identifier_evidence"], "different_identifier")

    def test_later_preview_window_is_checked(self):
        model = mock.Mock()
        model.choice_fast.side_effect = [
            {"choice": "unrelated", "probabilities": {"plausible": .01, "unrelated": .99}},
            {"choice": "plausible", "probabilities": {"plausible": .95, "unrelated": .05}}]
        item = {"path": "record", "excerpt": "x" * 1100, "source": "text"}
        decision.score(model, VETERINARY_INTENT, [item])
        self.assertEqual(model.choice_fast.call_count, 2)
        self.assertEqual(item["decision"], "match")

    def test_direct_file_precedes_subfolder_decisions(self):
        with tempfile.TemporaryDirectory(prefix="samosa-decision-order-") as temporary:
            root = Path(temporary)
            (root / "direct.txt").write_text("Veterinary record for a cat", encoding="utf-8")
            (root / "possible").mkdir()
            (root / "possible" / "inside.txt").write_text("Veterinary note", encoding="utf-8")
            (root / "other").mkdir()
            (root / "other" / "skip.txt").write_text("Unrelated", encoding="utf-8")
            events = []
            model = FakeModel()
            with mock.patch.object(decision, "progress", side_effect=events.append):
                result = decision.start(model, {"folder": str(root),
                                                "goal": "Find a veterinary record", "search_intent": VETERINARY_INTENT}, "", "", "", None)
            direct = next(i for i, event in enumerate(events)
                          if event.get("type") == "file_decision" and event.get("path") == "direct.txt")
            folder = next(i for i, event in enumerate(events)
                          if event.get("type") == "folder_checking")
            self.assertLess(direct, folder)
            self.assertEqual({item["path"] for item in result["items"]},
                             {"direct.txt", "possible/inside.txt", "other/skip.txt"})
            self.assertTrue(all(isinstance(item["mtime_ns"], str) for item in result["items"]))
            self.assertFalse(any("Folder name:" in call[0] for call in model.calls))
            self.assertEqual(result["model_folders"], 0)
            # Saved semantic rejections cannot hide files on refresh either.
            rows, inventory = decision.inventory(str(root), model, "Find files", [{
                "path": "other", "score": .01, "search": False, "reason": "model_skipped"}])
            self.assertIn("other/skip.txt", {row["rel_path"] for row in rows})


    def test_all_direct_files_finish_before_folder_scan(self):
        with tempfile.TemporaryDirectory(prefix="samosa-decision-batches-") as temporary:
            root = Path(temporary)
            for number in range(decision.BATCH_FILES + 1):
                (root / f"file-{number:03}.txt").write_text("Veterinary note", encoding="utf-8")
            (root / "possible").mkdir()
            (root / "possible" / "inside.txt").write_text("Veterinary note", encoding="utf-8")
            model = FakeModel()
            events = []
            with mock.patch.object(decision, "progress", side_effect=events.append):
                result = decision.start(model, {"folder": str(root),
                                                "goal": "Find a veterinary record", "search_intent": VETERINARY_INTENT}, "", "", "", None)
            self.assertEqual(result["checked_files"], decision.BATCH_FILES + 2)
            self.assertEqual(result["remaining_files"], 0)
            last_direct = next(i for i, event in enumerate(events)
                               if event.get("type") == "file_decision"
                               and event.get("path") == f"file-{decision.BATCH_FILES:03}.txt")
            first_folder = next(i for i, event in enumerate(events)
                                if event.get("type") == "folder_checking")
            self.assertLess(last_direct, first_folder)
            self.assertIn("possible/inside.txt", {item["path"] for item in result["items"]})

    def test_selected_refinement_survives_longer_read_without_changing_other_files(self):
        with tempfile.TemporaryDirectory(prefix="samosa-refinement-") as temporary:
            root = Path(temporary)
            items = []
            for name in ("Veterinary-a.txt", "Veterinary-b.txt"):
                path = root / name; path.write_text("Veterinary record for Titli.")
                info = path.stat()
                items.append({"path": name, "excerpt": path.read_text(), "source": "text",
                              "dev": info.st_dev, "ino": info.st_ino, "size": info.st_size, "mtime_ns": str(info.st_mtime_ns)})
            model = FakeModel(); decision.score(model, VETERINARY_INTENT, items)
            before_other = json.loads(json.dumps(items[1]))
            previous = {"schema_version": 7, "goal": "Find veterinary records", "folder": str(root),
                        "items": items, "checked_files": 2, "search_intent": VETERINARY_INTENT}
            narrowed = {**VETERINARY_INTENT, "constraints": ["approved"]}
            refined = decision.next_action(model, {"action": "refine", "followup": "only approved", "selected_paths": [items[0]["path"]],
                                          "search_intent": narrowed}, previous, "", "", "")
            deeper = decision.next_action(model, {"action": "deepen", "selected_paths": [items[0]["path"]]}, refined, "", "", "")
            by_path = {item["path"]: item for item in deeper["items"]}
            self.assertEqual(by_path[items[0]["path"]]["search_intent"]["constraints"], ["approved"])
            self.assertEqual([c["requirement"] for c in by_path[items[0]["path"]]["condition_evidence"]], ["approved"])
            self.assertEqual(by_path[items[1]["path"]], before_other)
            self.assertEqual(deeper["search_intent"], VETERINARY_INTENT)

    @unittest.skipUnless(os.environ.get("SAMOSA_EXTRACT") and os.environ.get("SAMOSA_OCR"),
                         "Set installed PDFium and OCR binary paths")
    def test_jobs_extracts_pdf_and_image_preview(self):
        extract = os.environ["SAMOSA_EXTRACT"]
        ocr = os.environ["SAMOSA_OCR"]
        for path, expected_source, expected_text in (
            (ROOT / "tests/fixtures/documents/hello.pdf", "pdf_text", None),
            (ROOT / "tools/testdata/ocr/tiny.png", "ocr", "2019"),
        ):
            info = path.stat()
            row = {"rel_path": path.name, "dev": info.st_dev,
                   "ino": info.st_ino, "size": info.st_size}
            root_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                preview, source = decision.excerpt(root_fd, row, extract, ocr, 100)
            finally:
                os.close(root_fd)
            self.assertEqual(source, expected_source, path.name)
            self.assertTrue(preview, path.name)
            self.assertLessEqual(len(preview), 100)
            if expected_text:
                self.assertIn(expected_text, preview)

    @unittest.skipUnless(os.environ.get("SAMOSA_DECISION_REAL_MODEL"),
                         "Set to check the cached DeBERTa model")
    def test_real_model_separates_veterinary_from_notice(self):
        model = decision.engine()
        request = "Can you find my cat's medical record? His name is Titli"
        self.assertEqual(decision.route(model, request)["action"], "find")
        items = [
            {"path": "veterinary_bill.pdf",
             "excerpt": "Pinnacle Veterinary Group patient Titli feline medical visit and treatment",
             "source": "pdf_text", "excerpt_chars": 100},
            {"path": "immigration_notice.pdf",
             "excerpt": "United States Citizenship and Immigration Services receipt and priority date",
             "source": "pdf_text", "excerpt_chars": 100},
        ]
        decision.score(model, VETERINARY_INTENT, items)
        self.assertEqual(items[0]["model_choice"], "plausible")
        self.assertEqual(items[1]["model_choice"], "unrelated")

    @unittest.skipUnless(os.environ.get("SAMOSA_DECISION_REAL_MODEL") and
                         os.environ.get("SAMOSA_EXTRACT") and os.environ.get("SAMOSA_OCR"),
                         "Set model, PDFium, and OCR paths for end-to-end check")
    def test_real_model_reads_direct_pdf_image_and_text(self):
        with tempfile.TemporaryDirectory(prefix="samosa-decision-real-") as temporary:
            root = Path(temporary)
            shutil.copyfile(ROOT / "tests/fixtures/documents/hello.pdf", root / "hello.pdf")
            shutil.copyfile(ROOT / "tools/testdata/ocr/tiny.png", root / "tiny.png")
            (root / "veterinary.txt").write_text(
                "Pinnacle Veterinary Group. Patient Titli, a cat. Medical visit record.",
                encoding="utf-8")
            model = decision.engine()
            result = decision.start(model, {"folder": str(root),
                                            "goal": "Find my cat Titli's medical record", "search_intent": VETERINARY_INTENT}, "",
                                    os.environ["SAMOSA_EXTRACT"], os.environ["SAMOSA_OCR"], None)
            by_path = {item["path"]: item for item in result["items"]}
            self.assertEqual(result["checked_files"], 3)
            self.assertEqual(by_path["hello.pdf"]["source"], "pdf_text")
            self.assertEqual(by_path["tiny.png"]["source"], "ocr")
            self.assertEqual(by_path["veterinary.txt"]["source"], "text")
            for item in by_path.values():
                self.assertTrue(item["excerpt"])
                self.assertIn(item["excerpt"], item["model_state"])
                self.assertIn(item["model_choice"], ("plausible", "unrelated"))



class MemoryDecisionContract(unittest.TestCase):
    def test_topic_proofs_use_only_literal_quote_and_requested_topic(self):
        model = mock.Mock()
        model.choice_fast.return_value = {'probabilities': {'plausible': .7}}
        request = {'question': 'Which documents concern botany?', 'topic': 'botany',
                   'verify_proofs': True,
                   'route': {'version': 1, 'scope': 'files', 'actions': ['inventory'],
                             'membership_kind': 'semantic'},
                   'sources': [{'number': 0, 'path': 'irrelevant-parent/notes.txt',
                                'preview': 'Plant growth and gardening observations.'}]}
        result = decision.memory_plan(model, request)
        call = model.choice_fast.call_args.kwargs
        self.assertEqual(call['state'], request['sources'][0]['preview'])
        self.assertEqual(call['criteria']['plausible'], 'This text is about botany.')
        self.assertEqual(result['sources'][0]['choice'], 'plausible')
        model.choice.assert_not_called()
        with self.assertRaises(decision.DecisionError):
            decision.memory_plan(model, {**request, 'topic': 'finance'})
        model.choice_fast.return_value = {'probabilities': {'plausible': float('nan')}}
        with self.assertRaises(decision.DecisionError):
            decision.memory_plan(model, request)

    def test_scope_then_actions_and_validated_source_stage(self):
        class TypedModel:
            def choice(self, **kwargs):
                if kwargs['criteria'] == decision.MEMORY_MEMBERSHIP:
                    return {'choice': 'semantic', 'probabilities': {'semantic': .9, 'named': .1}}
                return {'choice': 'files', 'probabilities': {'folder': .1, 'files': .9}}
            def noul(self, **kwargs):
                return {'noul': .9 if kwargs['instructions'] in (
                    decision.MEMORY_OPERATIONS['inventory'], decision.MEMORY_OPERATIONS['metadata']) else .1}
            def choice_fast(self, **kwargs):
                return {'choice': 'plausible', 'probabilities': {'plausible': .6, 'unrelated': .4}}
        model = TypedModel()
        route = decision.memory_plan(model, {'question': 'List lease documents and their page counts.', 'sources': []})
        self.assertEqual(route['scope'], 'files')
        self.assertEqual(route['actions'], ['inventory', 'metadata'])
        self.assertEqual(route['membership_kind'], 'semantic')
        with mock.patch.object(model, 'choice', side_effect=AssertionError('Parent routing must not repeat')):
            result = decision.memory_plan(model, {'question': 'List lease documents and their page counts.',
                'route': route, 'sources': [{'number': 0, 'path': 'lease.pdf', 'preview': 'Lease terms.'}]})
        self.assertEqual(result['sources'][0]['number'], 0)
        self.assertEqual(result['actions'], route['actions'])
        for malformed in ({**route, 'actions': ['delete']}, {**route, 'scope': 'outside_root'}):
            with self.assertRaises(decision.DecisionError):
                decision.memory_plan(model, {'question': 'Question', 'route': malformed, 'sources': []})
        with self.assertRaises(decision.DecisionError):
            decision.memory_plan(model, {'question': 'Question', 'route': route, 'sources': [], 'named_sources': [90]})

    def test_explicit_inventory_ids_do_not_require_filename_guessing(self):
        model = mock.Mock()
        route = {'version': 1, 'scope': 'files', 'actions': ['metadata']}
        result = decision.memory_plan(model, {'question': 'How long is this document?', 'route': route,
            'sources': [{'number': 0, 'path': 'report.pdf'}, {'number': 1, 'path': 'other.pdf'}], 'named_sources': [1]})
        self.assertEqual([s['choice'] for s in result['sources']], ['unrelated', 'plausible'])
        model.choice.assert_not_called()
        model.choice_fast.assert_not_called()

if __name__ == "__main__":
    unittest.main()
