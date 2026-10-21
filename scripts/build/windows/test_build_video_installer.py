"""Behavior cases for installer builds and interrupted SDK publication."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Any
from unittest import mock

import build_video_installer as builder


class InstallerBuildTests(unittest.TestCase):
    def test_existing_build_checks_keep_the_installer_unchanged_after_interruption(self):
        for boundary in ("before", "after"):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                root = Path(temporary)
                build = root / "build"
                build.mkdir()
                (build / "CTestTestfile.cmake").write_text("# existing test project")
                installer = build / "completed.exe"
                installer.write_bytes(b"accepted installer")
                effects: list[str] = []

                def check(arguments, source, diagnostics, selected_boundary=boundary, observed=effects):
                    if selected_boundary == "before":
                        raise InterruptedError("before test child")
                    observed.append("checked")
                    raise InterruptedError("after test child")

                with mock.patch.object(builder.shutil, "which", return_value="ctest"), \
                        mock.patch.object(builder, "run_command", side_effect=check):
                    with self.assertRaises(InterruptedError):
                        builder.test_existing_installer_build(root, build, "^fork-fixtures$")
                self.assertEqual(effects, [] if boundary == "before" else ["checked"])
                with mock.patch.object(builder.shutil, "which", return_value="ctest"), \
                        mock.patch.object(builder, "run_command", return_value="") as runner:
                    first = builder.test_existing_installer_build(root, build, "^fork-fixtures$")
                    self.assertEqual(builder.test_existing_installer_build(root, build, "^fork-fixtures$"), first)
                    self.assertEqual(runner.call_count, 2)
                self.assertEqual(first["status"], "passed")
                self.assertEqual(installer.read_bytes(), b"accepted installer")

    def test_seven_zip_sdk_uses_the_existing_cmake_extractor(self):
        with tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "player.dll").write_bytes(b"runtime")
            archive = root / "fixture.7z"
            cmake = shutil.which("cmake")
            self.assertIsNotNone(cmake)
            builder.run_command([str(cmake), "-E", "tar", "cf", str(archive), "--format=7zip", "player.dll"],
                                source, root / "_failure.json")
            cache = root / "cache"
            cache.mkdir()
            spec = {"url": archive.as_uri(), "sha256": builder.sha256(archive),
                    "files": ["player.dll"], "kind": "7z"}
            current = builder.prepare_sdk("fixture", spec, cache, source, root / "_failure.json")
            self.assertEqual((current / "player.dll").read_bytes(), b"runtime")
            self.assertEqual(builder.prepare_sdk("fixture", spec, cache, source, root / "_failure.json"), current)

    @unittest.skipUnless(sys.platform == "win32", "The installer process tree is Windows-only")
    def test_abrupt_worker_exit_stops_its_build_child(self):
        import win32api
        import win32con
        import win32event

        for _ in range(2):
            command = [sys.executable, str(Path(__file__)), "--job-worker"]
            result = subprocess.run(command, capture_output=True, text=True, check=False,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 77, result.stderr)
            pid = json.loads(result.stdout)["pid"]
            try:
                process = win32api.OpenProcess(win32con.SYNCHRONIZE, False, pid)
            except Exception as error:
                self.assertEqual(getattr(error, "winerror", None), 87)
            else:
                try:
                    self.assertEqual(win32event.WaitForSingleObject(process, 5000), win32event.WAIT_OBJECT_0)
                finally:
                    process.Close()

    def test_repeat_and_interrupt_each_build_command(self):
        for boundary in range(1, 6):
            for after in (False, True):
                with self.subTest(boundary=boundary, after=after), tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                    root = Path(temporary)
                    source, build = root / "source", root / "build"
                    source.mkdir()
                    (source / "CMakeLists.txt").write_text("project(test)")
                    inno = root / "ISCC.exe"
                    inno.write_bytes(b"test compiler")
                    calls = 0
                    interrupted = False

                    def run(arguments, _source, _diagnostics, *, boundary=boundary, after=after, build=build):
                        nonlocal calls, interrupted
                        calls += 1
                        stop = calls == boundary and not interrupted
                        if stop and not after:
                            interrupted = True
                            raise InterruptedError("before command")
                        if "innosetup" in arguments:
                            package = build / "package"
                            package.mkdir(exist_ok=True)
                            name = "autopsy-win-test-x64.exe"
                            target = package / name
                            if not target.exists():
                                target.write_bytes(b"completed installer")
                            builder.write_json(package / "_installer-x64.json",
                                               {"filename": name, "sha256": builder.sha256(target)})
                        result = "a" * 40 + "\n" if arguments[1:3] == ["rev-parse", "HEAD"] else ""
                        if stop and after:
                            interrupted = True
                            raise InterruptedError("after command")
                        return result

                    with mock.patch.object(builder, "prepare_sdk", return_value=root), \
                         mock.patch.object(builder.shutil, "which", return_value="cmake"), \
                         mock.patch.object(builder, "run_command", side_effect=run), \
                         mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}):
                        with self.assertRaises(InterruptedError):
                            builder.build_installer(source, build, inno)
                        first = builder.build_installer(source, build, inno)
                        manifest = build / "package/_build-manifest.json"
                        timestamp = manifest.stat().st_mtime_ns
                        self.assertEqual(builder.build_installer(source, build, inno), first)
                        self.assertEqual(manifest.stat().st_mtime_ns, timestamp)
                        self.assertFalse(first["signed"])
                        self.assertEqual(first["source_revision"], "a" * 40)

    def test_sdk_publication_survives_process_exit_at_each_file_effect(self):
        with tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            base = Path(temporary)
            archive = base / "sdk.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("sdk/bin/player.dll", b"runtime")
                output.writestr("sdk/include/client.h", b"public header")
            spec = {"url": archive.as_uri(), "sha256": builder.sha256(archive),
                    "files": ["player.dll", "include/client.h"], "kind": "zip"}
            spec_path = base / "spec.json"
            spec_path.write_text(json.dumps(spec))
            command = [sys.executable, str(Path(__file__)), "--sdk-worker", str(spec_path)]
            options: dict[str, Any] = {"capture_output": True, "text": True}
            if os.name == "nt":
                options["creationflags"] = subprocess.CREATE_NO_WINDOW
            baseline = base / "baseline"
            baseline.mkdir()
            result = subprocess.run([*command, str(baseline), "0", "before"], **options)
            self.assertEqual(result.returncode, 0, result.stderr)
            count = json.loads(result.stdout)["effects"]
            self.assertGreater(count, 8)
            for boundary in range(1, count + 1):
                for phase in ("before", "after"):
                    with self.subTest(boundary=boundary, phase=phase):
                        root = base / f"case-{boundary}-{phase}"
                        root.mkdir()
                        stopped = subprocess.run([*command, str(root), str(boundary), phase], **options)
                        self.assertEqual(stopped.returncode, 78, stopped.stderr)
                        current = builder.prepare_sdk("fixture", spec, root, base, base / "diagnostics.json")
                        self.assertEqual((current / "player.dll").read_bytes(), b"runtime")
                        self.assertEqual((current / "include/client.h").read_bytes(), b"public header")
                        self.assertEqual(builder.prepare_sdk("fixture", spec, root, base, base / "diagnostics.json"), current)
                        self.assertFalse(list(root.glob("*.pending")))
                        self.assertFalse(list(root.glob("*.unpack")))

    def test_bad_download_keeps_prior_archive_and_unrelated_directory(self):
        with tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            incoming, archive = root / "source.bin", root / "_sdk.zip"
            incoming.write_bytes(b"unexpected")
            archive.write_bytes(b"old completed archive")
            with self.assertRaises(RuntimeError):
                builder.download_archive({"url": incoming.as_uri(), "sha256": "0" * 64}, archive)
            self.assertEqual(archive.read_bytes(), b"old completed archive")
            self.assertFalse((root / "__sdk.zip.pending").exists())
            unrelated = root / "_fixture"
            unrelated.mkdir()
            (unrelated / "user-file").write_bytes(b"keep")
            with self.assertRaises(RuntimeError):
                builder.remove_owned_tree(unrelated, root)
            self.assertEqual((unrelated / "user-file").read_bytes(), b"keep")

    def test_child_failure_retains_invocation_exit_and_both_streams(self):
        with tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            diagnostics = root / "_failure.json"
            command = [sys.executable, "-c", "import sys; print('out'); print('err',file=sys.stderr); sys.exit(7)"]
            with self.assertRaises(RuntimeError):
                builder.run_command(command, root, diagnostics)
            report = json.loads(diagnostics.read_text())
            self.assertEqual(report["argv"], command)
            self.assertEqual(report["exit"], 7)
            self.assertIn("out", report["stdout"])
            self.assertIn("err", report["stderr"])


def sdk_worker(spec_path: Path, root: Path, boundary: int, phase: str) -> None:
    """Hard exits leave real orphaned files; the parent uses the normal entrypoint."""
    spec = json.loads(spec_path.read_text())
    effects = 0

    def observe(function, path_index=0, writes_only=False):
        def wrapped(*args, **kwargs):
            nonlocal effects
            path = Path(args[path_index]).resolve()
            mode = args[1] if len(args) > 1 else kwargs.get("mode", "r")
            writing = not writes_only or any(value in str(mode) for value in "wax+")
            owned = path.is_relative_to(root.resolve()) and writing
            stop = False
            if owned:
                effects += 1
                stop = effects == boundary
                if stop and phase == "before":
                    os._exit(78)
            result = function(*args, **kwargs)
            if stop and phase == "after":
                os._exit(78)
            return result
        return wrapped

    with mock.patch.object(Path, "mkdir", observe(Path.mkdir)), \
         mock.patch.object(Path, "open", observe(Path.open, writes_only=True)), \
         mock.patch.object(Path, "write_text", observe(Path.write_text)), \
         mock.patch.object(Path, "replace", observe(Path.replace)), \
         mock.patch.object(Path, "unlink", observe(Path.unlink)), \
         mock.patch.object(Path, "rmdir", observe(Path.rmdir)), \
         mock.patch.object(builder.shutil, "rmtree", observe(builder.shutil.rmtree)), \
         mock.patch.object(builder.shutil, "copyfile", observe(builder.shutil.copyfile, path_index=1)):
        builder.prepare_sdk("fixture", spec, root, root.parent, root.parent / "diagnostics.json")
    print(json.dumps({"effects": effects}))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--job-worker":
        builder.ensure_process_tree_cleanup()
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        print(json.dumps({"pid": child.pid}), flush=True)
        os._exit(77)
    elif len(sys.argv) > 1 and sys.argv[1] == "--sdk-worker":
        sdk_worker(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
    else:
        unittest.main()
