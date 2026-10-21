"""Behavior cases for impact selection and immutable, bounded result ownership."""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

import run_affected_tests as runner


class SelectionTests(unittest.TestCase):
    def names(self, paths: list[str], all_checks: bool = False) -> set[str]:
        return {group.name for group in runner.select_groups(paths, all_checks)[0]}

    def test_dedicated_edits_and_composition_have_separate_dependants(self) -> None:
        self.assertEqual(self.names(["src/video/EvaluateVideoTimeline.cpp"]), {"video-composition"})
        self.assertEqual(self.names(["src/video/PlanVideoEdits.h"]), {"video-edits", "video-composition"})
        self.assertEqual(self.names(["src/video/TestVideoEdits.cpp"]), {"video-edits"})

    def test_shared_headers_and_test_runtime_expand_only_declared_scope(self) -> None:
        self.assertEqual(self.names(["src/video/EditVideoTimeline.h"]),
                         {"video-playback", "video-edits", "video-composition"})
        self.assertEqual(self.names(["scripts/uv.lock"]), {group.name for group in runner.GROUPS})
        self.assertEqual(self.names(["README.md", "docs/design.md"]), set())

    def test_portable_consumers_have_a_dedicated_check(self) -> None:
        for name in ("prepare_audacity_consumer.py", "test_prepare_audacity_consumer.py"):
            self.assertEqual(self.names(["scripts/piped-work/" + name]), {"fork-audacity-consumer-cache"})

    def test_unknown_owned_executable_input_requires_a_scope_decision(self) -> None:
        for path in ("scripts/new_operation.py", "src/video/NewVideoOperation.cpp",
                     "src/video/ShowVideoNewFeature.h", "src/clip-groups/ChangeClipLinks.cpp",
                     "src/video/new-fixture.json"):
            with self.subTest(path=path):
                with self.assertRaisesRegex(RuntimeError, "No portable check owns"):
                    self.names([path])
        self.assertEqual(self.names(["scripts/new_operation.py"], True),
                         {group.name for group in runner.GROUPS})

    def test_native_impact_is_explicit_and_does_not_claim_desktop_acceptance(self) -> None:
        groups, native = runner.select_groups(["src/video/ShowVideoPreview.cpp"])
        self.assertEqual(groups, [])
        self.assertEqual(native, ["src/video/ShowVideoPreview.cpp"])
        groups, native = runner.select_groups(["src/tracks/ui/CommonTrackControls.cpp"])
        self.assertEqual(groups, [])
        self.assertEqual(native, ["src/tracks/ui/CommonTrackControls.cpp"])

    def test_renames_deletions_and_untracked_inputs_reach_selection(self) -> None:
        outputs = ["a" * 40 + "\n", "src/video/PlanVideoEdits.h\0old.py\0", "scripts/new_operation.py\0"]
        with patch.object(runner, "run_command", side_effect=outputs) as command:
            paths, comparison = runner.changed_paths(Path.cwd(), "HEAD~1")
        self.assertEqual(comparison, "a" * 40)
        self.assertIn("src/video/PlanVideoEdits.h", paths)
        self.assertIn("scripts/new_operation.py", paths)
        self.assertIn("--no-renames", command.call_args_list[1].args[0])

    def test_shallow_or_initial_base_selects_all_instead_of_guessing_no_change(self) -> None:
        failure = runner.CommandFailure(["git"], subprocess.CompletedProcess(["git"], 128, "", "missing"))
        with patch.object(runner, "run_command", side_effect=failure):
            self.assertTrue(runner.changed_paths(Path.cwd(), "unavailable")[1].startswith("all:"))
        self.assertTrue(runner.changed_paths(Path.cwd(), "0" * 40)[1].startswith("all:"))


