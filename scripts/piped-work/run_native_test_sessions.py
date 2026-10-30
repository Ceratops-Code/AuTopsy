"""Own private native application processes, profiles and execution boundaries.

The caller supplies the scenario/receipt API. Only this adapter knows Windows
process handles and the installed application's scripting/profile contract.
No user profile, running editor or unrelated output directory is owned here.
"""
import copy
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
import threading
import time
import uuid
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path

from prepare_test_media import (
    TestMediaCache,
    preserve_caption_models,
    seed_caption_models,
)
from run_audacity_consumer import (
    executable_version,
    read_audio_project_state,
    read_compatibility_fixture_inputs,
    run_process,
    select_audio_project_tracks,
    startup_window_messages,
)
from run_fork_test_harness import (
    NativeCrashDebugger,
    assert_owned_test_display,
    connected_test_displays,
    require_test_display_guard,
    run_behavior_worker,
    select_test_display,
    write_neutral_preview_video,
    write_tone,
)
from test_beginner_edits import BEGINNER_VIDEO_CASES
from test_clip_workflows import CLIP_WORKFLOW_CASES
from test_video_captions import CAPTION_CASES, caption_test_inputs
from test_video_generators import GENERATOR_CASES
from test_video_navigation import NAVIGATION_CASES
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
        "fixture_checks_sha256": sha(here / "prepare_test_media.py"),
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
    if args.case in ("clip-actions", "video-clip-export", "edit-cursor-line", *RECORDING_CASES):
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["clip_edits_sha256"] = sha(here / "test_video_clip_edits.py")
    if args.case == "track-groups":
        inputs["track_groups_sha256"] = sha(here / "test_track_groups.py")
    if args.case == "product-updates":
        inputs["product_updates_sha256"] = sha(here / "test_product_updates.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case in GENERATOR_CASES:
        inputs["video_generators_sha256"] = sha(here / "test_video_generators.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
    if args.case in CAPTION_CASES:
        inputs.update(caption_test_inputs(args.case, executable))
    if args.case in VIDEO_RECORDING_CASES:
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
    if args.case in CLIP_WORKFLOW_CASES:
        inputs["clip_workflows_sha256"] = sha(here / "test_clip_workflows.py")
        inputs["beginner_edits_sha256"] = sha(here / "test_beginner_edits.py")
    if args.case in NAVIGATION_CASES:
        inputs["video_navigation_sha256"] = sha(here / "test_video_navigation.py")
        inputs["beginner_edits_sha256"] = sha(here / "test_beginner_edits.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
        inputs["clip_workflows_sha256"] = sha(here / "test_clip_workflows.py")
        inputs["video_transport_sha256"] = sha(here / "test_video_transport.py")
        inputs["clip_actions_sha256"] = sha(here / "test_clip_actions.py")
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
    if args.case in RECORDING_CASES:
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
    *CLIP_WORKFLOW_CASES,
    *NAVIGATION_CASES,
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


SHARED_PROJECT_CASES = frozenset({
    *CLIP_WORKFLOW_CASES,
    *NAVIGATION_CASES,
    "video-reverse", "video-freeze", "video-repeat", "video-text",
    "video-caption-interchange", "video-caption-hebrew", "video-caption-warp",
    "video-selection", "video-edges", "video-linked-edges", "video-split-edges",
    "video-linked-split-edges", "video-clip-edits", "video-linked-clip-edits",
    "video-compound-render", "video-crop", "video-crop-controls", "video-visibility",
    "video-speed", "video-speed-storage", "video-clip-storage", "video-parent-links",
    "video-track-menu",
})
RESET_PREFERENCES = {"/FileFormats/MusicFileImportSettingChoice": "No", "/GUI/ShowSplashScreen": "0"}
_native_job = None


def contain_native_children():
    """Let Windows stop only this runner's descendants if the runner is killed.

    The non-inheritable job handle stays open for the process lifetime. Closing
    it in Python finalization could kill the runner before its exit code is
    reported, so the kernel closes it after process termination instead.
    """
    global _native_job
    if os.name != "nt" or _native_job is not None:
        return

    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", ctypes.c_uint32), ("minimum", ctypes.c_size_t),
                    ("maximum", ctypes.c_size_t), ("active", ctypes.c_uint32),
                    ("affinity", ctypes.c_size_t), ("priority", ctypes.c_uint32),
                    ("scheduling", ctypes.c_uint32)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    kernel.CreateJobObjectW.restype = ctypes.c_void_p
    kernel.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    kernel.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = ExtendedLimits()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if (not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits))
            or not kernel.AssignProcessToJobObject(job, kernel.GetCurrentProcess())):
        error = ctypes.get_last_error()
        kernel.CloseHandle(job)
        raise RuntimeError(f"Cannot contain the native test process tree: Windows error {error}.")
    _native_job = job


def stop_owned_native_children():
    """Retire leftover media workers before reusing this runner's profile.

    This runner's private job contains no user process. Membership is checked
    again on each opened process handle to exclude PID reuse. The controller
    survives, so a fresh app can run remaining scenarios after a failed worker.
    """
    if _native_job is None:
        return
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    deadline = time.monotonic() + 10
    while True:
        capacity = 64
        while True:
            class ProcessList(ctypes.Structure):
                _fields_ = [("assigned", ctypes.c_uint32), ("listed", ctypes.c_uint32),
                            ("ids", ctypes.c_size_t * capacity)]

            processes = ProcessList()
            if kernel.QueryInformationJobObject(_native_job, 3, ctypes.byref(processes),
                                               ctypes.sizeof(processes), None):
                break
            error = ctypes.get_last_error()
            if error != 234 or capacity >= 4096:
                raise RuntimeError(f"Cannot read owned test job members: Windows error {error}.")
            capacity *= 2
        pids = [pid for pid in processes.ids[:processes.listed] if pid != os.getpid()]
        if not pids:
            return
        for pid in pids:
            handle = kernel.OpenProcess(0x101001, False, pid)
            if not handle:
                if ctypes.get_last_error() == 87:
                    continue  # This job member already exited.
                raise RuntimeError(f"Cannot open owned test child {pid}: Windows error {ctypes.get_last_error()}.")
            try:
                member = ctypes.c_int()
                if not kernel.IsProcessInJob(handle, _native_job, ctypes.byref(member)):
                    raise RuntimeError(f"Cannot read job ownership for child {pid}.")
                if not member.value:
                    continue
                if kernel.WaitForSingleObject(handle, 0) != 0 and not kernel.TerminateProcess(handle, 1):
                    raise RuntimeError(f"Cannot stop owned test child {pid}: Windows error {ctypes.get_last_error()}.")
                remaining = max(0, int((deadline - time.monotonic()) * 1000))
                if kernel.WaitForSingleObject(handle, remaining) != 0:
                    raise RuntimeError(f"Owned test child {pid} did not finish teardown.")
            finally:
                kernel.CloseHandle(handle)
        if time.monotonic() >= deadline:
            raise RuntimeError("Owned test children did not retire within ten seconds.")


def session_case_order(cases):
    """Group compatible cases; unknown or repeated requests have no effects."""
    if not cases or len(cases) != len(set(cases)):
        raise ValueError("Choose a nonempty list of unique native scenarios.")
    known = set(NATIVE_CASES) | set(MODULE_REGISTERED_NATIVE_CASES) | set(RECORDING_CASES)
    if set(cases)-known or "fixtures" in cases:
        raise ValueError("The session contains an unknown or headless scenario.")
    return ([case for case in cases if case in SHARED_PROJECT_CASES]
            + [case for case in cases if case not in SHARED_PROJECT_CASES])


def owned_project_windows(pid):
    """Identify project frames by their native menu, never title or position."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetMenu.argtypes = [ctypes.c_void_p]
    user.GetMenu.restype = ctypes.c_void_p
    return [int(window["handle"]) for window in assert_owned_test_display(pid)
            if user.GetMenu(int(window["handle"]))]


def read_history_availability(client, window):
    """Read native menu states using current scripting IDs and translated labels.

    Only the two requested fields are read from each menu row: the inherited
    serializer can emit invalid JSON escapes in unrelated shortcut strings.
    No command is invoked and no menu is opened to inspect its enabled state.
    """
    labels = {}
    for line in client.send("GetInfo: Type=Menus Format=JSON").splitlines():
        identity = re.search(r'"id"\s*:\s*"(Undo|Redo)"', line)
        label = re.search(r'"label"\s*:\s*("(?:[^"\\]|\\.)*")', line)
        if identity and label:
            if identity[1] in labels:
                raise RuntimeError("History commands have ambiguous menu entries.")
            labels[identity[1]] = json.loads(label[1])
    if set(labels) != {"Undo", "Redo"}:
        raise RuntimeError("The native menu did not expose both history command IDs.")
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetMenu.argtypes, user.GetMenu.restype = [ctypes.c_void_p], ctypes.c_void_p
    user.GetSubMenu.argtypes, user.GetSubMenu.restype = [ctypes.c_void_p, ctypes.c_int], ctypes.c_void_p
    user.GetMenuItemCount.argtypes = [ctypes.c_void_p]
    user.GetMenuStringW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_uint]
    user.GetMenuState.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
    result = {}

    def visit(menu):
        for index in range(user.GetMenuItemCount(menu)):
            text = ctypes.create_unicode_buffer(512)
            user.GetMenuStringW(menu, index, text, len(text), 0x400)
            caption = text.value.replace("&", "").split("\t")[0]
            for command, label in labels.items():
                if caption == label:
                    if command in result:
                        raise RuntimeError("History controls have ambiguous native menu entries.")
                    state = user.GetMenuState(menu, index, 0x400)
                    if state == -1:
                        raise RuntimeError("The native history menu state could not be read.")
                    result[command] = not bool(state & 3)
            child = user.GetSubMenu(menu, index)
            if child:
                visit(child)

    visit(user.GetMenu(window))
    return result


def require_empty_project(client):
    """Observe a new project and restore the shared scenarios' preference keys."""
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    windows = owned_project_windows(pid)
    if len(windows) != 1:
        raise RuntimeError("Shared scenarios require exactly one owned project window.")
    client.send("Stop")
    for kind in ("Tracks", "Clips", "Labels"):
        if client.data(f"GetInfo: Type={kind} Format=JSON"):
            raise RuntimeError("The shared project still contains " + kind.lower())
    if read_history_availability(client, windows[0]) != {"Undo": False, "Redo": False}:
        raise RuntimeError("The shared project still has history or unreadable history controls.")
    client.send("SelectTime: Start=0 End=0")
    for name, value in RESET_PREFERENCES.items():
        client.send(f'SetPreference: Name="{name}" Value="{value}"')
        observed = client.send(f'GetPreference: Name="{name}"').split("BatchCommand", 1)[0].strip()
        if observed != value:
            raise RuntimeError("The shared preference did not reach its baseline: " + name)


class SharedNativeProcess:
    """Own one live app, a two-MiB log tail and its profile until retirement."""

    def __init__(self, work_root, executable):
        self.work_root, self.executable = work_root, executable
        self.app = self.log_thread = self.debugger = None
        self.environment = None
        self.profile = None
        self.current_root = None
        self.tail = bytearray()
        self.tail_lock = threading.Lock()
        self.starts = 0

    @contextmanager
    def output(self, root):
        self.current_root = root
        (root / "_app.log").write_bytes(b"")
        try:
            yield self
        finally:
            self.flush()

    def flush(self):
        if self.current_root is not None:
            with self.tail_lock:
                data = bytes(self.tail)
            (self.current_root / "_app.log").write_bytes(data)

    def bind(self, app, environment, profile):
        self.app, self.environment, self.profile = app, environment, profile
        self.starts += 1
        self.tail.clear()

        def drain():
            try:
                while block := app.stdout.read1(8192):
                    with self.tail_lock:
                        self.tail.extend(block)
                        if len(self.tail) > 2 * 1024 * 1024:
                            del self.tail[:-2 * 1024 * 1024]
            finally:
                app.stdout.close()

        self.log_thread = threading.Thread(target=drain, daemon=True)
        self.log_thread.start()

    def close(self, *, force=False):
        """Stop this app, retain complete models, then delete its owned profile."""
        app = self.app
        if app is None:
            return
        errors = []
        try:
            if app.poll() is None and not force:
                windows = owned_project_windows(app.pid)
                if len(windows) != 1:
                    raise RuntimeError("The shared session cannot close an ambiguous project set.")
                user = ctypes.WinDLL("user32", use_last_error=True)
                user.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
                if not user.PostMessageW(windows[0], 0x10, 0, 0):
                    raise RuntimeError("Closing the owned shared test window failed.")
                if app.wait(timeout=20):
                    raise RuntimeError("The shared test application failed during shutdown.")
        except Exception as error:
            errors.append(str(error))
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=20)
            self.app = None  # Ownership ends only after the child is known stopped.
            if self.debugger:
                try:
                    self.debugger.close()
                except Exception as error:
                    errors.append(str(error))
                finally:
                    self.debugger = None
            stop_owned_native_children()
            if self.log_thread:
                self.log_thread.join(timeout=10)
                if self.log_thread.is_alive():
                    errors.append("The owned application log did not finish.")
            self.flush()
            if self.profile is not None:
                owner = self.profile / "_autopsy_tests"
                if (self.profile.is_symlink() or self.profile.resolve() != self.profile.absolute()
                        or not owner.is_file() or owner.read_text() != str(self.work_root.resolve())):
                    raise RuntimeError("The shared test profile no longer has its expected owner.")
                try:
                    preserve_caption_models(TestMediaCache(self.work_root / "_generated-media"),
                                            self.profile, self.executable)
                except Exception as error:
                    errors.append(str(error))
                finally:
                    shutil.rmtree(self.profile)
                    self.profile = None
        if errors:
            raise RuntimeError("Shared application retirement failed: " + "; ".join(errors))


