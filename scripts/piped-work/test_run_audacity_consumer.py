"""Exercise isolated consumer startup, round trips, failure cleanup and pass reuse."""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import run_audacity_consumer as consumer
import test_fork_features as native


class ConsumerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(
            prefix="_consumer-runtime-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT"))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "_owner").write_text("AuTopsy behavior tests 1")
        self.package = self.root / "_package"
        (self.package / "modules").mkdir(parents=True)
        self.executable = self.package / "Audacity.exe"
        self.executable.write_bytes(b"unmodified executable")
        (self.package / "modules/mod-script-pipe.dll").write_bytes(b"unmodified module")
        self.state = {"tracks": [{"kind": "wave", "name": "Audio", "start": 0, "end": 5}],
                      "clips": [{"name": "Tone", "start": 0, "end": 5}], "labels": []}
        self.project = self.root / "_fixture.aup3"
        self.project.write_text(json.dumps(self.state))
        self.audio = self.root / "_fixture.wav"
        self.audio.write_bytes(b"\0" * 16)
        self.descriptor = self.root / "_compatibility-expected.json"
        self.descriptor.write_text(json.dumps(
            [{"project": self.project.name, "state": self.state, "audio": self.audio.name}]))
        self.display = {"device": "secondary", "work": [-1280, 0, 0, 720]}
        self.events = []
        self.live = set()
        self.interrupt = None

    def effect(self, name, operation):
        if self.interrupt == (name, "before"):
            self.interrupt = None
            raise InterruptedError(name + " before")
        result = operation()
        self.events.append(name)
        if self.interrupt == (name, "after"):
            self.interrupt = None
            raise InterruptedError(name + " after")
        return result

    def session_type(self):
        owner = self

        class Session:
            def __init__(self, executable, project, environment, display, scratch, configuration):
                self.command = [str(executable), str(project)]
                self.project = project

            def __enter__(self):
                def open_file():
                    self.state = json.loads(self.project.read_text())
                    owner.live.add(self.project)
                    owner.events.append(("startup-file", self.project.name))
                try:
                    owner.effect("open", open_file)
                    return self
                except BaseException:
                    owner.live.discard(self.project)
                    raise

            def __exit__(self, *exception):
                try:
                    owner.effect("close", lambda: owner.live.discard(self.project))
                finally:
                    owner.live.discard(self.project)

            def query(self, command):
                key = {"Tracks": "tracks", "Clips": "clips", "Labels": "labels"}
                match = re.search(r"Type=(\w+)", command)
                assert match is not None
                kind = match.group(1)
                rows = self.state[key[kind]]
                return [[0, label] for label in rows] if kind == "Labels" else rows

            def send(self, command):
                if command.startswith(("SelectTracks:", "SelectTime:")):
                    return ""
                action = command.split(":", 1)[0]
                match = re.search(r'Filename="([^"]+)"', command)
                assert match is not None
                path = Path(match.group(1))
                if action == "Export2":
                    owner.effect("export", lambda: path.write_bytes(owner.audio.read_bytes()))
                elif action in ("SaveProject2", "SaveCopy"):
                    owner.effect(action, lambda: path.write_text(json.dumps(self.state)))
                else:
                    raise AssertionError("Unexpected stock command: " + command)
                return ""

        return Session

    def run_consumer(self):
        with patch.object(consumer, "executable_version", return_value=(3, 7, 9)), \
                patch.object(consumer, "StockAudacitySession", self.session_type()), \
                patch.object(consumer, "run_process", side_effect=lambda args, **kw: Path(args[4]).read_bytes()):
            return consumer.check_stock_audacity_projects(
                self.root, self.executable, self.display, "ffmpeg")

    def assert_sources_and_cleanup(self, original):
        self.assertEqual(consumer.read_compatibility_fixture_inputs(self.root), original)
        self.assertFalse(self.live)
        self.assertFalse((self.root / "_stock-audacity").exists())

    def test_repeat_uses_normal_startup_for_each_open_and_reopen(self):
        original = consumer.read_compatibility_fixture_inputs(self.root)
        for _ in range(2):
            result = self.run_consumer()
            self.assertEqual(result["projects"], [self.project.name])
            self.assertEqual(result["open_save_reopen_repeats"], 2)
            self.assert_sources_and_cleanup(original)
        opened = [row[1] for row in self.events if isinstance(row, tuple)]
        self.assertEqual(opened, ["_input-0-0.aup3", "_reopened-0-0.aup3",
                                  "_input-0-1.aup3", "_reopened-0-1.aup3"] * 2)

    def test_fresh_entry_converges_after_each_session_effect_interruption(self):
        original = consumer.read_compatibility_fixture_inputs(self.root)
        for operation in ("open", "export", "SaveProject2", "SaveCopy", "close"):
            for boundary in ("before", "after"):
                with self.subTest(operation=operation, boundary=boundary):
                    self.interrupt = (operation, boundary)
                    with self.assertRaises(InterruptedError):
                        self.run_consumer()
                    self.assertFalse(self.live)
                    self.run_consumer()
                    self.assert_sources_and_cleanup(original)

    def test_filesystem_effect_interruption_does_not_publish_or_retain_scratch(self):
        original = consumer.read_compatibility_fixture_inputs(self.root)
        for name in ("copytree", "copyfile", "rmtree"):
            function = getattr(shutil, name)
            for boundary in ("before", "after"):
                seen = False

                def interrupt(*args, function=function, boundary=boundary, name=name, **kwargs):
                    nonlocal seen
                    if seen:
                        return function(*args, **kwargs)
                    seen = True
                    if boundary == "before":
                        raise InterruptedError(name)
                    function(*args, **kwargs)
                    raise InterruptedError(name)

                with self.subTest(name=name, boundary=boundary), \
                        patch.object(consumer.shutil, name, side_effect=interrupt), \
                        self.assertRaises((InterruptedError, RuntimeError)):
                    self.run_consumer()
                self.run_consumer()
                self.assert_sources_and_cleanup(original)

    def test_missing_linked_and_escaping_inputs_block_before_launch(self):
        original = self.descriptor.read_bytes()
        for name in ("../_outside.aup3", "_missing.aup3"):
            with self.subTest(name=name):
                self.descriptor.write_text(json.dumps([{"project": name, "state": self.state}]))
                with self.assertRaises(RuntimeError):
                    self.run_consumer()
                self.assertFalse(self.events)
        self.descriptor.write_bytes(original)

    def test_unowned_scratch_is_left_untouched(self):
        scratch = self.root / "_stock-audacity"
        scratch.mkdir()
        unrelated = scratch / "_unrelated"
        unrelated.write_text("keep")
        with self.assertRaisesRegex(RuntimeError, "another owner"):
            self.run_consumer()
        self.assertEqual(unrelated.read_text(), "keep")
        self.assertFalse(self.events)

    def test_primary_child_error_and_cleanup_error_both_survive(self):
        session = consumer.StockAudacitySession(
            self.executable, self.project, {}, self.display, self.root, "")
        (self.root / "_stdout.log").write_text("child output")
        (self.root / "_stderr.log").write_text("child error")
        stopped = []

        def stop():
            stopped.append(True)
            raise OSError("cleanup refused")

        session.app = SimpleNamespace(poll=lambda: 42, close=stop)
        with self.assertRaisesRegex(RuntimeError, "opening failed.*exit status 42.*child output.*child error.*cleanup refused"):
            session.__exit__(ValueError, ValueError("opening failed"), None)
        self.assertEqual(stopped, [True])

    def test_profile_reset_removes_recovery_state_and_repeats_without_writes(self):
        scratch = self.root / "_private"
        scratch.mkdir()
        (scratch / "_owner").write_text("AuTopsy stock consumer scratch 1")
        executable = scratch / "_app/Audacity.exe"
        executable.parent.mkdir()
        profile = executable.parent / "Portable Settings"
        profile.mkdir()
        (profile / "audacity.cfg").write_text("stale recovery list")
        (profile / "_recovery").write_text("old working copy")
        environment = {"TEMP": str(scratch / "_gui_temp"), "TMP": str(scratch / "_gui_temp")}
        for name in ("_temp", "_gui_temp"):
            path = scratch / name
            path.mkdir()
            (path / "_old").write_text("old scratch")
        consumer.prepare_consumer_profile(executable, "fresh preferences", environment, scratch)
        self.assertEqual([p.name for p in profile.iterdir()], ["audacity.cfg"])
        self.assertEqual((profile / "audacity.cfg").read_text(), "fresh preferences")
        with patch.object(Path, "write_text", side_effect=AssertionError("unnecessary write")), \
                patch.object(shutil, "rmtree", side_effect=AssertionError("unnecessary removal")):
            consumer.prepare_consumer_profile(executable, "fresh preferences", environment, scratch)
        for name in ("_temp", "_gui_temp"):
            self.assertEqual(list((scratch / name).iterdir()), [])

    def test_profile_reset_converges_after_every_filesystem_effect_boundary(self):
        for operation in ("rmtree", "mkdir", "write_text"):
            for boundary in ("before", "after"):
                scratch = self.root / ("_profile-" + operation + "-" + boundary)
                scratch.mkdir()
                (scratch / "_owner").write_text("AuTopsy stock consumer scratch 1")
                executable = scratch / "_app/Audacity.exe"
                executable.parent.mkdir()
                profile = executable.parent / "Portable Settings"
                profile.mkdir()
                (profile / "audacity.cfg").write_text("old recovery")
                environment = {"TEMP": str(scratch / "_gui_temp"), "TMP": str(scratch / "_gui_temp")}
                owner = shutil if operation == "rmtree" else Path
                function = getattr(owner, operation)
                seen = False

                def interrupt(*args, function=function, boundary=boundary, operation=operation, **kwargs):
                    nonlocal seen
                    if seen:
                        return function(*args, **kwargs)
                    seen = True
                    if boundary == "before":
                        raise InterruptedError(operation)
                    function(*args, **kwargs)
                    raise InterruptedError(operation)

                with self.subTest(operation=operation, boundary=boundary), \
                        patch.object(owner, operation, new=interrupt), \
                        self.assertRaises(InterruptedError):
                    consumer.prepare_consumer_profile(executable, "fresh", environment, scratch)
                consumer.prepare_consumer_profile(executable, "fresh", environment, scratch)
                consumer.prepare_consumer_profile(executable, "fresh", environment, scratch)
                self.assertEqual((profile / "audacity.cfg").read_text(), "fresh")
                self.assertEqual(list((scratch / "_temp").iterdir()), [])
                self.assertEqual(list((scratch / "_gui_temp").iterdir()), [])

    def test_profile_path_escape_blocks_before_deleting_anything(self):
        scratch = self.root / "_private"
        scratch.mkdir()
        (scratch / "_owner").write_text("AuTopsy stock consumer scratch 1")
        outside = self.root / "_unrelated"
        outside.mkdir()
        (outside / "_keep").write_text("keep")
        with self.assertRaisesRegex(RuntimeError, "escapes"):
            consumer.prepare_consumer_profile(
                scratch / "_app/Audacity.exe", "fresh",
                {"TEMP": str(outside), "TMP": str(outside)}, scratch)
        self.assertEqual((outside / "_keep").read_text(), "keep")

    def test_descriptor_survives_failed_package_consumer(self):
        original = self.descriptor.read_bytes()

        class Cache:
            def __init__(self, *args):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def prepare_consumer(self, package):
                return {"executable": "pinned"}

        with patch.object(consumer, "AudacityConsumerCache", Cache), \
                patch.object(consumer, "check_stock_audacity_projects", side_effect=RuntimeError("consumer failed")), \
                self.assertRaisesRegex(RuntimeError, "consumer failed"):
            consumer.check_portable_audacity_projects(self.root, self.display, "ffmpeg")
        self.assertEqual(self.descriptor.read_bytes(), original)

    def test_consumer_failure_and_check_update_preserve_accepted_application(self):
        calls = []
        inputs = {"application": "same artifact", "execution_scope": {"display": self.display}}

        def execute(run_id, identities):
            calls.append(run_id)
            return {"result": "passed",
                    "compatibility_fixtures": consumer.read_compatibility_fixture_inputs(self.root)}

        def run():
            return native.run_project_compatibility_checks(
                self.root, "groups", lambda: inputs, execute, self.root, self.display, "ffmpeg")

        with patch.object(native, "check_portable_audacity_projects", side_effect=RuntimeError("stock failed")), \
                self.assertRaisesRegex(RuntimeError, "stock failed"):
            run()
        with patch.object(native, "check_portable_audacity_projects", return_value=["passed"]) as stock:
            accepted = run()
            again = run()
            self.assertEqual(stock.call_count, 1)
            self.assertEqual(accepted["application"]["original_run"], again["application"]["original_run"])
        read = Path.read_bytes

        def changed(path):
            return b"new consumer check" if path.name == "run_audacity_consumer.py" else read(path)

        with patch.object(Path, "read_bytes", new=changed), \
                patch.object(native, "check_portable_audacity_projects", return_value=["passed"]) as stock:
            current = run()
            self.assertEqual(stock.call_count, 1)
            self.assertEqual(current["application"]["original_run"], accepted["application"]["original_run"])
        self.assertEqual(len(calls), 1)

    def test_changed_accepted_fixture_blocks_without_replaying_application(self):
        calls = []

        def execute(*args):
            calls.append(True)
            return {"result": "passed",
                    "compatibility_fixtures": consumer.read_compatibility_fixture_inputs(self.root)}

        def run():
            return native.run_project_compatibility_checks(
                self.root, "groups", lambda: {"execution_scope": {}}, execute,
                self.root, self.display, "ffmpeg")

        with patch.object(native, "check_portable_audacity_projects", return_value=[]):
            run()
            self.project.write_text("tampered")
            with self.assertRaisesRegex(RuntimeError, "missing or changed"):
                run()
        self.assertEqual(calls, [True])


