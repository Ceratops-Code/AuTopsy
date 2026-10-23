"""Behavior cases for impact selection and immutable, bounded result ownership."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

import run_affected_tests as runner

sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
import test_fork_features as native


class SelectionTests(unittest.TestCase):
    def test_renumbered_displays_never_select_the_primary_screen(self) -> None:
        primary = {"device": r"\\.\DISPLAY10", "primary": True, "work": [0, 0, 1280, 672]}
        secondary = {"device": r"\\.\DISPLAY11", "primary": False, "work": [-1280, 113, 0, 785]}
        for layout in ([primary, secondary], [secondary, primary]):
            self.assertEqual(native.select_test_display(layout), secondary)
        with self.assertRaises(RuntimeError):
            native.select_test_display([primary, dict(secondary, primary=True)])

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

    def test_runner_changes_select_its_own_tests_without_retesting_unchanged_models(self) -> None:
        self.assertEqual(self.names(["scripts/run_affected_tests.py"]), {"affected-test-selection"})

    def test_portable_consumers_have_a_dedicated_check(self) -> None:
        for name in ("prepare_audacity_consumer.py", "test_prepare_audacity_consumer.py"):
            self.assertEqual(self.names(["scripts/piped-work/" + name]), {"fork-audacity-consumer-cache"})

    def test_consumer_runtime_has_its_own_lifecycle_check(self) -> None:
        for name in ("run_audacity_consumer.py", "test_run_audacity_consumer.py"):
            self.assertEqual(self.names(["scripts/piped-work/" + name]), {"fork-audacity-consumer-runtime"})

    def test_storage_policy_and_native_adapters_have_separate_scopes(self) -> None:
        for path in ("libraries/lib-project-file-io/ConfigureProjectStorage.cpp",
                     "libraries/lib-project-file-io/tests/TestProjectStorage.cpp",
                     "lib-src/sqlite/sqlite3.c"):
            self.assertEqual(self.names([path]), {"project-storage"})
        groups, native_inputs = runner.select_groups(["libraries/lib-project-file-io/DBConnection.cpp"])
        self.assertEqual(groups, [])
        self.assertEqual(native_inputs, ["libraries/lib-project-file-io/DBConnection.cpp"])

    def test_track_folder_policy_and_native_adapters_have_separate_scopes(self) -> None:
        for path in ("src/track-groups/EditTrackGroups.cpp", "src/track-groups/RenderGroupWaveform.h",
                     "src/track-groups/TestTrackGroups.cpp"):
            self.assertEqual(self.names([path]), {"track-groups"})
        for path in ("src/track-groups/CreateTrackGroup.cpp", "src/track-groups/ShowTrackGroups.cpp"):
            groups, native = runner.select_groups([path])
            self.assertEqual(groups, [])
            self.assertEqual(native, [path])

    def test_cursor_recording_policy_and_clip_adapters_have_separate_scopes(self) -> None:
        for path in ("src/recording/PlanCursorRecording.cpp", "src/recording/TestCursorRecording.cpp"):
            self.assertEqual(self.names([path]), {"cursor-recording"})
        for path in ("src/clip-groups/OpenClipActions.cpp", "src/ProjectAudioManager.cpp",
                     "src/tracks/ui/EditCursorOverlay.cpp", "scripts/piped-work/test_clip_actions.py"):
            groups, native = runner.select_groups([path])
            self.assertEqual(groups, [])
            self.assertEqual(native, [path])

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



class NativeReceiptTests(unittest.TestCase):
    root: Path
    setUp = ReceiptTests.setUp

    def inputs(self) -> dict[str, Any]:
        return {"executable_sha256": "app", "check_sha256": "check",
                "execution_scope": {"platform": "one", "test_display": "screen-two"}}

    def execute(self, run_id, inputs):
        return {"result": "passed", "version": "fixture",
                "observations": {"native_object_seen": True}}

    def test_native_repeat_reuses_original_result_without_starting_another_app(self):
        inputs = self.inputs()
        with patch.object(self, "execute", wraps=self.execute) as app:
            first = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
            original = Path(first["receipt"]).read_bytes()
            for _ in range(3):
                again = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
                self.assertEqual(again["result"], "reused")
                self.assertEqual(again["original_run"], first["original_run"])
            self.assertEqual(app.call_count, 1)
        self.assertEqual(Path(first["receipt"]).read_bytes(), original)
        self.assertFalse(list((self.root / "_accepted").glob("_pending-*")))

    def test_native_newer_matching_failure_blocks_pass_until_a_later_matching_success(self):
        inputs = self.inputs()
        first = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        failed = json.loads(Path(first["receipt"]).read_text())
        failed.update(run=native.uuid.uuid4().hex, result="failed",
                      finished_ns=failed["finished_ns"] + 1)
        native.retain_behavior_result(self.root, failed)
        self.assertEqual(native.plan_native_check(self.root, "video", inputs)["decision"], "execute")
        with patch.object(self, "execute", wraps=self.execute) as app:
            repaired = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
            reused = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        self.assertEqual(app.call_count, 1)
        self.assertEqual(reused["original_run"], repaired["original_run"])

    def test_native_changed_artifact_check_or_environment_runs_only_its_affected_case(self):
        for field in ("executable_sha256", "check_sha256", "execution_scope"):
            with self.subTest(field=field):
                root = self.root / field
                root.mkdir()
                inputs = self.inputs()
                first = native.run_native_check(root, "video", lambda inputs=inputs: inputs, self.execute)
                inputs[field] = "changed"
                with patch.object(self, "execute", wraps=self.execute) as app:
                    changed = native.run_native_check(root, "video", lambda inputs=inputs: inputs, self.execute)
                    again = native.run_native_check(root, "video", lambda inputs=inputs: inputs, self.execute)
                self.assertEqual(app.call_count, 1)
                self.assertNotEqual(first["original_run"], changed["original_run"])
                self.assertEqual(changed["original_run"], again["original_run"])

    def test_native_legacy_acceptance_keeps_original_bytes_without_inventing_scope(self):
        record: dict[str, Any] = {"run": native.uuid.uuid4().hex, "case": "video", "result": "passed",
                  "input": {"source": "generated:video"}, "executable_sha256": "app"}
        native.retain_behavior_result(self.root, record)
        path = self.root / "_accepted" / ("_" + record["run"] + ".json")
        original = path.read_bytes()
        self.assertEqual(native.plan_native_check(self.root, "video", self.inputs())["decision"], "execute")
        result = native.run_native_check(self.root, "video", self.inputs, self.execute)
        self.assertEqual(result["result"], "passed")
        self.assertEqual(path.read_bytes(), original)

    def test_native_interruptions_use_fresh_inputs_and_reuse_only_completed_publication(self):
        stages = ("before-intent", "after-intent", "before-operation", "after-operation",
                  "before-publication", "after-publication", "before-cleanup", "after-cleanup")
        for stage in stages:
            with self.subTest(stage=stage):
                root = self.root / stage
                root.mkdir()
                interrupted = False
                effects: list[str] = []
                write = native.write_behavior_record
                unlink = Path.unlink

                def write_at_boundary(path, payload, *, stage=stage, write=write):
                    nonlocal interrupted
                    intent = path.name.startswith("_pending-")
                    selected = ("intent" if intent else "publication")
                    if not interrupted and stage == "before-" + selected:
                        interrupted = True
                        raise InterruptedError(stage)
                    write(path, payload)
                    if not interrupted and stage == "after-" + selected:
                        interrupted = True
                        raise InterruptedError(stage)

                def unlink_at_boundary(path, *args, stage=stage, unlink=unlink, **kwargs):
                    nonlocal interrupted
                    selected = path.name.startswith("_pending-") and path.suffix == ".json"
                    if selected and not interrupted and stage == "before-cleanup":
                        interrupted = True
                        raise InterruptedError(stage)
                    unlink(path, *args, **kwargs)
                    if selected and not interrupted and stage == "after-cleanup":
                        interrupted = True
                        raise InterruptedError(stage)

                def operation(run_id, inputs, *, stage=stage, effects=effects):
                    nonlocal interrupted
                    if stage == "before-operation" and not interrupted:
                        interrupted = True
                        raise InterruptedError(stage)
                    effects.append(run_id)
                    if stage == "after-operation" and not interrupted:
                        interrupted = True
                        raise InterruptedError(stage)
                    return self.execute(run_id, inputs)

                with patch.object(native, "write_behavior_record", side_effect=write_at_boundary), \
                        patch.object(Path, "unlink", new=unlink_at_boundary):
                    with self.assertRaises(InterruptedError):
                        native.run_native_check(root, "video", self.inputs, operation)
                before = len(effects)
                fresh = native.run_native_check(root, "video", self.inputs, operation)
                published = stage in ("after-publication", "before-cleanup", "after-cleanup")
                self.assertEqual(fresh["result"], "reused" if published else "passed")
                self.assertEqual(len(effects) - before, 0 if published else 1)
                self.assertFalse(list((root / "_accepted").glob("_pending-*")))
                self.assertFalse(list((root / "_accepted").glob("*.tmp")))

    def test_native_observations_cannot_change_the_owned_input_snapshot(self):
        inputs = self.inputs()

        def operation(run_id, snapshot):
            snapshot["execution_scope"]["platform"] = "altered observation"
            result = self.execute(run_id, snapshot)
            result["check_inputs"] = snapshot
            return result

        first = native.run_native_check(self.root, "video", lambda: inputs, operation)
        saved = json.loads(Path(first["receipt"]).read_text())
        self.assertEqual(saved["check_inputs"], inputs)
        self.assertEqual(saved["execution_scope"], inputs["execution_scope"])
        with patch.object(self, "execute") as app:
            again = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        app.assert_not_called()
        self.assertEqual(again["original_run"], first["original_run"])

    def test_native_input_change_during_operation_cannot_publish_a_pass(self):
        inputs = self.inputs()

        def change_inputs(run_id, original):
            inputs["check_sha256"] = "changed"
            return self.execute(run_id, original)

        with self.assertRaisesRegex(RuntimeError, "inputs changed"):
            native.run_native_check(self.root, "video", lambda: dict(inputs), change_inputs)
        rows = native.read_behavior_results(self.root)
        self.assertEqual([row[2]["result"] for row in rows], ["failed"])
        result = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        self.assertEqual(result["result"], "passed")

    def test_native_retention_keeps_blocking_failure_and_leaves_unrelated_outputs(self):
        inputs = self.inputs()
        first = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        archive = self.root / "_accepted"
        unrelated = archive / "keep.txt"
        unrelated.write_text("unrelated")
        failure = json.loads(Path(first["receipt"]).read_text())
        failure.update(run=native.uuid.uuid4().hex, result="failed",
                       finished_ns=failure["finished_ns"] + 1)
        native.retain_behavior_result(self.root, failure)
        for index in range(12):
            other = dict(failure, run=native.uuid.uuid4().hex,
                         input_identity="other-" + str(index), finished_ns=native.time.time_ns())
            native.retain_behavior_result(self.root, other)
        self.assertEqual(native.plan_native_check(self.root, "video", inputs)["decision"], "execute")
        self.assertLessEqual(len(native.read_behavior_results(self.root)), 8)
        self.assertTrue(unrelated.is_file())

    def test_native_pruning_converges_after_interruption_around_each_removal(self):
        for after_effect in (False, True):
            with self.subTest(after_effect=after_effect):
                root = self.root / str(after_effect)
                root.mkdir()
                native.retain_behavior_result(root)
                for index in range(5):
                    record: dict[str, Any] = {"run": native.uuid.uuid4().hex, "case": "video",
                              "result": "passed", "finished_ns": index}
                    native.write_behavior_record(
                        root / "_accepted" / ("_" + record["run"] + ".json"), record)
                remove = Path.unlink
                interrupted = False

                def remove_at_boundary(path, *args, after=after_effect, remove=remove, **kwargs):
                    nonlocal interrupted
                    if path.suffix == ".json" and not interrupted:
                        interrupted = True
                        if after:
                            remove(path, *args, **kwargs)
                        raise InterruptedError("result pruning")
                    remove(path, *args, **kwargs)

                with patch.object(Path, "unlink", new=remove_at_boundary):
                    with self.assertRaises(InterruptedError):
                        native.retain_behavior_result(root)
                native.retain_behavior_result(root)
                original = {row[1].name: row[1].read_bytes() for row in native.read_behavior_results(root)}
                self.assertEqual(len(original), 3)
                native.retain_behavior_result(root)
                self.assertEqual(original, {row[1].name: row[1].read_bytes()
                                            for row in native.read_behavior_results(root)})

    def test_native_incomplete_outcome_survives_interrupted_startup_publication(self):
        for after_effect in (False, True):
            with self.subTest(after_effect=after_effect):
                root = self.root / str(after_effect)
                root.mkdir()
                native.retain_behavior_result(root)
                run_id = native.uuid.uuid4().hex
                plan = native.plan_native_check(root, "video", self.inputs())
                pending = root / "_accepted" / ("_pending-" + run_id + ".json")
                native.write_behavior_record(
                    pending, {"run": run_id, "case": "video", "result": "incomplete",
                              "started_ns": native.time.time_ns(), "input_identity": plan["input_identity"]})
                write = native.write_behavior_record

                def publish_at_boundary(path, payload, *, after=after_effect, write=write):
                    if after:
                        write(path, payload)
                    raise InterruptedError("incomplete outcome publication")

                with patch.object(native, "write_behavior_record", side_effect=publish_at_boundary):
                    with self.assertRaises(InterruptedError):
                        native.finish_interrupted_native_checks(root)
                with patch.object(self, "execute", wraps=self.execute) as app:
                    result = native.run_native_check(root, "video", self.inputs, self.execute)
                    native.run_native_check(root, "video", self.inputs, self.execute)
                self.assertEqual(result["result"], "passed")
                self.assertEqual(app.call_count, 1)
                self.assertFalse(pending.exists())

    def test_native_dependency_bytes_and_display_scope_affect_selection(self):
        executable = self.root / "app.exe"
        executable.write_bytes(b"app")
        module = self.root / "script.dll"
        module.write_bytes(b"module")
        dependency = self.root / "media.dll"
        dependency.write_bytes(b"first")
        args = native.argparse.Namespace(case="video", ffmpeg="", ffprobe="")
        first = native.native_test_inputs(args, executable, module, {"device": "second"})
        dependency.write_bytes(b"changed")
        second = native.native_test_inputs(args, executable, module, {"device": "second"})
        self.assertNotEqual(first, second)
        self.assertNotEqual(second, native.native_test_inputs(
            args, executable, module, {"device": "another"}))




if __name__ == "__main__":
    unittest.main()