def build_case_media(args, destination):
    """Produce neutral inputs for one media shape; scenarios only get copies."""
    for name, channels in (
        ("tone-a.wav", 1),
        ("tone-b.wav", 1),
        ("tone-stereo.wav", 2),
    ):
        write_tone(destination / name, channels)
    if args.case in (*BEGINNER_VIDEO_CASES, *CLIP_WORKFLOW_CASES, *NAVIGATION_CASES) or args.case in ("video-preview-close", "video-audio-parity", "video-audio-join-links", "clip-selection", "video-clip-export", "edit-cursor-line", "video", "video-selection", "video-edges", "video-linked-edges", "video-split-edges", "video-linked-split-edges", "video-clip-edits", "video-linked-clip-edits", "video-compound-render", "video-clipboard", "video-track-menu", "video-transport", "video-visibility", "video-speed", "video-speed-storage", "video-clip-storage", "video-parent-links", "media", "project-compatibility"):
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
                str(destination / "source.mp4"),
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
                str(destination / "other-source.mp4"),
            ],
            timeout=30,
        )
    if args.case in ("preview", "transport-toolbar", "preview-navigation", "preview-controls", "video-cursor", "preview-soak"):
        write_neutral_preview_video(args.ffmpeg, destination / "source.mp4")

def prepare_case_media(args, root):
    """Reuse exact recipe/tool bytes; application acceptance is independent."""
    small = args.case in (*BEGINNER_VIDEO_CASES, *CLIP_WORKFLOW_CASES, *NAVIGATION_CASES) or args.case in (
        "video-preview-close", "video-audio-parity", "video-audio-join-links",
        "clip-selection", "video-clip-export", "edit-cursor-line", "video",
        "video-selection", "video-edges", "video-linked-edges", "video-split-edges",
        "video-linked-split-edges", "video-clip-edits", "video-linked-clip-edits",
        "video-compound-render", "video-clipboard", "video-track-menu",
        "video-transport", "video-visibility", "video-speed", "video-speed-storage",
        "video-clip-storage", "video-parent-links", "media", "project-compatibility")
    preview = args.case in ("preview", "transport-toolbar", "preview-navigation",
                           "preview-controls", "video-cursor", "preview-soak")
    purpose = "preview" if preview else "small-video" if small else "tones"
    tools = {}
    if small or preview:
        executable = Path(shutil.which(args.ffmpeg) or args.ffmpeg).resolve(strict=True)
        with executable.open("rb") as stream:
            tools["ffmpeg"] = hashlib.file_digest(stream, "sha256").hexdigest()
    inputs = {"tools": tools, "recipes": {
        fn.__name__: hashlib.sha256(inspect.getsource(fn).encode()).hexdigest()
        for fn in (build_case_media, write_tone, write_neutral_preview_video)}}
    cache = TestMediaCache(root.parent / "_generated-media")
    group = cache.materialize(purpose, inputs, lambda target: build_case_media(args, target))
    cache.copy_to(group, root)


