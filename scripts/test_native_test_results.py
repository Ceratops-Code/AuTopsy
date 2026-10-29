"""Behavior cases for impact selection and immutable, bounded result ownership."""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
import run_native_test_sessions as sessions
import test_fork_features as native


class NativeReceiptTests(unittest.TestCase):
    root: Path
    def setUp(self) -> None:
        parent = os.environ.get("AUTOPSY_TEST_TEMP_ROOT")
        if not parent:
            self.fail("The owning CTest entry must supply AUTOPSY_TEST_TEMP_ROOT.")
        temporary = tempfile.TemporaryDirectory(prefix="_native-results-", dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

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

        import test_video_transport as edits
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
        for path in (driver, recipe, executable, module, self.root / "run_fork_test_harness.py", self.root / "pipeclient.py", self.root / "test_video_transport.py"):
            path.write_bytes(b"first input")
        arguments = SimpleNamespace(case="video-clip-mute", ffmpeg=None, ffprobe=None)
        with patch.object(native, "__file__", str(driver)):
            def read():
                return sessions.native_test_inputs(arguments, executable, module, {"device": "screen-two"}, native)
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
                     self.root / "run_fork_test_harness.py", self.root / "pipeclient.py", self.root / "test_video_transport.py"):
            path.write_bytes(b"original input")
        for case in ("preview-quality", "preview-controls", "video-cursor"):
            arguments = SimpleNamespace(case=case, ffmpeg=None, ffprobe=None)
            with self.subTest(case=case), patch.object(native, "__file__", str(driver)):
                def read(arguments=arguments):
                    return sessions.native_test_inputs(arguments, executable, module, {"device": "screen-two"}, native)
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
        first = sessions.native_test_inputs(args, executable, module, {"device": "second"}, native)
        dependency.write_bytes(b"changed")
        second = sessions.native_test_inputs(args, executable, module, {"device": "second"}, native)
        self.assertNotEqual(first, second)
        self.assertNotEqual(second, sessions.native_test_inputs(
            args, executable, module, {"device": "another"}, native))





if __name__ == "__main__":
    unittest.main()
