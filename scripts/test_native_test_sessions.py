"""Focused process reuse, empty-project, fixture and interruption contracts."""
from __future__ import annotations

import ctypes
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import Mock, patch

import run_affected_tests as runner

sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
import prepare_test_media as media
import run_native_test_sessions as sessions
import test_fork_features as native


class FakeSession:
    """Expose process boundaries without claiming desktop behavior."""

    def __init__(self, root, executable):
        self.app = None
        self.closed = []

    def close(self, *, force=False):
        if self.app is not None:
            self.closed.append(force)
            self.app = None


class OwnedTestRoot(unittest.TestCase):
    def setUp(self):
        temporary = os.environ.get("AUTOPSY_TEST_TEMP_ROOT")
        if not temporary:
            raise RuntimeError("SDLC must provide AUTOPSY_TEST_TEMP_ROOT.")
        self.directory = tempfile.TemporaryDirectory(prefix="_native-sessions-", dir=temporary)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)


class NativeSessionTests(OwnedTestRoot):
    def args(self, cases):
        return SimpleNamespace(cases=cases, case=cases[0], executable=self.root / "AuTopsy.exe",
                               ffmpeg="ffmpeg", test_display={"device": "screen 2"})

    @staticmethod
    def inputs(_args):
        return {"executable_sha256": "app", "script_module_sha256": "module",
                "payload_dependencies": {}, "execution_scope": {"device": "screen 2"}}

    def test_plan_groups_project_cases_and_keeps_cold_crash_capture_and_multi_project_isolated(self):
        cases = ["branding", "video-selection", "video-caption-english", "video-edges",
                 "cursor-recording-save-after", "video-audio-parity", "preview-quality"]
        for _ in range(2):
            self.assertEqual(sessions.session_case_order(cases),
                ["video-selection", "video-edges", "branding", "video-caption-english",
                 "cursor-recording-save-after", "video-audio-parity", "preview-quality"])
        for cases in ([], ["video-selection"] * 2, ["fixtures"], ["unknown"]):
            with self.subTest(cases=cases), self.assertRaises(ValueError):
                sessions.session_case_order(cases)

    def test_shared_cases_keep_independent_receipts_and_do_not_restart_accepted_cases(self):
        owners, effects = [], []

        def create(root, executable):
            owner = FakeSession(root, executable)
            owners.append(owner)
            return owner

        def execute(args, _root, _run, _inputs, owner):
            if owner is not None:
                owner.app = owner.app or object()
            effects.append((args.case, owner, owner.app if owner else None))
            return {"result": "passed"}

        args = self.args(["video-selection", "branding", "video-edges"])
        with redirect_stdout(io.StringIO()):
            self.assertEqual(sessions.run_case_batch(args, self.root, native, self.inputs, execute, create), 0)
            originals = {record[2]["case"]: record[2]["run"] for record in native.read_behavior_results(self.root)}
            self.assertEqual(sessions.run_case_batch(args, self.root, native, self.inputs, execute, create), 0)
        self.assertEqual([row[0] for row in effects], ["video-selection", "video-edges", "branding"])
        self.assertIs(effects[0][2], effects[1][2])
        self.assertIsNone(effects[2][1])
        self.assertEqual(owners[0].closed, [False])
        self.assertEqual(originals, {row[2]["case"]: row[2]["run"] for row in native.read_behavior_results(self.root)})

    def test_failure_before_or_after_worker_effect_retires_session_and_preserves_other_passes(self):
        for boundary in ("before", "after"):
            with self.subTest(boundary=boundary):
                work = self.root / boundary
                owners: list[FakeSession] = []
                effects: list[tuple[str, object]] = []
                state = {"failing": True}

                def create(root, executable, owners=owners):
                    owner = FakeSession(root, executable)
                    owners.append(owner)
                    return owner

                def execute(args, _root, _run, _inputs, owner, state=state, boundary=boundary, effects=effects):
                    if args.case == "video-edges" and state["failing"] and boundary == "before":
                        raise RuntimeError("reset failed before worker effect")
                    owner.app = owner.app or object()
                    effects.append((args.case, owner.app))
                    if args.case == "video-edges" and state["failing"]:
                        raise RuntimeError("worker crashed after effect")
                    return {"result": "passed"}

                args = self.args(["video-selection", "video-edges", "video-split-edges"])
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(sessions.run_case_batch(args, work, native, self.inputs, execute, create), 1)
                    original = next(row[2]["run"] for row in native.read_behavior_results(work)
                                    if row[2]["case"] == "video-selection")
                    state["failing"] = False
                    self.assertEqual(sessions.run_case_batch(args, work, native, self.inputs, execute, create), 0)
                self.assertIsNot(effects[0][1], next(row[1] for row in effects if row[0] == "video-split-edges"))
                self.assertEqual(sum(name == "video-selection" for name, _ in effects), 1)
                self.assertEqual(sum(name == "video-split-edges" for name, _ in effects), 1)
                self.assertEqual(next(row[2]["run"] for row in native.read_behavior_results(work)
                                      if row[2]["case"] == "video-selection"), original)
                self.assertIn(True, owners[0].closed)

    def test_user_interruption_stops_batch_and_closes_its_owned_process(self):
        owner = FakeSession(None, None)

        def interrupted(*_args):
            owner.app = object()
            raise KeyboardInterrupt

        with redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
            sessions.run_case_batch(self.args(["video-selection", "video-edges"]), self.root,
                                   native, self.inputs, interrupted, lambda *_: owner)
        self.assertEqual(owner.closed, [True])
        self.assertEqual([row[2]["case"] for row in native.read_behavior_results(self.root)], ["video-selection"])

    def test_reset_requires_new_empty_history_one_project_and_known_preferences(self):
        def reply(command, success=True):
            if command.startswith("GetPreference"):
                return ("No" if "MusicFile" in command else "0") + "\nBatchCommand finished: OK"
            return "BatchCommand finished: OK"

        client = SimpleNamespace(send=Mock(side_effect=reply), data=Mock(return_value=[]))
        with patch.dict(os.environ, AUTOPSY_OWNED_APP_PID="42"), \
                patch.object(sessions, "owned_project_windows", return_value=[11]), \
                patch.object(sessions, "read_history_availability", return_value={"Undo": False, "Redo": False}) as history:
            for _ in range(2):
                sessions.require_empty_project(client)
            self.assertFalse(any(call.args[0] == "Undo" for call in client.send.call_args_list))
            for state in ({"Undo": True, "Redo": False}, {"Undo": False, "Redo": True}, {}):
                history.return_value = state
                with self.assertRaisesRegex(RuntimeError, "still has history"):
                    sessions.require_empty_project(client)
            history.return_value = {"Undo": False, "Redo": False}
            for kind in ("Tracks", "Clips", "Labels"):
                client.data.side_effect = lambda command, kind=kind: ["leftover"] if kind in command else []
                with self.assertRaisesRegex(RuntimeError, "still contains"):
                    sessions.require_empty_project(client)
            client.data.side_effect = None
            client.send.side_effect = lambda *_args, **_kw: "wrong value"
            with self.assertRaisesRegex(RuntimeError, "preference"):
                sessions.require_empty_project(client)
        with patch.dict(os.environ, AUTOPSY_OWNED_APP_PID="42"), \
                patch.object(sessions, "owned_project_windows", return_value=[11, 12]), \
                self.assertRaisesRegex(RuntimeError, "exactly one"):
            sessions.require_empty_project(client)

    def test_worker_closes_its_project_and_checks_reset_without_exiting_shared_app(self):
        client = Mock()
        with patch.object(native, "ScriptClient", return_value=client), \
                patch.object(native, "check_video_interactions"), \
                patch.object(sessions, "require_empty_project") as reset:
            native.worker(SimpleNamespace(case="video-selection", keep_app_open=True), self.root)
        self.assertEqual(reset.call_count, 2)
        client.send.assert_called_once_with("Close")

    def test_log_tail_is_bounded_and_retirement_repeats_without_stopping_another_process(self):
        app = SimpleNamespace(stdout=io.BytesIO(b"x" * (3 * 1024 * 1024)),
                              poll=Mock(return_value=None), terminate=Mock(), wait=Mock(return_value=0))
        owner = sessions.SharedNativeProcess(self.root, self.root / "app.exe")
        with owner.output(self.root):
            owner.bind(app, {}, None)
            assert owner.log_thread is not None
            owner.log_thread.join(timeout=5)
            owner.close(force=True)
            owner.close(force=True)
        self.assertEqual((self.root / "_app.log").stat().st_size, 2 * 1024 * 1024)
        app.terminate.assert_called_once()
        self.assertIsNone(owner.app)

    @unittest.skipUnless(os.name == "nt", "Windows job containment")
    def test_session_retirement_stops_its_media_child_and_keeps_the_controller_alive(self):
        script = ("import sys,subprocess;sys.path.insert(0,sys.argv[1]);"
                  "import run_native_test_sessions as s;s.contain_native_children();"
                  "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(90)']);"
                  "s.stop_owned_native_children();s.stop_owned_native_children();"
                  "assert p.wait(timeout=5)==1;print('controller-alive')")
        for _ in range(2):
            result = subprocess.run([sys.executable, "-c", script, str(Path(sessions.__file__).parent)],
                                    capture_output=True, text=True, check=False, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "controller-alive")

    @unittest.skipUnless(os.name == "nt", "Windows job containment")
    def test_killed_runner_stops_its_child_process(self):
        script = ("import sys,os,subprocess,json;sys.path.insert(0,sys.argv[1]);"
                  "import run_native_test_sessions as s;s.contain_native_children();"
                  "s.contain_native_children();p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(90)']);"
                  "print(json.dumps({'pid':p.pid}),flush=True);os._exit(77)")
        for _ in range(2):
            result = subprocess.run([sys.executable, "-c", script, str(Path(sessions.__file__).parent)],
                                    capture_output=True, text=True, check=False, timeout=20)
            self.assertEqual(result.returncode, 77, result.stderr)
            pid = json.loads(result.stdout)["pid"]
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x100000, False, pid)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0)
                finally:
                    kernel.CloseHandle(handle)
            else:
                self.assertEqual(ctypes.get_last_error(), 87)