def execute_case(args, root, executable, module, display, kernel, pipe, case_api, run_id, identities, shared=None):
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
    prepare_case_media(args, root)
    version_numbers = executable_version(executable)
    version = ".".join(str(x) for x in version_numbers)
    profile = executable.parent / "Portable Settings"
    profile_owner = profile / "_autopsy_tests"
    left, top, right, bottom = display["work"]
    if shared is None or shared.app is None:
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
        stamp = datetime.fromtimestamp(module.stat().st_mtime).strftime("%Y-%m-%dT%H:%M:%S")  # noqa: DTZ006 -- matches the upstream module preference in local time
        left, top, right, bottom = display["work"]
        width, height = min(1100, right - left - 32), min(650, bottom - top - 32)
        placement = (
            f"[Window]\nX={left + 16}\nY={top + 16}\nWidth={width}\nHeight={height}\n"
            f"Normal_X={left + 16}\nNormal_Y={top + 16}\nNormal_Width={width}\nNormal_Height={height}\n"
            "Maximized=0\nIconized=0\n"
        )
        capture_configuration = ""
        if args.case in RECORDING_CASES:
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
        if args.case in RECORDING_CASES[1:]:
            environment["AUTOPSY_TEST_SAVE_ROOT"] = str(audio.resolve())
        if args.case in VIDEO_RECORDING_CASES:
            environment["AUTOPSY_TEST_VIDEO_RECORDING_INPUT"] = "generated"
        if shared is not None:
            seed_caption_models(TestMediaCache(args.work_root / "_generated-media"), profile, executable)
    else:
        environment = shared.environment
        assert environment is not None
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    with (shared.output(root) if shared else (root / "_app.log").open("w")) as log:
        if shared is not None and shared.app is not None:
            app, debugger = shared.app, shared.debugger
        else:
            app = subprocess.Popen(
                [str(executable)], cwd=executable.parent, env=environment,
                stdout=subprocess.PIPE if shared else log, stderr=subprocess.STDOUT)
            debugger = None
            if shared is not None:
                shared.bind(app, environment, profile)
        try:
            if not args.reject_display and debugger is None:
                debugger = NativeCrashDebugger(app, root)
                if shared is not None:
                    shared.debugger = debugger
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
                "--executable", str(executable), "--work-root", str(args.work_root),
                "--case", args.case, "--ffmpeg", args.ffmpeg, "--ffprobe", args.ffprobe,
                "--worker",
                *(["--keep-app-open"] if shared is not None else []),
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
            if args.case in RECORDING_CASES[1:]:
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
                if shared is not None:
                    status = app.poll()
                    if status is None:
                        status = 0
                    else:
                        raise RuntimeError("A reusable application exited between scenarios.")
                else:
                    status = app.wait(timeout=20)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    "Owned app did not exit; " + startup_window_messages(app.pid)
                ) from error
            if status:
                if debugger:
                    debugger.close()
                stop_owned_native_children()
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
                "session": {"pid": app.pid, "shared": shared is not None,
                            "process_starts": shared.starts if shared else 1},
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
            if shared is None:
                if app.poll() is None:
                    app.terminate()
                    app.wait(timeout=20)
                if debugger:
                    debugger.close()
                stop_owned_native_children()
                if args.case in RECORDING_CASES:
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




