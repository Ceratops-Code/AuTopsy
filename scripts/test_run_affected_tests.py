"""Behavior cases for impact selection and immutable, bounded result ownership."""
from __future__ import annotations

import io
import itertools
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import patch

import run_affected_tests as runner

sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
import test_fork_features as native


class SelectionTests(unittest.TestCase):
    @staticmethod
    def native_catalogue(executable: Path) -> list[dict[str, Any]]:
        names = set(runner.INSTALLER_BASELINE_TESTS)
        names.update("fork-" + case for cases, _ in runner.INSTALLER_NATIVE_GROUPS for case in cases)
        return [{"name": name, "command": [sys.executable, "native.py", "--executable", str(executable),
                    "--case", name.removeprefix("fork-")]} for name in sorted(names)]

    def test_installer_selection_uses_native_impact_and_rejects_missing_scope(self) -> None:
        catalogue = self.native_catalogue(Path("AuTopsy.exe"))
        baseline = set(runner.INSTALLER_BASELINE_TESTS)
        self.assertEqual(set(runner.select_installer_tests(["README.md"], catalogue)), baseline)
        expected = baseline | {"fork-preview-controls", "fork-preview-quality", "fork-preview-navigation",
                               "fork-video-cursor", "fork-video-transport"}
        self.assertEqual(set(runner.select_installer_tests(["src/video/ShowVideoPreview.cpp"], catalogue)), expected)
        self.assertEqual(set(runner.select_installer_tests(["scripts/build/windows/build_video_installer.py"], catalogue)), baseline)
        for paths, extra, unknown in ((["src/video/NewOperation.cpp"], None, False),
                                      ([], "does-not-exist", False), ([], None, True)):
            with self.subTest(paths=paths, extra=extra, unknown=unknown), self.assertRaises(RuntimeError):
                runner.select_installer_tests(paths, catalogue, extra, unknown)
        with self.assertRaisesRegex(RuntimeError, "absent"):
            runner.select_installer_tests([], [test for test in catalogue if test["name"] != "fork-preview"])
        self.assertIn("fork-video-crop", runner.select_installer_tests([], catalogue, "^fork-video-crop$", True))

    def test_prepackage_checks_keep_acceptance_across_interruption_and_block_changed_runtime(self) -> None:
        for target, boundary in itertools.product(("fork-preview", "fork-project-compatibility"),
                                                  ("before", "after", "changed payload")):
            with self.subTest(target=target, boundary=boundary), tempfile.TemporaryDirectory(prefix="_prepackage-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                root = Path(temporary)
                build = root / "build"
                runtime = build / "Release"
                runtime.mkdir(parents=True)
                (build / "_fork-tests").mkdir()
                executable = runtime / "AuTopsy.exe"
                executable.write_bytes(b"compiled host")
                catalogue = [test for test in self.native_catalogue(executable)
                             if test["name"] in runner.INSTALLER_BASELINE_TESTS]
                interrupted, fixture_calls = False, 0
                effects: list[str] = []

                def command(arguments, _root, catalogue=catalogue, target=target, boundary=boundary,
                            build=build, executable=executable, effects=effects):
                    nonlocal interrupted, fixture_calls
                    if "--show-only=json-v1" in arguments:
                        return json.dumps({"tests": catalogue})
                    name = next(test["name"] for test in catalogue if re.search(arguments[-1], test["name"]))
                    if name == "fork-fixtures":
                        fixture_calls += 1
                        return ""
                    if name == target and not interrupted and boundary == "before":
                        interrupted = True
                        raise InterruptedError("before native effect")
                    with redirect_stdout(io.StringIO()):
                        result = native.run_native_check(build / "_fork-tests", name.removeprefix("fork-"),
                            lambda: {"executable": runner.file_digest(executable)},
                            lambda _run, _inputs: effects.append(name) or {"result": "passed"})
                    if name == target and not interrupted:
                        interrupted = True
                        if boundary == "changed payload":
                            executable.write_bytes(b"different host")
                            return ""
                        raise InterruptedError("after native effect")
                    return "1: " + json.dumps(result) + "\n"

                with patch.object(runner, "run_command", side_effect=command), \
                        patch.object(runner, "changed_paths", return_value=([], "a" * 40)):
                    with self.assertRaises((InterruptedError, RuntimeError)):
                        runner.run_prepackage_checks(root, build, None, None)
                    executable.write_bytes(b"compiled host")
                    first = runner.run_prepackage_checks(root, build, None, None)
                    second = runner.run_prepackage_checks(root, build, None, None)
                self.assertEqual(first["payload"], second["payload"])
                self.assertEqual([item.get("acceptance") for item in first["checks"]],
                                 [item.get("acceptance") for item in second["checks"]])
                self.assertEqual(fixture_calls, 1)
                self.assertEqual(sorted(effects), ["fork-preview", "fork-project-compatibility"])
                self.assertFalse((build / "package").exists())

    def test_prepackage_consumes_nested_native_results_and_rejects_missing_or_conflicting_receipts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="_native-results-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            build = Path(temporary)
            work = build / "_fork-tests"
            work.mkdir()
            children = {}
            with redirect_stdout(io.StringIO()):
                for phase in ("application", "stock-audacity"):
                    children[phase] = native.run_native_check(work, "project-compatibility-" + phase,
                        lambda: {"work": "same host", "check": "same recipe"},
                        lambda _run, _inputs: {"result": "passed"})
            value = {"case": "project-compatibility", "result": "passed", **children}
            output = "1: " + json.dumps(value) + "\n"
            test = {"name": "fork-project-compatibility", "command": ["client", "--case", "project-compatibility"]}
            first = runner.read_native_acceptance(output, test, build)
            self.assertEqual(len(first["acceptance"]), 2)
            self.assertEqual(first, runner.read_native_acceptance(output, test, build))
            for failed_output in ("", "1: {malformed}\n", output + output):
                with self.subTest(output=failed_output), self.assertRaises(RuntimeError):
                    runner.read_native_acceptance(failed_output, test, build)
            children["application"]["original_run"] = "f" * 32
            with self.assertRaisesRegex(RuntimeError, "disagree"):
                runner.read_native_acceptance("1: " + json.dumps(value), test, build)

    def test_unpainted_track_keeps_original_state_when_optional_capture_fails(self) -> None:
        harness = sys.modules[native.painted_video_tracks.__module__]
        info = {"videoTrackThumbnails": False, "videoTrackRows": [{"dataWidth": 0}],
                "videoTrackPanelHandle": 123}
        client = SimpleNamespace(data=lambda command: info)
        for failure in (ModuleNotFoundError("PIL"), OSError("capture interrupted")):
            with self.subTest(failure=str(failure)), patch.dict(os.environ, {"AUTOPSY_BEHAVIOR_ROOT": "."}), \
                    patch.object(harness.time, "monotonic", side_effect=[0, 6]), \
                    patch.object(harness, "capture_owned_project", side_effect=failure) as capture:
                with self.assertRaises(AssertionError) as observed:
                    harness.painted_video_tracks(client)
                details = observed.exception.args[0]
                self.assertFalse(details["videoTrackThumbnails"])
                self.assertEqual(details["videoTrackRows"], info["videoTrackRows"])
                self.assertIn(str(failure), details["screenshot_error"])
                self.assertEqual(capture.call_count, 1)

    def test_project_capture_publishes_complete_png_and_keeps_last_picture_on_failure(self) -> None:
        harness = sys.modules[native.painted_video_tracks.__module__]
        encoded = b"\x89PNG\r\n\x1a\nowned picture"
        real_write, real_replace = Path.write_bytes, Path.replace
        for effect, after in itertools.product(("encode", "write", "publish"), (False, True)):
            with self.subTest(effect=effect, after=after), tempfile.TemporaryDirectory(prefix="_capture-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                picture = Path(temporary) / "_project.png"
                pending = picture.with_name("_" + picture.name + ".pending")
                picture.write_bytes(b"previous complete picture")

                def interrupt(operation, label, selected_effect=effect, selected_after=after):
                    def invoke(*args, **kwargs):
                        if selected_effect == label and not selected_after:
                            raise InterruptedError("before " + label)
                        result = operation(*args, **kwargs)
                        if selected_effect == label and selected_after:
                            raise InterruptedError("after " + label)
                        return result
                    return invoke

                with patch.object(harness, "run_process", side_effect=interrupt(lambda *a, **k: encoded, "encode")), \
                        patch.object(Path, "write_bytes", interrupt(real_write, "write")), \
                        patch.object(Path, "replace", interrupt(real_replace, "publish")):
                    with self.assertRaises(InterruptedError):
                        harness.write_project_capture("ffmpeg", picture, 2, 1, bytes(8))
                self.assertEqual(picture.read_bytes(), encoded if effect == "publish" and after else b"previous complete picture")
                self.assertFalse(pending.exists())
                pending.write_bytes(b"orphaned write")
                with patch.object(harness, "run_process", return_value=encoded) as encoder:
                    harness.write_project_capture("ffmpeg", picture, 2, 1, bytes(8))
                    timestamp = picture.stat().st_mtime_ns
                    harness.write_project_capture("ffmpeg", picture, 2, 1, bytes(8))
                self.assertEqual(picture.read_bytes(), encoded)
                self.assertEqual(picture.stat().st_mtime_ns, timestamp)
                self.assertEqual(encoder.call_args.kwargs["input"], bytes(8))
                self.assertFalse(pending.exists())
                for width, height, pixels in ((0, 1, b""), (2, 1, b"")):
                    with self.assertRaises(ValueError):
                        harness.write_project_capture("ffmpeg", picture, width, height, pixels)
                with patch.object(harness, "run_process", return_value=b"incomplete PNG"):
                    with self.assertRaisesRegex(RuntimeError, "no PNG picture"):
                        harness.write_project_capture("ffmpeg", picture, 2, 1, bytes(8))
                self.assertEqual(picture.read_bytes(), encoded)

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
        for path in ("src/tracks/ui/AffordanceHandle.cpp", "src/export/ExportAudioDialog.cpp"):
            groups, native = runner.select_groups([path])
            self.assertEqual(groups, [])
            self.assertEqual(native, [path])
        groups, native = runner.select_groups(["src/toolbars/ControlToolBar.cpp"])
        self.assertEqual(groups, [])
        self.assertEqual(native, ["src/toolbars/ControlToolBar.cpp"])
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

    def test_clock_ties_and_rollback_preserve_publication_order_across_reentry(self) -> None:
        root = self.root / "_results"
        finished = []
        for result in ("passed", "failed", "passed", "failed", "passed"):
            with runner.ReceiptStore(root) as store, patch.object(runner.time, "time_ns", return_value=10):
                record = store.publish("one", "current", result, {})
                finished.append(record["finished_ns"])
                # Deliberately reverse filesystem age; retention and selection
                # must both use the owner's publication order.
                path = root / runner.digest("one") / f"_result-{record['id']}.json"
                os.utime(path, ns=(1, 1))
                self.assertEqual(store.accepted("one", "current"), record if result == "passed" else None)
        with runner.ReceiptStore(root) as store, patch.object(runner.time, "time_ns", return_value=1):
            newest = store.publish("one", "current", "passed", {})
            self.assertGreater(newest["finished_ns"], finished[-1])
            self.assertEqual(store.accepted("one", "current"), newest)
            self.assertEqual(len(list((root / runner.digest("one")).glob("_result-*.json"))), 3)
        self.assertEqual(finished, sorted(set(finished)))

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
            self.assertIn("exited 7", result["failure"]["message"])
            evidence = json.loads(Path(result["failure"]["diagnostic_output"]).read_text())["evidence"]
            self.assertEqual(evidence["exit_status"], 7)
            self.assertEqual(evidence["stdout"], "out")
            self.assertEqual(evidence["stderr"], "err")
            self.assertIsNone(store.accepted(self.test["name"], "inputs"))

    def test_command_report_is_complete_compact_and_removed_after_success(self) -> None:
        root = self.root / "_results"
        stream = "unrelated output\n" * 1000 + "1/2 Test #12: fork-video-clip-mute ....***Failed 1 sec\nAssertionError: wrong picture"
        for _ in range(2):
            failure = runner.CommandFailure(["ctest", "--output-on-failure"],
                                            subprocess.CompletedProcess(["ctest"], 8, stream, "error stream"))
            with self.assertRaises(runner.CommandFailure):
                with runner.ReceiptStore(root):
                    raise failure
            self.assertLess(len(str(failure)), 700)
            self.assertIn("fork-video-clip-mute", str(failure))
            self.assertIn("wrong picture", str(failure))
            assert failure.diagnostic_output is not None
            saved = json.loads(Path(failure.diagnostic_output).read_text())
            self.assertEqual(saved["command"], ["ctest", "--output-on-failure"])
            self.assertEqual(saved["exit_status"], 8)
            self.assertEqual(saved["stdout"], stream)
            self.assertEqual(saved["stderr"], "error stream")
            self.assertFalse(list(root.glob("_pending-*.json")))
        with runner.ReceiptStore(root):
            pass
        self.assertFalse((root / "_command-failure.json").exists())

    def test_native_failure_refers_to_its_existing_report_without_copying_streams(self) -> None:
        native_build = self.root / "_native"
        native_build.mkdir()
        report = native_build / "_test_command_failure.json"
        child = {"argv": ["ctest"], "exit": 8, "stdout":
                 "1/1 Test #45: fork-video-clip-mute ...***Failed 1 sec\nAssertionError: pixel cue", "stderr": "<absent>"}
        report.write_text(json.dumps(child))
        failure = runner.CommandFailure(["ctest"], subprocess.CompletedProcess(
            ["ctest"], 8, "Windows installer check failed; details: " + str(report), "<absent>"))
        with patch.dict(os.environ, {"AUTOPSY_VIDEO_BUILD_DIR": str(native_build)}):
            with self.assertRaises(runner.CommandFailure):
                with runner.ReceiptStore(self.root / "_results"):
                    raise failure
        self.assertIn("fork-video-clip-mute", str(failure))
        self.assertIn("pixel cue", str(failure))
        assert failure.diagnostic_output is not None
        saved = json.loads(Path(failure.diagnostic_output).read_text())
        self.assertEqual(saved["child_diagnostic_output"], str(report))
        self.assertNotIn(child["stdout"], json.dumps(saved))
        self.assertEqual(json.loads(report.read_text()), child)

    def test_interrupted_command_report_releases_owner_and_cleans_staging(self) -> None:
        root = self.root / "_results"
        for after_effect in (False, True):
            failure = runner.CommandFailure(["ctest"], subprocess.CompletedProcess(["ctest"], 8, "out", "err"))
            replace = os.replace

            def interrupt(source, target, *, after=after_effect, publish=replace):
                if after:
                    publish(source, target)
                raise KeyboardInterrupt("report publication interrupted")

            with patch.object(runner.os, "replace", side_effect=interrupt):
                with self.assertRaises(KeyboardInterrupt):
                    with runner.ReceiptStore(root):
                        raise failure
            self.assertFalse(list(root.glob("_pending-*.json")))
            self.assertEqual((root / "_command-failure.json").exists(), after_effect)
            # A fresh public owner must acquire the lock and clean the obsolete
            # report, without any saved execution progress from the interruption.
            with runner.ReceiptStore(root):
                pass
            self.assertFalse((root / "_command-failure.json").exists())

    def test_cli_links_full_report_without_serializing_child_streams_again(self) -> None:
        output = "bulk evidence\n" * 1000 + "AssertionError: precise cause"
        failure = runner.CommandFailure(["ctest"], subprocess.CompletedProcess(["ctest"], 8, output, "stderr"))
        with self.assertRaises(runner.CommandFailure):
            with runner.ReceiptStore(self.root / "_results"):
                raise failure
        stream = io.StringIO()
        with patch.object(runner, "run_selected_tests", side_effect=failure), patch.object(runner.sys, "stderr", stream):
            self.assertEqual(runner.main([]), 1)
        result = json.loads(stream.getvalue())
        self.assertIn("precise cause", result["error"])
        self.assertLess(len(stream.getvalue()), 1000)
        self.assertNotIn("bulk evidence", stream.getvalue())
        self.assertEqual(json.loads(Path(result["diagnostic_output"]).read_text())["stdout"], output)

    def test_failed_report_write_preserves_original_cause_and_complete_cli_fallback(self) -> None:
        failure = runner.CommandFailure(["ctest"], subprocess.CompletedProcess(["ctest"], 8, "out", "err"))
        with patch.object(runner.os, "replace", side_effect=OSError("disk unavailable")):
            with self.assertRaises(runner.CommandFailure) as caught:
                with runner.ReceiptStore(self.root / "_results"):
                    raise failure
        self.assertIs(caught.exception, failure)
        self.assertIsNone(failure.diagnostic_output)
        self.assertIn("disk unavailable", failure.diagnostic_error or "")
        stream = io.StringIO()
        with patch.object(runner, "run_selected_tests", side_effect=failure), patch.object(runner.sys, "stderr", stream):
            self.assertEqual(runner.main([]), 1)
        fallback = json.loads(stream.getvalue())["command_failure"]
        self.assertEqual(fallback["stdout"], "out")
        self.assertEqual(fallback["stderr"], "err")
        self.assertEqual(fallback["exit_status"], 8)
        with runner.ReceiptStore(self.root / "_results"):
            pass

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
                "script_module_sha256": "module", "payload_dependencies": {"media.dll": "dependency"},
                "execution_scope": {"platform": "one", "test_display": "screen-two"}}

    def execute(self, run_id, inputs):
        return {"result": "passed", "version": "fixture",
                "observations": {"native_object_seen": True}}

    def test_native_buttons_use_their_own_click_protocol_without_replaying_effects(self):
        from types import SimpleNamespace
        from unittest.mock import Mock

        import test_video_clip_edits as edits
        for kind in ("Button", "wxWindowClassNR"):
            def class_name(_handle, buffer, _size, name=kind):
                buffer.value = name
                return len(name)
            def rectangle(_handle, pointer):
                pointer._obj.right, pointer._obj.bottom = 80, 40
                return True
            active = [42]
            def gui_state(_thread, pointer, active_window=active):
                pointer._obj.active = active_window[0]
                return True
            api = SimpleNamespace(GetClassNameW=Mock(side_effect=class_name),
                                  GetAncestor=Mock(return_value=42),
                                  GetForegroundWindow=Mock(return_value=99),
                                  GetWindowThreadProcessId=Mock(return_value=123),
                                  GetGUIThreadInfo=Mock(side_effect=gui_state),
                                  GetClientRect=Mock(side_effect=rectangle),
                                  SendMessageTimeoutW=Mock(return_value=True))
            window: Any = object.__new__(edits.PreviewNativeWindows)
            window.api, window.owner, window.enabled = api, lambda _h: 123, lambda _h: True
            with patch.dict(os.environ, {"AUTOPSY_OWNED_APP_PID": "123"}):
                for _ in range(2):
                    window.click(9)
            expected = [0xF5] * 2 if kind == "Button" else [0x201, 0x202, 0x202] * 2
            self.assertEqual([call.args[1] for call in api.SendMessageTimeoutW.call_args_list], expected)
            if kind == "Button":
                api.GetClientRect.assert_not_called()
                # An inactive dialog is an unmet input precondition. It may not
                # receive the click and then be retried as though it had failed.
                active[0] = 99
                api.SendMessageTimeoutW.reset_mock()
                def ready(condition, _description):
                    if not condition():
                        raise RuntimeError("inactive dialog")
                with patch.dict(os.environ, {"AUTOPSY_OWNED_APP_PID": "123"}), patch.object(edits, "wait_for_native_state", side_effect=ready):
                    with self.assertRaisesRegex(RuntimeError, "inactive dialog"):
                        window.click(9)
                api.SendMessageTimeoutW.assert_not_called()

    def test_clip_mute_check_edits_keep_the_unchanged_products_original_acceptance(self):
        # Exact basenames are the native runner contract, not executable fixtures.
        driver = self.root / "_test_driver.py"
        recipe = self.root / "test_video_clip_edits.py"
        executable, module = self.root / "_app.exe", self.root / "_script.dll"
        for path in (driver, recipe, executable, module, self.root / "run_fork_test_harness.py", self.root / "pipeclient.py"):
            path.write_bytes(b"first input")
        arguments = SimpleNamespace(case="video-clip-mute", ffmpeg=None, ffprobe=None)
        with patch.object(native, "__file__", str(driver)):
            def read():
                return native.native_test_inputs(arguments, executable, module, {"device": "screen-two"})
            with patch.object(self, "execute", wraps=self.execute) as app:
                first = native.run_native_check(self.root, arguments.case, read, self.execute)
                recipe.write_bytes(b"new complete-audio fixture recipe")
                changed = native.run_native_check(self.root, arguments.case, read, self.execute)
                again = native.run_native_check(self.root, arguments.case, read, self.execute)
            self.assertEqual(app.call_count, 1)
            self.assertEqual(first["original_run"], changed["original_run"])
            self.assertEqual(again["result"], "reused")
            self.assertEqual(again["original_run"], changed["original_run"])

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

    def test_preview_check_edits_preserve_the_original_check_version(self):
        driver = self.root / "_test_driver.py"
        recipe = self.root / "test_preview_controls.py"
        executable, module = self.root / "_app.exe", self.root / "_script.dll"
        for path in (driver, recipe, executable, module, self.root / "test_video_clip_edits.py", self.root / "test_clip_actions.py",
                     self.root / "run_fork_test_harness.py", self.root / "pipeclient.py"):
            path.write_bytes(b"original input")
        for case in ("preview-quality", "preview-controls", "video-cursor"):
            arguments = SimpleNamespace(case=case, ffmpeg=None, ffprobe=None)
            with self.subTest(case=case), patch.object(native, "__file__", str(driver)):
                def read(arguments=arguments):
                    return native.native_test_inputs(arguments, executable, module, {"device": "screen-two"})
                first = native.run_native_check(self.root, case, read, self.execute)
                original = Path(first["receipt"]).read_bytes()
                recipe.write_bytes(case.encode())
                with patch.object(self, "execute", wraps=self.execute) as app:
                    changed = native.run_native_check(self.root, case, read, self.execute)
                    again = native.run_native_check(self.root, case, read, self.execute)
                self.assertEqual(app.call_count, 0)
                self.assertEqual(first["original_run"], changed["original_run"])
                self.assertEqual(again["original_run"], changed["original_run"])
                self.assertEqual(Path(first["receipt"]).read_bytes(), original)

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

    def test_native_changed_artifact_or_environment_runs_only_its_affected_case(self):
        for field in ("executable_sha256", "script_module_sha256", "payload_dependencies", "execution_scope", "required_coverage"):
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

    def test_native_check_edit_does_not_hide_a_newer_failure_or_rewrite_a_pass(self):
        inputs = self.inputs()
        first = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        original = Path(first["receipt"]).read_bytes()
        inputs["check_sha256"] = "edited check"
        with patch.object(self, "execute", wraps=self.execute) as app:
            reused = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        self.assertEqual(app.call_count, 0)
        self.assertEqual(reused["original_run"], first["original_run"])
        self.assertEqual(Path(first["receipt"]).read_bytes(), original)
        failed = json.loads(original)
        failed.update(run=native.uuid.uuid4().hex, result="failed", check_inputs=inputs,
                      finished_ns=failed["finished_ns"] + 1)
        native.retain_behavior_result(self.root, failed)
        inputs["check_sha256"] = "another edited check"
        self.assertEqual(native.plan_native_check(self.root, "video", inputs)["decision"], "execute")
        with patch.object(self, "execute", wraps=self.execute) as app:
            repaired = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
            again = native.run_native_check(self.root, "video", lambda: inputs, self.execute)
        self.assertEqual(app.call_count, 1)
        self.assertEqual(again["original_run"], repaired["original_run"])

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