@unittest.skipUnless(os.name == "nt", "Windows process ownership")
class OwnedProcessTest(unittest.TestCase):
    def test_worker_interruption_stops_its_app_without_a_pid_recovery_file(self):
        """Closing the worker's OS handles ends its already-owned child tree."""
        with tempfile.TemporaryDirectory(
            prefix="_consumer-worker-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT"),
        ) as temporary:
            root = Path(temporary)
            ready = root / "_ready.json"
            module_root = Path(consumer.__file__).parent
            child = "import time; time.sleep(60)"
            code = (
                "import json,os,sys,time; from pathlib import Path; "
                f"sys.path.insert(0,{str(module_root)!r}); "
                "from run_audacity_consumer import OwnedConsumerProcess; "
                f"root=Path({str(root)!r}); "
                "out=(root/'_stdout').open('w'); err=(root/'_stderr').open('w'); "
                f"p=OwnedConsumerProcess([sys.executable,'-c',{child!r}],root,dict(os.environ),out,err); "
                f"Path({str(ready)!r}).write_text(json.dumps(p.pid)); time.sleep(60)"
            )
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            for _ in range(2):
                ready.unlink(missing_ok=True)
                worker = subprocess.Popen(
                    [sys.executable, "-c", code], stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW,
                )
                handle = None
                try:
                    deadline = time.monotonic() + 10
                    while not ready.is_file():
                        if worker.poll() is not None:
                            output, errors = worker.communicate()
                            self.fail(f"Worker exit {worker.returncode}; stdout {output!r}; stderr {errors!r}")
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.02)
                    handle = kernel.OpenProcess(0x100000, False, json.loads(ready.read_text()))
                    self.assertTrue(handle)
                    self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258)
                    worker.terminate()
                    worker.communicate(timeout=5)
                    self.assertEqual(kernel.WaitForSingleObject(handle, 3000), 0)
                finally:
                    if worker.poll() is None:
                        worker.terminate()
                    worker.communicate(timeout=5)
                    if handle:
                        kernel.CloseHandle(handle)

    def test_job_stops_only_its_process_tree_and_repeated_close_is_safe(self):
        with tempfile.TemporaryDirectory(
            prefix="_consumer-job-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT"),
        ) as temporary:
            root = Path(temporary)
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            for repeat in range(2):
                pids = root / ("_child-" + str(repeat) + ".json")
                child = "import time; time.sleep(60)"
                code = ("import json,subprocess,sys,time; from pathlib import Path; "
                        f"p=subprocess.Popen([sys.executable,'-c',{child!r}]); "
                        f"Path({str(pids)!r}).write_text(json.dumps(p.pid)); time.sleep(60)")
                with (root / "_stdout").open("w") as output, (root / "_stderr").open("w") as errors:
                    app = consumer.OwnedConsumerProcess(
                        [sys.executable, "-c", code], root, dict(os.environ), output, errors)
                    child_handle = None
                    try:
                        deadline = time.monotonic() + 10
                        while not pids.is_file():
                            self.assertIsNone(app.poll())
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(0.02)
                        child_handle = kernel.OpenProcess(0x100000, False, json.loads(pids.read_text()))
                        self.assertTrue(child_handle)
                        self.assertEqual(kernel.WaitForSingleObject(child_handle, 0), 258)
                        app.close()
                        app.close()
                        self.assertIsNotNone(app.poll())
                        self.assertEqual(kernel.WaitForSingleObject(child_handle, 3000), 0)
                    finally:
                        app.close()
                        if child_handle:
                            kernel.CloseHandle(child_handle)


if __name__ == "__main__":
    unittest.main()