def run_case_batch(args, work_root, case_api, read_case_inputs, execute_case_work,
                   create_shared=SharedNativeProcess):
    """Keep per-case acceptance while retiring a failed session before continuing.

    Only scenario requests are ordered here. A new call reads current inputs and
    immutable acceptance; this object never restores a workflow progress log.
    """
    cases = session_case_order(args.cases) if args.cases else [args.case]
    work_root.mkdir(parents=True, exist_ok=True)
    shared = None
    failed = False
    try:
        for name in cases:
            current = copy.copy(args)
            current.case = name
            reusable = bool(args.cases and name in SHARED_PROJECT_CASES)
            if not reusable and shared is not None:
                shared.close()
                shared = None
            if reusable and shared is None:
                shared = create_shared(work_root, args.executable)
            case_root = work_root / ("_" + name)

            def inputs(current=current, reusable=reusable):
                result = read_case_inputs(current)
                if reusable:
                    result["execution_scope"]["project_isolation"] = "shared-empty-project.v1"
                return result

            def execute(run_id, identities, current=current, case_root=case_root, shared=shared):
                return execute_case_work(current, case_root, run_id, identities, shared)

            try:
                if name in ("project-compatibility", "track-groups", "video-clip-mute",
                            "collect-media", "video-fades", "video-embedding", "video-pictures"):
                    result = case_api.run_project_compatibility_checks(work_root, name, inputs,
                        execute, case_root, args.test_display, args.ffmpeg)
                else:
                    result = case_api.run_native_check(work_root, name, inputs, execute)
                print(json.dumps(result), flush=True)
            except Exception as error:
                failed = True
                print(json.dumps({"case": name, "result": "failed", "error": str(error)}), flush=True)
                if shared is not None:
                    shared.close(force=True)
                    shared = None
                if not args.cases:
                    raise
    finally:
        if shared is not None:
            shared.close(force=sys.exc_info()[0] is not None)
    return 1 if failed else 0