class TestMediaCacheTests(OwnedTestRoot):
    def test_startup_removes_only_marked_orphans_and_rejects_changed_group_identity(self):
        cache = media.TestMediaCache(self.root / "_cache")
        pending = cache.root / "_pending-abandoned"
        pending.mkdir()
        marker = pending / "_owner"
        marker.write_text("another owner")
        with self.assertRaisesRegex(RuntimeError, "another owner"):
            media.TestMediaCache(cache.root)
        self.assertTrue(pending.is_dir())
        marker.write_text(media.SIGNATURE)
        cache = media.TestMediaCache(cache.root)
        self.assertFalse(pending.exists())
        group = cache.materialize("tones", {}, lambda path: (path / "tone.wav").write_bytes(b"neutral"))
        data = cache.read(group)
        data["inputs"]["recipe"] = "changed"
        (group / "_complete.json").write_text(json.dumps(data))
        with self.assertRaisesRegex(RuntimeError, "identity disagrees"):
            cache.materialize("tones", {}, lambda _: self.fail("must not rewrite cached work"))

    def test_recipe_reuse_copy_isolation_and_current_plus_two_predecessors(self):
        cache = media.TestMediaCache(self.root / "_cache")
        producer = Mock(side_effect=lambda target: (target / "tone.wav").write_bytes(b"neutral"))
        for _ in range(2):
            group = cache.materialize("tones", {"recipe": 1}, producer)
            destination = self.root / "_case"
            cache.copy_to(group, destination)
        producer.assert_called_once()
        (destination / "tone.wav").write_bytes(b"changed by scenario")
        self.assertTrue(cache.current(group))
        with self.assertRaisesRegex(RuntimeError, "replace existing"):
            cache.copy_to(group, destination)
        for version in range(2, 7):
            cache.materialize("tones", {"recipe": version}, producer)
        self.assertEqual(len(list(cache.root.glob("_ready-*"))), 3)
        cache = media.TestMediaCache(cache.root)
        self.assertEqual(len(list(cache.root.glob("_ready-*"))), 3)

    def test_interrupted_generation_and_publication_reenter_through_current_inputs(self):
        for boundary in ("generation", "before-publish", "after-publish"):
            with self.subTest(boundary=boundary):
                cache = media.TestMediaCache(self.root / ("_" + boundary))
                effects: list[str] = []

                def produce(target, effects=effects, boundary=boundary):
                    effects.append("generated")
                    (target / "fixture.mp4").write_bytes(b"neutral")
                    if boundary == "generation" and len(effects) == 1:
                        raise InterruptedError("during generation")

                replace = Path.replace

                def interrupted(path, target, boundary=boundary, replace=replace):
                    if boundary == "after-publish":
                        replace(path, target)
                    raise InterruptedError("at publish boundary")

                with patch.object(Path, "replace", interrupted), self.assertRaises(InterruptedError):
                    cache.materialize("video", {}, produce)
                self.assertFalse(list(cache.root.glob("_pending-*")))
                group = media.TestMediaCache(cache.root).materialize("video", {}, produce)
                self.assertTrue(cache.current(group))
                self.assertEqual(len(effects), 1 if boundary == "after-publish" else 2)

    def test_interrupted_copy_repeats_atomically_and_removes_only_its_pending_output(self):
        cache = media.TestMediaCache(self.root / "_cache")
        group = cache.materialize("video", {}, lambda p: (p / "source.mp4").write_bytes(b"neutral"))
        for boundary in ("before", "after"):
            target = self.root / ("_" + boundary)
            replace = Path.replace

            def interrupted(path, destination, boundary=boundary, replace=replace):
                if boundary == "after":
                    replace(path, destination)
                raise InterruptedError("copy publication")

            with patch.object(Path, "replace", interrupted), self.assertRaises(InterruptedError):
                cache.copy_to(group, target)
            cache.copy_to(group, target)
            cache.copy_to(group, target)
            self.assertEqual((target / "source.mp4").read_bytes(), b"neutral")
            self.assertFalse(list(target.glob("*.pending")))

    def test_cache_rejects_foreign_ownership_changed_bytes_and_traversal_metadata(self):
        cache = media.TestMediaCache(self.root / "_cache")
        group = cache.materialize("tones", {}, lambda p: (p / "tone.wav").write_bytes(b"neutral"))
        (group / "tone.wav").write_bytes(b"tampered")
        with self.assertRaisesRegex(RuntimeError, "bytes changed"):
            cache.materialize("tones", {}, lambda _: self.fail("must not replace accepted bytes"))
        data = cache.read(group)
        data["files"] = {"../escape": "a" * 64}
        (group / "_complete.json").write_text(json.dumps(data))
        with self.assertRaisesRegex(RuntimeError, "unknown format"):
            cache.copy_to(group, self.root / "_destination")
        (cache.root / "_owner").write_text("another owner")
        with self.assertRaisesRegex(RuntimeError, "another owner"):
            media.TestMediaCache(cache.root)

    def test_completed_models_survive_only_for_matching_speech_worker(self):
        app = self.root / "app.exe"
        worker = self.root / "autopsy-whisper.exe"
        worker.write_bytes(b"worker version 1")
        profile = self.root / "_profile"
        models = profile / "_captions"
        models.mkdir(parents=True)
        (models / "model.bin").write_bytes(b"complete weights")
        (models / "partial.bin.part").write_bytes(b"incomplete")
        (models / "_work").mkdir()
        cache = media.TestMediaCache(self.root / "_cache")
        for _ in range(2):
            media.preserve_caption_models(cache, profile, app)
            media.seed_caption_models(cache, self.root / "_next", app)
        self.assertEqual([path.name for path in (self.root / "_next/_captions").iterdir()], ["model.bin"])
        worker.write_bytes(b"worker version 2")
        media.seed_caption_models(cache, self.root / "_different", app)
        self.assertFalse((self.root / "_different").exists())


