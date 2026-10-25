"""Behavior cases for installer builds and interrupted SDK publication."""
from __future__ import annotations

import itertools
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
    def test_native_engine_identity_survives_relocated_temporary_launchers(self):
        with tempfile.TemporaryDirectory(prefix="_cmake-engine-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            source, build = root / "source", root / "build"
            source.mkdir()
            build.mkdir()
            (source / "main.cpp").write_text("product", encoding="utf-8")
            native = root / "_native-cmake.exe"
            native.write_bytes(b"pinned native CMake engine")
            first, second = root / "_launcher-one.exe", root / "_launcher-two.exe"
            first.write_bytes(b"temporary Python path one")
            second.write_bytes(b"temporary Python path two")
            cache = build / "CMakeCache.txt"
            cache.write_text(f"# Generated at first location\nCMAKE_COMMAND:INTERNAL={native}\nCMAKE_ROOT:INTERNAL=first\nCMAKE_CXX_FLAGS:STRING=/O2\n", encoding="utf-8")
            with mock.patch.object(builder, "run_command", return_value="main.cpp\0"):
                original = builder.compiled_runtime_inputs(source, build, [str(first), "-B", str(build)], {}, root / "_failure.json")
                cache.write_text(cache.read_text(encoding="utf-8").replace("first", "second"), encoding="utf-8")
                self.assertEqual(builder.compiled_runtime_inputs(source, build, [str(second), "-B", str(build)], {}, root / "_failure.json"), original)
                native.write_bytes(b"different native CMake engine")
                self.assertNotEqual(builder.compiled_runtime_inputs(source, build, [str(second), "-B", str(build)], {}, root / "_failure.json"), original)

    def test_compiled_runtime_inputs_distinguish_product_changes_from_check_edits(self):
        with tempfile.TemporaryDirectory(prefix="_compile-inputs-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            source, build = root / "source", root / "build"
            source.mkdir()
            build.mkdir()
            tracked = ("main.cpp", "scripts/piped-work/test_video_clip_edits.py", "docs/design.md", "README.md",
                       "scripts/build/windows/test_build_video_installer.py")
            for name in tracked:
                target = source / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("original", encoding="utf-8")
            tool, cache = root / "cmake.exe", build / "CMakeCache.txt"
            tool.write_bytes(b"compiler driver")
            cache.write_text("CMAKE_CXX_FLAGS:STRING=/O2", encoding="utf-8")
            configure = [str(tool), "-S", str(source), "-B", str(build)]
            with mock.patch.object(builder, "run_command", return_value="\0".join(tracked) + "\0"):
                initial = builder.compiled_runtime_inputs(source, build, configure, {}, root / "_failure.json")
                for name in tracked[1:]:
                    (source / name).write_text("edited check/document", encoding="utf-8")
                    self.assertEqual(builder.compiled_runtime_inputs(source, build, configure, {}, root / "_failure.json"), initial)
                for path in (source / "main.cpp", cache, tool):
                    original = path.read_bytes()
                    path.write_bytes(b"changed product input")
                    self.assertNotEqual(builder.compiled_runtime_inputs(source, build, configure, {}, root / "_failure.json"), initial)
                    path.write_bytes(original)
                with mock.patch.dict(os.environ, {"CL": "/fsanitize=address"}):
                    self.assertNotEqual(builder.compiled_runtime_inputs(source, build, configure, {}, root / "_failure.json"), initial)

    def test_compiled_runtime_repeats_and_interruptions_keep_only_completed_provenance(self):
        for effect, after in itertools.product(("compile", "record"), (False, True)):
            with self.subTest(effect=effect, after=after), tempfile.TemporaryDirectory(prefix="_compile-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                build = Path(temporary)
                runtime = build / "Release"
                runtime.mkdir()
                executable = runtime / "AuTopsy.exe"
                inputs = {"source_files": {"main.cpp": "original"}, "settings": "release"}
                calls: list[str] = []
                stopped = False
                real_write = builder.write_json

                def boundary(label, operation, *args, selected_effect=effect, selected_after=after, **kwargs):
                    nonlocal stopped
                    stop = selected_effect == label and not stopped
                    if stop:
                        stopped = True
                    if stop and not selected_after:
                        raise InterruptedError("before " + label)
                    value = operation(*args, **kwargs)
                    if stop and selected_after:
                        raise InterruptedError("after " + label)
                    return value

                def compile_effect(*_args, calls=calls, executable=executable):
                    calls.append("compile")
                    executable.write_bytes(b"compiled product")

                with mock.patch.object(builder, "compiled_runtime_inputs", side_effect=lambda *a, inputs=inputs: dict(inputs)), \
                        mock.patch.object(builder, "run_command", side_effect=lambda *a, boundary=boundary, compile_effect=compile_effect: boundary("compile", compile_effect, *a)), \
                        mock.patch.object(builder, "write_json", side_effect=lambda *a, boundary=boundary, real_write=real_write: boundary("record", real_write, *a)):
                    with self.assertRaises(InterruptedError):
                        builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "a" * 40, build / "_failure.json")
                    completed = builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "a" * 40, build / "_failure.json")
                    assert completed is not None
                    record = build / "_compiled-runtime.json"
                    original, count = record.read_bytes(), len(calls)
                    self.assertEqual(builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "b" * 40, build / "_failure.json"), completed)
                    self.assertEqual(len(calls), count)
                    self.assertEqual(record.read_bytes(), original)
                    self.assertEqual(completed["source_revision"], "a" * 40)
                    executable.write_bytes(b"unexpected runtime change")
                    builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "b" * 40, build / "_failure.json")
                    self.assertEqual(len(calls), count + 1)
                    inputs["settings"] = "different requested build"
                    builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "c" * 40, build / "_failure.json")
                    self.assertEqual(len(calls), count + 2)
                self.assertFalse((build / "__compiled-runtime.json.pending").exists())

    def test_changed_product_during_compilation_cannot_publish_provenance(self):
        with tempfile.TemporaryDirectory(prefix="_compile-change-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            build = Path(temporary)
            inputs = {"source_files": {"main.cpp": "before"}}
            def compile_changed_source(*_args):
                inputs["source_files"] = {"main.cpp": "after"}
            with mock.patch.object(builder, "compiled_runtime_inputs", side_effect=lambda *a: dict(inputs)), \
                    mock.patch.object(builder, "run_command", side_effect=compile_changed_source):
                with self.assertRaisesRegex(RuntimeError, "changed during compilation"):
                    builder.reuse_or_compile_runtime(build, build, ["cmake"], {}, "a" * 40, build / "_failure.json")
            self.assertFalse((build / "_compiled-runtime.json").exists())

    @staticmethod
    def native_report(build):
        runtime = build / "Release"
        runtime.mkdir(exist_ok=True)
        executable = runtime / "AuTopsy.exe"
        if not executable.exists():
            executable.write_bytes(b"checked native host")
        return {"result": "passed", "scope": "prepackage", "payload_directory": "Release",
                "payload": {executable.name: builder.sha256(executable)},
                "checks": [{"name": name, "result": "passed"} for name in (
                    "fork-fixtures", "fork-preview", "fork-project-compatibility")]}

    def test_package_location_does_not_activate_or_follow_native_test_selection(self):
        for _ in range(2):
            with mock.patch.dict(os.environ, {"AUTOPSY_INSTALLER_BUILD_DIR": "_accepted-build",
                                              "AUTOPSY_VIDEO_BUILD_DIR": "_native-tests"}, clear=True):
                self.assertEqual(builder.get_installer_build_directory(), Path("_accepted-build"))
            with mock.patch.dict(os.environ, {"AUTOPSY_VIDEO_BUILD_DIR": "_native-tests"}, clear=True):
                self.assertEqual(builder.get_installer_build_directory(), Path(".build/_windows-installer"))

    def test_preview_publication_repeats_and_interrupts_each_remote_effect(self):
        revision, body = "a" * 40, "Unsigned AuTopsy preview.\n"
        for boundary, line_ending in itertools.product(range(5), ("\n", "\r\n")):
            returned_body = body.replace("\n", line_ending)
            for after in ((False, True) if boundary else (False,)):
                with self.subTest(boundary=boundary, after=after, line_ending=line_ending), tempfile.TemporaryDirectory(prefix="_release-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                    root = Path(temporary)
                    source, build = root / "source", root / "build"
                    source.mkdir()
                    package = build / "package"
                    package.mkdir(parents=True)
                    installer = package / "autopsy-win-test-x64.exe"
                    installer.write_bytes(b"accepted installer")
                    manifest = package / "_build-manifest.json"
                    builder.write_json(manifest, {
                        "schema": "autopsy-windows-installer.v1", "source_revision": revision,
                        "source_dirty": False, "signed": False, "filename": installer.name,
                        "sha256": builder.sha256(installer),
                        "checks": [{"name": "fork-fixtures", "result": "passed", "source_revision": revision}],
                    })
                    notes = root / "notes.md"
                    notes.write_text(body, encoding="utf-8")
                    remote: dict[str, Any] = {"release": None, "tag": None, "calls": 0,
                                               "stopped": False, "effects": []}

                    def command(arguments, _source, _diagnostics, *, state=remote,
                                selected=boundary, after_effect=after, release_body=returned_body, **_options):
                        if arguments[0] == "git":
                            return ""
                        if arguments[:3] == ["gh", "release", "view"]:
                            return json.dumps({"apiUrl": "https://api.github.com/repos/Ceratops-Code/AuTopsy/releases/1"}) if state["release"] else ""
                        if arguments[:2] == ["gh", "api"]:
                            if "/git/ref/tags/" in arguments[2]:
                                return json.dumps({"object": {"type": "commit", "sha": state["tag"]}}) if state["tag"] else ""
                            return json.dumps(state["release"])
                        state["calls"] += 1
                        stop = state["calls"] == selected and not state["stopped"]
                        if stop and not after_effect:
                            state["stopped"] = True
                            raise InterruptedError("before remote effect")
                        action = arguments[2]
                        state["effects"].append(action)
                        if action == "create":
                            state["release"] = {"draft": True, "prerelease": True, "assets": [],
                                                "target_commitish": revision, "body": release_body,
                                                "name": "AuTopsy v3.7.10-autopsy.2",
                                                "html_url": "https://github.com/Ceratops-Code/AuTopsy/releases/tag/v3.7.10-autopsy.2"}
                        elif action == "upload":
                            path = Path(arguments[4])
                            state["release"]["assets"].append({"name": path.name, "digest": "sha256:" + builder.sha256(path)})
                        elif action == "edit":
                            state["release"]["draft"] = False
                            state["tag"] = revision
                        else:
                            raise AssertionError(arguments)
                        if stop and after_effect:
                            state["stopped"] = True
                            raise InterruptedError("after remote effect")
                        return ""

                    with mock.patch.object(builder, "run_command", side_effect=command):
                        if boundary:
                            with self.assertRaises(InterruptedError):
                                builder.publish_installer(source, build, "Ceratops-Code/AuTopsy", "v3.7.10-autopsy.2", notes)
                        result = builder.publish_installer(source, build, "Ceratops-Code/AuTopsy", "v3.7.10-autopsy.2", notes)
                        effects = list(remote["effects"])
                        self.assertEqual(builder.publish_installer(source, build, "Ceratops-Code/AuTopsy", "v3.7.10-autopsy.2", notes), result)
                        self.assertEqual(remote["effects"], effects)
                        self.assertEqual(effects, ["create", "upload", "upload", "edit"])
                        self.assertEqual(result["status"], "published")
                        self.assertEqual(set(result["assets"]), {installer.name, manifest.name})
                        remote["release"]["body"] = returned_body + "Different release text."
                        with self.assertRaisesRegex(RuntimeError, "requested release metadata"):
                            builder.publish_installer(source, build, "Ceratops-Code/AuTopsy", "v3.7.10-autopsy.2", notes)
                        self.assertEqual(remote["effects"], effects)
                        remote["release"]["body"] = returned_body
                        remote["tag"] = "b" * 40
                        with self.assertRaisesRegex(RuntimeError, "different source revision"):
                            builder.publish_installer(source, build, "Ceratops-Code/AuTopsy", "v3.7.10-autopsy.2", notes)
                        self.assertEqual(remote["effects"], effects)

    def test_existing_build_checks_keep_the_installer_unchanged_after_interruption(self):
        for phase in (0, 1):
            for boundary in ("before", "after"):
                with self.subTest(phase=phase, boundary=boundary), tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                    root = Path(temporary)
                    build = root / "build"
                    build.mkdir()
                    (build / "CTestTestfile.cmake").write_text("# existing test project")
                    installer = build / "completed.exe"
                    installer.write_bytes(b"accepted installer")
                    effects: list[str] = []
                    state = {"call": 0}

                    def check(arguments, source, diagnostics, selected_phase=phase,
                              selected_boundary=boundary, observed=effects, state=state, **_options):
                        current = state["call"]
                        state["call"] += 1
                        if current == selected_phase and selected_boundary == "before":
                            raise InterruptedError("before child effect")
                        observed.append("configure" if "-S" in arguments else "check")
                        if current == selected_phase and selected_boundary == "after":
                            raise InterruptedError("after child effect")

                    with mock.patch.object(builder.shutil, "which", return_value="ctest"), \
                            mock.patch.object(builder, "run_command", side_effect=check):
                        with self.assertRaises(InterruptedError):
                            builder.test_existing_installer_build(root, build, "^fork-fixtures$")
                    expected = ["configure", "check"][:phase + (boundary == "after")]
                    self.assertEqual(effects, expected)
                    with mock.patch.object(builder.shutil, "which", return_value="ctest"), \
                            mock.patch.object(builder, "run_command", return_value="") as runner:
                        first = builder.test_existing_installer_build(root, build, "^fork-fixtures$")
                        self.assertEqual(builder.test_existing_installer_build(root, build, "^fork-fixtures$"), first)
                        self.assertEqual(runner.call_count, 4)
                        configuration = runner.call_args_list[0].args[0]
                        self.assertIn("-DPython3_EXECUTABLE:FILEPATH=" + str(builder.get_native_test_python()), configuration)
                    self.assertEqual(first["status"], "passed")
                    self.assertEqual(installer.read_bytes(), b"accepted installer")

    def test_native_runtime_binding_rejects_missing_base_without_using_overlay(self):
        with tempfile.TemporaryDirectory(prefix="_runtime-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            base = Path(temporary)
            with mock.patch.object(builder.sys, "base_prefix", str(base)):
                with self.assertRaisesRegex(RuntimeError, "persistent native-test Python"):
                    builder.get_native_test_python()
                executable = base / ("python.exe" if os.name == "nt" else "bin/python3")
                executable.parent.mkdir(parents=True, exist_ok=True)
                executable.write_bytes(b"stable runtime")
                self.assertEqual(builder.get_native_test_python(), executable.resolve())

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
        for boundary in range(1, 13):
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
                        if "--before-package" in arguments:
                            result = json.dumps(self.native_report(build))
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
                        self.assertEqual(first["checks"], [{"name": "fork-fixtures", "result": "passed",
                                                           "source_revision": "a" * 40}])

    def test_failed_media_check_cannot_package_an_installer(self):
        with tempfile.TemporaryDirectory(prefix="_installer-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            source, build = root / "source", root / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text("project(test)")
            inno = root / "ISCC.exe"
            inno.write_bytes(b"test compiler")
            commands = []

            def run(arguments, _source, _diagnostics):
                commands.append(arguments)
                if "--before-package" in arguments:
                    raise RuntimeError("encoded-media check failed")
                return "a" * 40 + "\n" if arguments[1:3] == ["rev-parse", "HEAD"] else ""

            with mock.patch.object(builder, "prepare_sdk", return_value=root), \
                    mock.patch.object(builder.shutil, "which", return_value="cmake"), \
                    mock.patch.object(builder, "run_command", side_effect=run), \
                    mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}):
                with self.assertRaisesRegex(RuntimeError, "encoded-media check failed"):
                    builder.build_installer(source, build, inno)
            self.assertFalse(any("innosetup" in arguments for arguments in commands))
            self.assertFalse((build / "package/_build-manifest.json").exists())

    def test_desktop_gate_blocks_packaging_on_failure_missing_acceptance_and_changed_payload(self):
        for fault in ("desktop failure", "missing check", "changed payload", "changed source"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory(prefix="_gate-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
                root = Path(temporary)
                source, build = root / "source", root / "build"
                source.mkdir()
                (source / "CMakeLists.txt").write_text("project(test)")
                inno = root / "ISCC.exe"
                inno.write_bytes(b"compiler")
                commands: list[list[str]] = []
                checked = False

                def run(arguments, _source, _diagnostics, commands=commands, fault=fault, build=build):
                    nonlocal checked
                    commands.append(arguments)
                    if arguments[1:3] == ["rev-parse", "HEAD"]:
                        return "a" * 40
                    if arguments[1:3] == ["diff", "--binary"]:
                        return "source changed" if checked and fault == "changed source" else ""
                    if "--before-package" in arguments:
                        checked = True
                        if fault == "desktop failure":
                            raise RuntimeError("GUI input unavailable")
                        report = self.native_report(build)
                        if fault == "missing check":
                            report["checks"].pop()
                        if fault == "changed payload":
                            (build / "Release/AuTopsy.exe").write_bytes(b"untested host")
                        return json.dumps(report)
                    return ""

                with mock.patch.object(builder, "prepare_sdk", return_value=root), \
                        mock.patch.object(builder.shutil, "which", return_value="cmake"), \
                        mock.patch.object(builder, "run_command", side_effect=run), \
                        mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}):
                    with self.assertRaises(RuntimeError):
                        builder.build_installer(source, build, inno)
                self.assertFalse(any("innosetup" in arguments for arguments in commands))
                self.assertFalse((build / "package/_build-manifest.json").exists())

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

    def test_release_lookup_accepts_only_expected_absence(self):
        with tempfile.TemporaryDirectory(prefix="_release-", dir=os.environ.get("AUTOPSY_TEST_TEMP_ROOT")) as temporary:
            root = Path(temporary)
            diagnostics = root / "_failure.json"
            command = ["gh", "api", "repos/Ceratops-Code/AuTopsy/git/ref/tags/missing"]
            for status, absent in ((404, True), (403, False)):
                failed = subprocess.CompletedProcess(command, 1, "", f"gh: request failed (HTTP {status})")
                with mock.patch.object(builder, "ensure_process_tree_cleanup"), \
                        mock.patch.object(builder.subprocess, "run", return_value=failed):
                    if absent:
                        self.assertEqual(builder.run_command(command, root, diagnostics, missing_ok=True), "")
                    else:
                        with self.assertRaises(RuntimeError):
                            builder.run_command(command, root, diagnostics, missing_ok=True)
                        self.assertIn("HTTP 403", json.loads(diagnostics.read_text())["stderr"])


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