def run_requested_cases(args, root, case_api):
    """Resolve the private Windows boundary and apply the case's receipt owner."""
    if os.name != "nt":
        raise RuntimeError("The scripting integration cases require Windows.")
    contain_native_children()
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
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    if not mutex or ctypes.get_last_error() == 183:
        if mutex:
            kernel.CloseHandle(mutex)
        raise RuntimeError("Another AuTopsy behavior-test runner is active.")
    previous_display = os.environ.get("AUTOPSY_TEST_DISPLAY")
    os.environ["AUTOPSY_TEST_DISPLAY"] = display["device"]
    try:
        pipe = r"\\.\pipe\AuTopsyTestToSrvPipe"
        if kernel.WaitNamedPipeW(pipe, 1) or ctypes.get_last_error() not in (2,):
            raise RuntimeError("An AuTopsy scripting session is already active.")
        root.parent.mkdir(parents=True, exist_ok=True)
        args.executable = executable
        args.test_display = display

        def read_inputs(current):
            return native_test_inputs(current, executable, module, display, case_api)

        def execute(current, case_root, run_id, identities, shared):
            return execute_case(current, case_root, executable, module, display, kernel, pipe,
                                case_api, run_id, identities, shared)

        if args.reject_display or args.interrupt_cache:
            case_api.retain_behavior_result(root.parent)
            execute(args, root, uuid.uuid4().hex, read_inputs(args), None)
            if args.reject_display:
                args.reject_display = "transition"
                root = root.with_name(root.name + "_transition")
                execute(args, root, uuid.uuid4().hex, read_inputs(args), None)
            return 0
        return run_case_batch(args, root.parent, case_api, read_inputs, execute)
    finally:
        kernel.CloseHandle(mutex)
        if previous_display is None:
            os.environ.pop("AUTOPSY_TEST_DISPLAY", None)
        else:
            os.environ["AUTOPSY_TEST_DISPLAY"] = previous_display
