"""Own private native application processes, profiles and execution boundaries.

The caller supplies the scenario/receipt API. Only this adapter knows Windows
process handles and the installed application's scripting/profile contract.
No user profile, running editor or unrelated output directory is owned here.
"""
import ctypes
import hashlib
import inspect
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

from run_audacity_consumer import (
    executable_version, read_audio_project_state, read_compatibility_fixture_inputs,
    run_process, select_audio_project_tracks, startup_window_messages,
)
from run_fork_test_harness import (
    NativeCrashDebugger, assert_owned_test_display, connected_test_displays, require_test_display_guard,
    run_behavior_worker, select_test_display,
    write_neutral_preview_video, write_tone,
)
from test_beginner_edits import BEGINNER_VIDEO_CASES
from test_video_recording import VIDEO_RECORDING_CASES


def native_test_inputs(args, executable, module, display, case_api):
    """Read affecting files and scope; Git HEAD and unrelated docs are not inputs.

    The media recipe is owned by these check modules and the exact FFmpeg
    tools. Runtime DLLs are read too: an unchanged EXE alone is insufficient.
    No recorded scope is inferred for historical acceptance records.
    """
    def sha(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    here = Path(__file__).parent
    inputs = {
        "executable_sha256": sha(executable),
        "script_module_sha256": sha(module),
        "check_sha256": sha(Path(case_api.__file__)),
        "session_checks_sha256": sha(Path(__file__)),
        "harness_sha256": sha(here / "run_fork_test_harness.py"),
        "pipe_client_sha256": sha(here / "pipeclient.py"),
        "shared_process_checks": {
            operation.__name__: hashlib.sha256(inspect.getsource(operation).encode()).hexdigest()
            for operation in (read_audio_project_state, select_audio_project_tracks,
                              executable_version, startup_window_messages, run_process)
        },
        "payload_dependencies": {p.name: sha(p) for p in sorted(executable.parent.glob("*.dll"))},
        "execution_scope": {
            "platform": platform.platform(), "machine": platform.machine(),
            "python": sys.version, "python_executable_sha256": sha(Path(sys.executable)),
            "test_display": display,
            "tools": {name: sha(Path(shutil.which(value) or value).resolve(strict=True))
                      for name in ("ffmpeg", "ffprobe") if (value := getattr(args, name))},
        },
    }
    if args.case in ("video-preview-close", "video-audio-parity", "video-audio-join-links", "video-split-edges", "video-linked-split-edges", "video-clip-edits",
                     "video-linked-clip-edits", "video-compound-render", "video-transport",
                     "video-visibility", "video-clip-mute", "video-speed", "video-speed-storage",
                     "video-clip-storage", "video-parent-links", "video-embedding", "video-embedding-controls", "video-crop", "video-crop-controls", "transport-toolbar", "preview-navigation"):
        inputs["clip_edits_sha256"] = sha(here / "test_video_clip_edits.py")
    if args.case in ("clip-actions", "video-clip-export", "edit-cursor-line", *case_api.RECORDING_CASES):
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["clip_edits_sha256"] = sha(here / "test_video_clip_edits.py")
    if args.case == "track-groups":
        inputs["track_groups_sha256"] = sha(here / "test_track_groups.py")
    if args.case == "product-updates":
        inputs["product_updates_sha256"] = sha(here / "test_product_updates.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case in case_api.GENERATOR_CASES:
        inputs["video_generators_sha256"] = sha(here / "test_video_generators.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case in case_api.CAPTION_CASES:
        inputs.update(case_api.caption_test_inputs(args.case, executable))
    if args.case in VIDEO_case_api.RECORDING_CASES:
        inputs["video_recording_sha256"] = sha(here / "test_video_recording.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["beginner_edits_sha256"] = sha(here / "test_beginner_edits.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case == "clip-selection":
        inputs["clip_selection_sha256"] = sha(here / "test_clip_selection.py")
    if args.case in BEGINNER_VIDEO_CASES:
        inputs["beginner_edits_sha256"] = sha(here / "test_beginner_edits.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["clip_edits_sha256"] = sha(here / "test_video_clip_edits.py")
    if args.case in ("video-export-timing", "video-vfr-export", "video-encoder-boundaries"):
        inputs["export_timing_sha256"] = sha(here / "test_video_export_timing.py")
    if args.case in ("preview-controls", "preview-quality", "video-cursor", "preview-soak"):
        inputs["preview_controls_sha256"] = sha(here / "test_preview_controls.py")
        inputs["clip_edits_sha256"] = sha(here / "test_video_clip_edits.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
    if "clip_edits_sha256" in inputs or "export_timing_sha256" in inputs:
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case == "preview-soak":
        from test_preview_controls import preview_soak_seconds
        inputs["execution_scope"]["soak_seconds"] = preview_soak_seconds()
        inputs["execution_scope"]["preview_metric_contract"] = "bounded-sample-window.v1"
    if args.case == "product-updates":
        source = os.environ.get("AUTOPSY_TEST_UPDATES_SOURCE", "fixture")
        if source not in ("fixture", "github"):
            raise RuntimeError("Choose fixture or github for the update test's request source.")
        inputs["execution_scope"]["updates_source"] = source
    if args.case in case_api.RECORDING_CASES:
        from test_clip_actions import require_recording_device
        inputs["execution_scope"]["recording_device"] = require_recording_device()
    if args.case == "video-encoder-boundaries":
        from test_video_export_timing import video_hardware_scope
        inputs["execution_scope"]["video_hardware"] = video_hardware_scope()
    if args.case == "display-guard":
        inputs["execution_scope"]["display_handle_probe"] = "stale-missing-and-interrupted-enumeration.v3"
    if debugger := os.environ.get("AUTOPSY_TEST_NATIVE_DEBUGGER"):
        inputs["execution_scope"]["native_debugger_sha256"] = sha(Path(debugger).resolve(strict=True))
    return inputs


def require_unlocked_desktop(kernel=None, sessions=None):
    """Reject fresh GUI work on a locked/disconnected or unreadable session.

    WTSSessionInfoEx has an eight-byte-aligned union; only its documented level-1
    prefix is read. This is an environmental precondition, not an app failure.
    Existing accepted results need no desktop. A later lock is still caught by
    the input target guards, since Windows can change state after this query.
    """
    class SessionPrefix(ctypes.Structure):
        _fields_ = [("level", ctypes.c_uint32), ("padding", ctypes.c_uint32),
                    ("session", ctypes.c_uint32), ("state", ctypes.c_uint32),
                    ("flags", ctypes.c_int32)]

    kernel = kernel or ctypes.WinDLL("kernel32", use_last_error=True)
    sessions = sessions or ctypes.WinDLL("wtsapi32", use_last_error=True)
    kernel.ProcessIdToSessionId.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    session = ctypes.c_uint32()
    if not kernel.ProcessIdToSessionId(kernel.GetCurrentProcessId(), ctypes.byref(session)):
        raise RuntimeError(f"Cannot read the GUI test session: Windows error {ctypes.get_last_error()}.")
    sessions.WTSQuerySessionInformationW.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
    sessions.WTSFreeMemory.argtypes = [ctypes.c_void_p]
    sessions.WTSFreeMemory.restype = None
    pointer, size = ctypes.c_void_p(), ctypes.c_uint32()
    if not sessions.WTSQuerySessionInformationW(None, session.value, 25,
            ctypes.byref(pointer), ctypes.byref(size)):
        raise RuntimeError(f"Cannot read GUI session {session.value}: Windows error {ctypes.get_last_error()}.")
    try:
        if not pointer.value or size.value < ctypes.sizeof(SessionPrefix):
            raise RuntimeError("Windows returned incomplete GUI session information.")
        info = SessionPrefix.from_buffer_copy(ctypes.string_at(pointer, ctypes.sizeof(SessionPrefix)))
        if info.level != 1 or info.session != session.value or info.state != 0 or info.flags != 1:
            raise RuntimeError(f"GUI session {session.value} is locked, disconnected or unknown "
                f"(level={info.level}, state={info.state}, flags={info.flags}); unlock Windows before fresh desktop checks.")
    finally:
        sessions.WTSFreeMemory(pointer)


def remove_private_capture_audio(root, *, process_stopped):
    """Delete only this stopped case's captured samples, including SQLite scratch.

    A repeated call after an interrupted deletion removes only what remains.
    The case marker and unrelated fixtures/results are never cleanup targets.
    """
    if not process_stopped:
        raise RuntimeError("Captured samples cannot be removed while the owned process is live.")
    marker = root / "_owner"
    if (root.resolve() != root.absolute() or marker.is_symlink() or not marker.is_file()
            or marker.read_text() != "AuTopsy behavior tests 1"):
        raise RuntimeError("Captured sample cleanup requires this case's unchanged owner marker.")
    audio = root / "_audio_temp"
    if not audio.exists():
        return
    if audio.resolve() != audio.absolute() or not audio.is_dir():
        raise RuntimeError("Captured sample cleanup refuses a redirected or foreign audio directory.")
    shutil.rmtree(audio)


RECORDING_CASES = ("cursor-recording", "cursor-recording-save-before", "cursor-recording-save-after")
# Feature modules register these cases in their own CMake directory. They remain
# normal CLI operations but must not also enter video's generated CTest list.
MODULE_REGISTERED_NATIVE_CASES = ("track-groups",)
NATIVE_CASES = (
    "product-updates",
    *GENERATOR_CASES,
    *VIDEO_RECORDING_CASES,
    "video-audio-parity",
    "video-audio-join-links",
    "video-preview-close",
    *BEGINNER_VIDEO_CASES,
    "display-guard",
    "groups",
    "tracks",
    "history-view",
    "transport-toolbar",
    "preview-navigation",
    "preview-controls", "preview-quality", "video-cursor", "preview-soak",
    "clip-actions", "clip-selection", "video-clip-export", "video-clip-mute", "edit-cursor-line",
    "project-compatibility",
    "video",
    "video-selection", "video-edges", "video-linked-edges", "video-clipboard", "video-track-menu",
    "video-split-edges", "video-linked-split-edges", "video-clip-edits", "video-linked-clip-edits", "video-compound-render",
    "video-transport", "video-visibility", "video-speed", "video-speed-storage", "video-clip-storage", "video-parent-links", "video-embedding", "video-embedding-controls", "video-crop", "video-crop-controls", "video-export-timing", "video-vfr-export", "video-encoder-boundaries",
    "media",
    "preview",
    "branding",
    "fixtures",
)


def execute_isolated_case(args, root, executable, module, display, kernel, pipe, case_api, run_id, identities):
    """Run one fresh-process scenario under the caller's existing profile mutex."""
    require_unlocked_desktop()
    marker = root / "_owner"
    signature = "AuTopsy behavior tests 1"
    shared_cache = root.parent / "_preview_media"
    cache_owner = shared_cache / "_test_owner"
    cache_signature = "AuTopsy preview behavior cache 1"
    if shared_cache.exists() and (
        not cache_owner.is_file() or cache_owner.read_text() != cache_signature
    ):
        raise RuntimeError(
            "The preview test cache has another owner: " + str(shared_cache)
        )
    if root.exists():
        if not marker.is_file() or marker.read_text() != signature:
            raise RuntimeError(
                "The test output directory has another owner: " + str(root)
            )
        previous_cache = root / "_preview-cache"
        if (
            args.case == "preview"
            and previous_cache.is_dir()
            and not shared_cache.exists()
        ):
            shutil.copytree(previous_cache, shared_cache)
            cache_owner.write_text(cache_signature)
        shutil.rmtree(root)
    root.mkdir()
    marker.write_text(signature)
    for name, channels in (
        ("tone-a.wav", 1),
        ("tone-b.wav", 1),
        ("tone-stereo.wav", 2),
    ):
        write_tone(root / name, channels)
    if args.case in BEGINNER_VIDEO_CASES or args.case in ("video-preview-close", "video-audio-parity", "video-audio-join-links", "clip-selection", "video-clip-export", "edit-cursor-line", "video", "video-selection", "video-edges", "video-linked-edges", "video-split-edges", "video-linked-split-edges", "video-clip-edits", "video-linked-clip-edits", "video-compound-render", "video-clipboard", "video-track-menu", "video-transport", "video-visibility", "video-speed", "video-speed-storage", "video-clip-storage", "video-parent-links", "media", "project-compatibility"):
        run_process(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "lavfi",
                "-i",
                "color=c=red:s=160x90:r=30:d=2",
                "-f",
                "lavfi",
                "-i",
                "color=c=blue:s=160x90:r=30:d=2",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=48000:duration=4",
                "-filter_complex",
                "[0:v][1:v]concat=n=2:v=1:a=0[v]",
                "-map",
                "[v]",
                "-map",
                "2:a",
                "-c:v",
                "mpeg4",
                "-q:v",
                "2",
                "-c:a",
                "aac",
                "-shortest",
                str(root / "source.mp4"),
            ],
            timeout=60,
        )
        run_process(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "lavfi",
                "-i",
                "color=c=yellow:s=90x160:r=25:d=2",
                "-an",
                "-c:v",
                "mpeg4",
                "-q:v",
                "2",
                str(root / "other-source.mp4"),
            ],
            timeout=30,
        )
    if args.case in ("preview", "transport-toolbar", "preview-navigation", "preview-controls", "video-cursor", "preview-soak"):
        write_neutral_preview_video(args.ffmpeg, root / "source.mp4")
    version_numbers = executable_version(executable)
    profile = executable.parent / "Portable Settings"
    profile_owner = profile / "_autopsy_tests"
    if profile.exists():
        if not profile_owner.is_file() or profile_owner.read_text() != str(
            args.work_root.resolve()
        ):
            raise RuntimeError(
                "An unrelated Portable Settings profile exists: " + str(profile)
            )
        shutil.rmtree(profile)
    profile.mkdir()
    profile_owner.write_text(str(args.work_root.resolve()))
    if args.case == "preview" and shared_cache.is_dir():
        shutil.copytree(shared_cache, profile / "_video-previews")
    audio = root / "_audio_temp"
    audio.mkdir()
    gui = root / "_gui_temp"
    gui.mkdir()
    version = ".".join(str(x) for x in version_numbers)
    stamp = datetime.fromtimestamp(module.stat().st_mtime).strftime("%Y-%m-%dT%H:%M:%S")  # noqa: DTZ006 -- matches the upstream module preference in local time
    left, top, right, bottom = display["work"]
    width, height = min(1100, right - left - 32), min(650, bottom - top - 32)
    placement = (
        f"[Window]\nX={left + 16}\nY={top + 16}\nWidth={width}\nHeight={height}\n"
        f"Normal_X={left + 16}\nNormal_Y={top + 16}\nNormal_Width={width}\nNormal_Height={height}\n"
        "Maximized=0\nIconized=0\n"
    )
    capture_configuration = ""
    if args.case in case_api.RECORDING_CASES:
        from test_clip_actions import require_recording_device
        device = require_recording_device()
        capture_configuration = (f"[AudioIO]\nHost=MME\nRecordingDevice={device['name']}\n"
            "RecordChannels=1\nLatencyCorrection=0\nSWPlaythrough=0\nDuplex=0\n")
    (profile / "autopsy.cfg").write_text(
        f"[Version]\nMajor={version_numbers[0]}\nMinor={version_numbers[1]}\nMicro={version_numbers[2]}\n"
        f"[Directories]\nTempDir={audio.as_posix()}\n[GUI]\nShowSplashScreen={int(args.case == 'branding')}\n"
        "[FileFormats]\nMusicFileImportSettingChoice=No\n[Module]\nmod-script-pipe=1\n"
        f"[ModuleDateTime]\nmod-script-pipe={stamp}\n" + placement + capture_configuration,
        encoding="utf-8",
    )
    environment = dict(
        os.environ,
        TEMP=str(gui),
        TMP=str(gui),
        PYTHONDONTWRITEBYTECODE="1",
        AUTOPSY_BEHAVIOR_ROOT=str(args.work_root.resolve()),
        AUTOPSY_TEST_FFMPEG=args.ffmpeg,
        AUTOPSY_TEST_DISPLAY=display["device"]
        if args.reject_display is not True
        else "missing-test-display",
    )
    if args.case == "product-updates":
        environment.pop("AUTOPSY_TEST_UPDATES_JSON", None)
        if os.environ.get("AUTOPSY_TEST_UPDATES_SOURCE", "fixture") == "fixture":
            environment["AUTOPSY_TEST_UPDATES_JSON"] = str(root / "_updates.json")
    initial_preferences = (profile / "autopsy.cfg").read_text(encoding="utf-8")
    environment.pop("AUTOPSY_TEST_STALE_MONITOR", None)
    if args.case == "display-guard":
        environment["AUTOPSY_TEST_STALE_MONITOR"] = "1"
    if args.reject_display == "transition":
        environment["AUTOPSY_TEST_STALE_MONITOR"] = "lost"
    if args.case in case_api.RECORDING_CASES[1:]:
        environment["AUTOPSY_TEST_SAVE_ROOT"] = str(audio.resolve())
    if args.case in VIDEO_RECORDING_CASES:
        environment["AUTOPSY_TEST_VIDEO_RECORDING_INPUT"] = "generated"
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    with (root / "_app.log").open("w") as log:
        app = subprocess.Popen(
            [str(executable)],
            cwd=executable.parent,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        debugger = None
        try:
            if not args.reject_display:
                debugger = NativeCrashDebugger(app, root)
            if args.reject_display:
                started = time.monotonic()
                status = app.wait(timeout=30)
                log.flush()
                report = (root / "_app.log").read_text(
                    encoding="utf-8", errors="replace"
                )
                if args.reject_display == "transition" and (
                        "AUTOPSY_TEST_DISPLAY bound-before-permanent-loss" not in report or
                        not 1.8 <= time.monotonic()-started < 15):
                    raise RuntimeError("Permanent monitor loss did not stop at its bounded placement boundary: " + report[-2000:])
                if (
                    status != 197
                    or "AUTOPSY_TEST_WINDOW show" in report
                    or "test display guard:" not in report
                ):
                    raise RuntimeError(
                        f"Unsafe-display rejection: owned app exit {status}; {report[-4000:]}"
                    )
                print(
                    "display-rejection: PASS (missing screen stopped before the first window)"
                )
                return
            startup = user.WaitForInputIdle(int(app._handle), 30000)  # type: ignore[attr-defined]  # CPython's Windows process handle.
            if startup != 0:
                raise RuntimeError(
                    f"Owned app startup status {startup}; PID {app.pid}; "
                    + startup_window_messages(app.pid)
                )
            deadline = time.monotonic() + 30
            while not kernel.WaitNamedPipeW(pipe, 100):
                if app.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(
                        f"The owned test app did not expose its scripting pipe; exit={app.poll()}; "
                        + startup_window_messages(app.pid)
                    )
                time.sleep(0.05)
            if app.poll() is not None:
                raise RuntimeError("The owned test app exited during startup.")
            environment["AUTOPSY_OWNED_APP_PID"] = str(app.pid)
            previous_display = os.environ.get("AUTOPSY_TEST_DISPLAY")
            os.environ["AUTOPSY_TEST_DISPLAY"] = display["device"]
            try:
                windows = assert_owned_test_display(app.pid)
            finally:
                if previous_display is None:
                    os.environ.pop("AUTOPSY_TEST_DISPLAY", None)
                else:
                    os.environ["AUTOPSY_TEST_DISPLAY"] = previous_display
            (root / "_display.json").write_text(
                json.dumps({"display": display, "startup_windows": windows}, indent=2)
            )
            worker_command = [
                sys.executable,
                str(Path(case_api.__file__)),
                *sys.argv[1:],
                "--worker",
            ]
            if args.interrupt_cache:
                cache = profile / "_video-previews"
                baseline = set(cache.rglob("_complete")) if cache.exists() else set()
                operation = subprocess.Popen(
                    worker_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=environment,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
                try:
                    deadline = time.monotonic() + 45
                    observed = None
                    while (
                        time.monotonic() < deadline
                        and app.poll() is None
                        and operation.poll() is None
                    ):
                        candidates = (
                            list(cache.rglob("_building.mp4"))
                            if args.interrupt_cache == "before-publish"
                            else [
                                path
                                for path in cache.rglob("_complete")
                                if path not in baseline
                            ]
                        )
                        if candidates:
                            observed = candidates[0]
                            break
                        time.sleep(0.005)
                    if observed is None:
                        raise RuntimeError(
                            "The preview did not reach the requested interruption boundary."
                        )
                    app.terminate()
                    app.wait(timeout=20)
                    print("Observed cache interruption: " + args.interrupt_cache)
                    return
                finally:
                    if operation.poll() is None:
                        operation.terminate()
                    output, errors = operation.communicate(timeout=10)
                    (root / "_commands.log").write_text(output, encoding="utf-8")
                    (root / "_errors.log").write_text(errors, encoding="utf-8")
            if args.case in case_api.RECORDING_CASES[1:]:
                from test_clip_actions import run_recording_save_faults

                def restart_recording_app(fault_enabled):
                    # Only stopped private test processes reach this boundary.
                    # Reset their recovery list, preserving the interrupted
                    # project/WAL in the private audio directory.
                    (profile / "autopsy.cfg").write_text(initial_preferences, encoding="utf-8")
                    next_environment = dict(environment)
                    if not fault_enabled:
                        next_environment.pop("AUTOPSY_TEST_SAVE_ROOT", None)
                    restarted = subprocess.Popen([str(executable)], cwd=executable.parent,
                        env=next_environment, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        if user.WaitForInputIdle(int(restarted._handle), 30000):  # type: ignore[attr-defined]
                            raise RuntimeError("Reopened private recording app did not become idle.")
                        deadline = time.monotonic() + 30
                        while not kernel.WaitNamedPipeW(pipe, 100):
                            if restarted.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError("Reopened private recording app has no scripting pipe.")
                            time.sleep(0.05)
                        return restarted
                    except BaseException:
                        if restarted.poll() is None:
                            restarted.terminate()
                            restarted.wait(timeout=10)
                        raise

                app = run_recording_save_faults(worker_command, environment, app, root,
                    restart_recording_app, lambda: remove_private_capture_audio(root, process_stopped=True))
                result = None  # Fault journeys checked each real child result.
            else:
                result = run_behavior_worker(worker_command, environment, app, root,
                    timeout_seconds=285 if args.case == "preview-soak" else 240)
            if result is not None and result.returncode:
                raise RuntimeError(
                    f"Command: {subprocess.list2cmdline(worker_command)}\n"
                    f"Behavior worker exit {result.returncode}\n"
                    f"stdout:\n{result.stdout[-14000:] or '<absent>'}\n"
                    f"stderr:\n{result.stderr or '<absent>'}"
                )
            try:
                status = app.wait(timeout=20)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    "Owned app did not exit; " + startup_window_messages(app.pid)
                ) from error
            if status:
                if debugger:
                    debugger.close()
                log.flush()
                trace = root / "_native-debugger.log"
                raise RuntimeError(f"The owned app exited with status {status}.\n"
                    f"Command: {subprocess.list2cmdline(worker_command)}\n"
                    f"Worker stdout:\n{result.stdout[-8000:] if result and result.stdout else '<absent>'}\n"
                    f"Worker stderr:\n{result.stderr if result and result.stderr else '<absent>'}\n"
                    f"App output:\n{(root / '_app.log').read_text(encoding='utf-8', errors='replace')[-8000:]}\n"
                    f"Native crash trace: {trace if trace.is_file() else '<absent>'}")
            log.flush()
            app_output = (root / "_app.log").read_text(encoding="utf-8", errors="replace")
            if args.case == "display-guard" and "AUTOPSY_TEST_DISPLAY rebound-stale-handle" not in app_output:
                raise RuntimeError("The native display guard did not exercise stale-handle replacement.")
            if args.case == "display-guard" and "AUTOPSY_TEST_DISPLAY waited-for-monitor" not in app_output:
                raise RuntimeError("The native display guard did not defer placement during a monitor transition.")
            shows = re.findall(
                r"^AUTOPSY_TEST_WINDOW show (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$",
                app_output,
                re.MULTILINE,
            )
            assert shows, "No pre-show placement observations were recorded."
            for _, x, y, w, h in shows:
                x, y, w, h = map(int, (x, y, w, h))
                assert left <= x and top <= y and x + w <= right and y + h <= bottom, (
                    shows
                )
            source = root / "source.mp4"
            source_identity = None
            if source.is_file():
                info = source.stat()
                source_identity = {
                    "source": "generated:" + args.case,
                    "bytes": info.st_size,
                    "modified_ns": None,
                }
            record = {
                "run": run_id,
                "case": args.case,
                "result": "passed",
                "version": version.strip(),
                "test_display": display,
                "pre_show_count": len(shows),
                "input": source_identity,
                **identities,
            }
            if args.case in ("project-compatibility", "track-groups", "video-clip-mute", "collect-media", "video-fades", "video-embedding", "video-pictures"):
                record["compatibility_fixtures"] = read_compatibility_fixture_inputs(root)
            observations = root / "_playback.json"
            if observations.is_file():
                record["observations"] = json.loads(observations.read_text())
            return record
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=20)
            if debugger:
                debugger.close()
            if args.case in case_api.RECORDING_CASES:
                remove_private_capture_audio(root, process_stopped=app.poll() is not None)
            if profile_owner.is_file() and profile_owner.read_text() == str(
                args.work_root.resolve()
            ):
                cache = profile / "_video-previews"
                if args.case == "preview" and cache.is_dir():
                    if shared_cache.exists():
                        shutil.rmtree(shared_cache)
                    shutil.copytree(cache, shared_cache)
                    cache_owner.write_text(cache_signature)
                shutil.rmtree(profile)




def run_requested_cases(args, root, case_api):
    """Resolve the private Windows boundary and apply the case's receipt owner."""
    if os.name != "nt":
        raise RuntimeError("The scripting integration cases require Windows.")
    executable = args.executable.resolve(strict=True)
    module = executable.parent / "modules/mod-script-pipe.dll"
    require_test_display_guard(module)
    display = select_test_display(connected_test_displays())
    print("GUI test display: " + display["device"], flush=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.WaitNamedPipeW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    kernel.CreateMutexW.restype = ctypes.c_void_p
    # All cases share the native app's pipe/profile boundary; refuse concurrent
    # callers before touching either, including separate manual CTest runs.
    mutex = kernel.CreateMutexW(None, True, "Local\\AuTopsyBehaviorTests")
    if not mutex or ctypes.get_last_error() == 183:
        raise RuntimeError("Another AuTopsy behavior-test runner is active.")
    pipe = r"\\.\pipe\AuTopsyTestToSrvPipe"
    if kernel.WaitNamedPipeW(pipe, 1) or ctypes.get_last_error() not in (2,):
        raise RuntimeError("An AuTopsy scripting session is already active.")
    root.parent.mkdir(parents=True, exist_ok=True)
    def execute(run_id, identities):
        return execute_isolated_case(args, root, executable, module, display, kernel, pipe,
            case_api, run_id, identities)

    if args.reject_display or args.interrupt_cache:
        case_api.retain_behavior_result(root.parent)
        execute(uuid.uuid4().hex, native_test_inputs(args, executable, module, display, case_api))
        if args.reject_display:
            # The same negative journey also loses an already bound display;
            # it must expire before the first Show rather than wait forever.
            args.reject_display = "transition"
            root = root.with_name(root.name + "_transition")
            execute(uuid.uuid4().hex, native_test_inputs(args, executable, module, display, case_api))
        return
    def read_inputs():
        return native_test_inputs(args, executable, module, display, case_api)

    if args.case in ("project-compatibility", "track-groups", "video-clip-mute", "collect-media", "video-fades", "video-embedding", "video-pictures"):
        result = case_api.run_project_compatibility_checks(
            root.parent, args.case, read_inputs, execute, root, display, args.ffmpeg)
    else:
        result = case_api.run_native_check(root.parent, args.case, read_inputs, execute)
    print(json.dumps(result))