class NativeCatalogueTests(OwnedTestRoot):
    def test_case_with_a_ctest_environment_or_special_requirement_keeps_ctest_execution(self):
        command = [sys.executable, str(Path(native.__file__)), "--case", "video-edges"]
        for name in ("ENVIRONMENT", "FIXTURES_REQUIRED", "WILL_FAIL"):
            catalogue = [{"name": "fork-video-edges", "command": command,
                          "properties": [{"name": name, "value": "special"}]}]
            with patch.object(runner, "run_command", return_value="") as execute:
                runner.run_native_catalogue(self.root, self.root, catalogue,
                                            ["fork-video-edges"], collect_receipts=False)
                self.assertEqual(execute.call_args.args[0][0], "ctest")

    def test_catalogue_batches_only_compatible_commands_and_consumes_independent_results(self):
        (self.root / "_work").mkdir()
        driver = Path(native.__file__)
        command = [sys.executable, str(driver), "--executable", str(self.root / "AuTopsy.exe"),
                   "--work-root", str(self.root / "_work")]
        catalogue: list[dict[str, Any]] = [{"name": "fork-" + case, "command": [*command, "--case", case]}
                                        for case in ("video-selection", "video-edges", "branding")]
        calls, effects = [], []

        def run(arguments, _root, **_options):
            calls.append(arguments)
            cases = arguments[arguments.index("--cases") + 1:] if "--cases" in arguments else ["branding"]
            stream = io.StringIO()
            with redirect_stdout(stream):
                for case in cases:
                    def observe(_run, _inputs, case=case):
                        effects.append(case)
                        return {"result": "passed"}

                    value = native.run_native_check(self.root / "_work", case, lambda: {"app": "same"},
                                                    observe)
                    print(json.dumps(value))
            return stream.getvalue()

        names = [test["name"] for test in catalogue]
        with patch.object(runner, "run_command", side_effect=run):
            first = runner.run_native_catalogue(self.root, self.root, catalogue, names)
            second = runner.run_native_catalogue(self.root, self.root, catalogue, names)
        self.assertEqual(calls[0][-3:], ["--cases", "video-selection", "video-edges"])
        self.assertEqual(calls[1][0], "ctest")
        self.assertEqual(effects, ["video-selection", "video-edges", "branding"])
        self.assertEqual([x["acceptance"] for x in first], [x["acceptance"] for x in second])
        self.assertEqual({x["result"] for x in second}, {"reused"})

    def test_private_runtime_requires_ownership_and_an_explicit_named_scope(self):
        app = self.root / "AuTopsy.exe"
        app.write_bytes(b"private payload")
        with patch.object(runner, "run_command") as command:
            with self.assertRaisesRegex(RuntimeError, "marked private"):
                runner.run_private_runtime_checks(self.root, self.root / "_work", app, ".", "baseline")
            (self.root / "_test_runtime_owner").write_text("AuTopsy private test runtime 1")
            with self.assertRaisesRegex(RuntimeError, "matches no"):
                runner.run_private_runtime_checks(self.root, self.root / "_work", app, "no-such-case", "baseline")
        command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