class ReceiptTests(unittest.TestCase):
    def setUp(self) -> None:
        parent = os.environ.get("AUTOPSY_TEST_TEMP_ROOT")
        if not parent:
            self.fail("The owning CTest entry must supply AUTOPSY_TEST_TEMP_ROOT.")
        self.temporary = tempfile.TemporaryDirectory(prefix="_affected-tests-", dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.test: dict[str, Any] = {"name": "video-edits::one case", "command": ["test-executable", "one case"]}

    def test_matching_pass_retains_original_result_and_newer_failure_supersedes_it(self) -> None:
        with runner.ReceiptStore(self.root / "_results") as store:
            first = store.publish("one", "input-a", "passed", {})
            self.assertEqual(store.accepted("one", "input-a"), first)
            self.assertIsNone(store.accepted("one", "input-b"))
            store.publish("one", "input-a", "failed", {"stderr": "failed"})
            self.assertIsNone(store.accepted("one", "input-a"))
            third = store.publish("one", "input-a", "passed", {})
            self.assertEqual(store.accepted("one", "input-a"), third)

    def test_retention_is_finite_and_never_cleans_unrelated_files(self) -> None:
        root = self.root / "_results"
        root.mkdir()
        unrelated = root / "keep.txt"
        unrelated.write_text("unrelated")
        pending = root / "_pending-abandoned.json"
        pending.write_text("incomplete")
        with runner.ReceiptStore(root) as store:
            self.assertFalse(pending.exists())
            for identity in range(7):
                store.publish("one", str(identity), "passed", {})
            self.assertEqual(len(list((root / runner.digest("one")).glob("*.json"))), 3)
        self.assertTrue(unrelated.exists())
        self.assertEqual((root / "_runner.lock").stat().st_size, 1)

    def test_interrupted_owner_leaves_a_reusable_lock_file_without_saved_progress(self) -> None:
        root = self.root / "_results"
        root.mkdir()
        (root / "_runner.lock").touch()  # Death immediately after file creation.
        with runner.ReceiptStore(root) as store:
            record = store.publish("one", "current", "passed", {})
        with runner.ReceiptStore(root) as fresh:
            self.assertEqual(fresh.accepted("one", "current"), record)

    def test_live_runner_blocks_competing_owner_without_removing_its_files(self) -> None:
        with runner.ReceiptStore(self.root / "_results") as first:
            with self.assertRaisesRegex(RuntimeError, "Another affected-test runner"):
                with runner.ReceiptStore(first.root):
                    self.fail("Competing owner entered")
            self.assertTrue(first.lock.exists())

    def test_input_changes_and_execution_scope_get_distinct_identities(self) -> None:
        source = self.root / "input.txt"
        source.write_text("current input")
        group = runner.CheckGroup("one", ("input.txt",))
        identity = runner.check_input_identity(self.root, group, self.test, {"platform": "one"})
        self.assertEqual(identity, runner.check_input_identity(self.root, group, self.test, {"platform": "one"}))
        source.write_text("changed input")
        self.assertNotEqual(identity, runner.check_input_identity(self.root, group, self.test, {"platform": "one"}))
        self.assertNotEqual(identity, runner.check_input_identity(self.root, group, self.test, {"platform": "two"}))
        source.unlink()
        self.assertNotEqual(identity, runner.check_input_identity(self.root, group, self.test, {"platform": "one"}))

    def test_exact_named_ctest_pass_is_reused_without_a_second_child(self) -> None:
        with runner.ReceiptStore(self.root / "_results") as store:
            with patch.object(runner, "run_command", return_value="passed") as command:
                first = runner.run_named_check(self.root, self.root, self.test, "inputs", store)
                second = runner.run_named_check(self.root, self.root, self.test, "inputs", store)
        self.assertEqual(first["result"], "passed")
        self.assertEqual(second["result"], "reused")
        self.assertEqual(first["receipt"], second["receipt"])
        self.assertEqual(command.call_count, 1)
        self.assertEqual(command.call_args.args[0][-1], "^video\\-edits::one\\ case$")

    def test_failed_child_keeps_both_streams_and_cannot_reuse_an_older_pass(self) -> None:
        failed = runner.CommandFailure(["ctest", "-R", "one"],
                                       subprocess.CompletedProcess(["ctest"], 7, "out", "err"))
        with runner.ReceiptStore(self.root / "_results") as store:
            with patch.object(runner, "run_command", side_effect=failed):
                result = runner.run_named_check(self.root, self.root, self.test, "inputs", store)
            self.assertEqual(result["result"], "failed")
            self.assertEqual(result["failure"]["exit_status"], 7)
            self.assertEqual(result["failure"]["stdout"], "out")
            self.assertEqual(result["failure"]["stderr"], "err")
            self.assertIsNone(store.accepted(self.test["name"], "inputs"))

    def test_interruptions_around_child_and_receipt_use_fresh_authoritative_state(self) -> None:
        # Before the child: no effect or acceptance. After the child but before
        # receipt publication: no completed acceptance exists. After publication:
        # a fresh call must reuse the original accepted result, never rerun it.
        for boundary in ("before-child", "before-receipt", "after-receipt"):
            with self.subTest(boundary=boundary):
                with runner.ReceiptStore(self.root / ("_results-" + boundary)) as store:
                    publish = store.publish

                    def interrupted_publish(name: str, identity: str, result: str,
                                            evidence: dict[str, object], *, stage: str = boundary,
                                            publisher=publish) -> object:
                        if stage == "before-receipt":
                            raise InterruptedError("before publication")
                        record = publisher(name, identity, result, evidence)
                        raise InterruptedError(str(record["id"]))

                    with patch.object(runner, "run_command", side_effect=(
                        InterruptedError("before child") if boundary == "before-child" else None)):
                        with patch.object(store, "publish", side_effect=interrupted_publish):
                            with self.assertRaises(InterruptedError):
                                runner.run_named_check(self.root, self.root, self.test, "inputs", store)
                    with patch.object(runner, "run_command", return_value="passed") as fresh_child:
                        result = runner.run_named_check(self.root, self.root, self.test, "inputs", store)
                    self.assertEqual(result["result"], "reused" if boundary == "after-receipt" else "passed")
                    self.assertEqual(fresh_child.call_count, 0 if boundary == "after-receipt" else 1)

    def test_atomic_result_replacement_cleans_pending_output_after_interruption(self) -> None:
        with runner.ReceiptStore(self.root / "_results") as store:
            for after_effect in (False, True):
                real_replace = os.replace

                def interrupted_replace(source: Path, target: Path, *, after: bool = after_effect,
                                        replace=real_replace) -> None:
                    if after:
                        replace(source, target)
                    raise InterruptedError("publication interrupted")

                with patch.object(runner.os, "replace", side_effect=interrupted_replace):
                    with self.assertRaises(InterruptedError):
                        store.publish("one", str(after_effect), "passed", {})
                self.assertFalse(list(store.root.glob("_pending-*.json")))
                accepted = store.accepted("one", str(after_effect))
                self.assertEqual(accepted is not None, after_effect)
                if accepted:
                    self.assertEqual(json.loads((store.root / runner.digest("one") /
                                                 f"_result-{accepted['id']}.json").read_text())["result"], "passed")

    def test_input_race_before_or_during_check_cannot_publish_false_acceptance(self) -> None:
        with runner.ReceiptStore(self.root / "_results") as store:
            with patch.object(runner, "run_command") as command:
                with self.assertRaisesRegex(RuntimeError, "Inputs changed after compilation"):
                    runner.run_named_check(self.root, self.root, self.test, "inputs", store, lambda: False)
                command.assert_not_called()
            states = iter([True, False])
            with patch.object(runner, "run_command", return_value="passed"):
                result = runner.run_named_check(self.root, self.root, self.test, "inputs", store, lambda: next(states))
            self.assertEqual(result["result"], "failed")
            self.assertIsNone(store.accepted(self.test["name"], "inputs"))

    def test_missing_child_runtime_keeps_invocation_and_absent_stream_evidence(self) -> None:
        with patch.object(runner.subprocess, "run", side_effect=FileNotFoundError("missing runtime")):
            with self.assertRaises(runner.CommandFailure) as caught:
                runner.run_command(["missing-command", "argument"], self.root)
        self.assertEqual(caught.exception.details["command"], ["missing-command", "argument"])
        self.assertEqual(caught.exception.details["stdout"], "<absent>")
        self.assertIn("missing runtime", caught.exception.details["stderr"])


if __name__ == "__main__":
    unittest.main()
