"""Shared process, display, media and GUI harness for AuTopsy behavior tests.

The caller owns its test process and marked work root. All visible actions stay
on the selected secondary display; absent display evidence blocks input.
"""

import ctypes
import json
import math
import os
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

from run_audacity_consumer import (
    run_process,
    startup_window_messages,
)


class NativeCrashDebugger:
    """Optional CDB attachment to the caller's private app PID, with one log.

    The caller must stop its app before close. A failed app keeps at most 256 KiB
    of this case's current trace; normal exit removes it. No system debugger or
    registry setting is changed, and no other process name is used for selection.
    """

    def __init__(self, app, root):
        self.app, self.process, self.stream = app, None, None
        self.path = root / "_native-debugger.log"
        selected = os.environ.get("AUTOPSY_TEST_NATIVE_DEBUGGER")
        if not selected:
            return
        executable = Path(selected).resolve(strict=True)
        if executable.name.casefold() != "cdb.exe" or app.poll() is not None:
            raise RuntimeError("Native crash tracing requires CDB and a live private test app.")
        action = ".ecxr; kv 40; .detach; q"
        commands = "kv 40; " + "; ".join(f'sxe -c "{action}" {kind}' for kind in ("av", "sov", "ii", "dz")) + '; sxe -c "kv 30; g" eh; g'
        command = [str(executable), "-p", str(app.pid), "-G", "-lines", "-c", commands]
        self.stream = self.path.open("wb")
        try:
            self.process = subprocess.Popen(command, stdout=self.stream, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        except BaseException as error:
            self.stream.close()
            self.stream = None
            self.path.unlink(missing_ok=True)
            raise RuntimeError(f"Starting native crash trace: {subprocess.list2cmdline(command)}\n"
                f"Exit: <not started>\nstdout: <absent>\nstderr: {error}") from error

    def close(self):
        if self.stream is None:
            return
        if self.app.poll() is None:
            raise RuntimeError("Stop the private app before closing its debugger.")
        assert self.process is not None
        try:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)
        finally:
            self.stream.close()
            self.stream = None
        if self.app.returncode == 0:
            self.path.unlink(missing_ok=True)
        elif self.path.stat().st_size > 262144:
            with self.path.open("rb") as source:
                source.seek(-262144, 2)
                tail = source.read()
            self.path.write_bytes(tail)


def write_neutral_preview_video(ffmpeg, output):
    """Create a 16-second synthetic picture/tone in a caller-owned test directory.

    The caller supplies a fresh marked work root or a private temporary directory.
    Only the completed fixture and one private pending sibling belong to this
    operation. Repeated calls keep the completed fixture; interrupted encoding
    leaves no published partial video and a fresh call discards abandoned staging.
    """
    output = Path(output)
    pending = output.with_name("_" + output.stem + ".pending.mp4")
    pending.unlink(missing_ok=True)
    if output.is_file():
        return
    try:
        run_process(
            [ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
             "testsrc2=s=1920x1080:r=30:d=16", "-f", "lavfi", "-i",
             "sine=frequency=440:sample_rate=48000:duration=16",
             "-c:v", "mpeg4", "-q:v", "5", "-c:a", "aac", "-shortest",
             "-metadata", "comment=AuTopsy synthetic preview fixture 1", str(pending)],
            timeout=60,
        )
        pending.replace(output)
    finally:
        pending.unlink(missing_ok=True)


def check_neutral_preview_fixture(work_root, ffmpeg, ffprobe):
    """Check encoded picture/audio and fresh calls around both publication effects."""
    import tempfile
    from unittest import mock

    work_root.mkdir(parents=True, exist_ok=True)
    real_run, real_replace = run_process, Path.replace
    for boundary in (None, "encode-before", "encode-after", "publish-before", "publish-after"):
        with tempfile.TemporaryDirectory(prefix="_neutral-video-", dir=work_root) as directory:
            output = Path(directory) / "source.mp4"

            def encode(*args, selected_boundary=boundary, **kwargs):
                if selected_boundary == "encode-before":
                    raise InterruptedError("before encoder")
                result = real_run(*args, **kwargs)
                if selected_boundary == "encode-after":
                    raise InterruptedError("after encoder")
                return result

            def publish(source, target, selected_boundary=boundary):
                if selected_boundary == "publish-before":
                    raise InterruptedError("before publication")
                result = real_replace(source, target)
                if selected_boundary == "publish-after":
                    raise InterruptedError("after publication")
                return result

            with mock.patch(__name__ + ".run_process", side_effect=encode), \
                    mock.patch.object(Path, "replace", publish):
                try:
                    write_neutral_preview_video(ffmpeg, output)
                except InterruptedError:
                    assert boundary is not None
                else:
                    assert boundary is None
            # Represent a hard stop that bypassed cleanup, then use the same
            # public operation without carrying an execution checkpoint.
            pending = output.with_name("_source.pending.mp4")
            pending.write_bytes(b"interrupted encoding")
            write_neutral_preview_video(ffmpeg, output)
            timestamp = output.stat().st_mtime_ns
            write_neutral_preview_video(ffmpeg, output)
            assert output.stat().st_mtime_ns == timestamp and not pending.exists()
            media = json.loads(real_run(
                [ffprobe, "-v", "error", "-count_frames", "-show_streams",
                 "-show_format", "-of", "json", str(output)], text=True, timeout=60))
            video = next(row for row in media["streams"] if row["codec_type"] == "video")
            audio = next(row for row in media["streams"] if row["codec_type"] == "audio")
            assert (video["width"], video["height"], video["nb_read_frames"]) == (1920, 1080, "480"), video
            assert audio["codec_name"] == "aac" and abs(float(audio["duration"]) - 16) < 0.1, audio
            assert media["format"]["tags"]["comment"] == "AuTopsy synthetic preview fixture 1"
    print("fixtures: PASS (synthetic picture/audio, repeated calls and interrupted publication)")


def run_behavior_worker(command, environment, app, root, *, timeout_seconds=240, expected_app_stop=None):
    """Collect worker streams with a finite budget; stop on owned-app failure.

    The outer case owner closes the app. This function owns only its worker,
    including termination and both output streams on interruption or expiry.
    Long soak callers reserve extra setup/cleanup time in their outer deadline.
    A fault-test owner may set expected_app_stop only after observing its exact
    native boundary, immediately before stopping this app. That acknowledgement
    changes shutdown handling, never the worker's exit code or acceptance.
    """
    if not math.isfinite(timeout_seconds) or not 0 < timeout_seconds <= 285:
        raise ValueError("Choose a finite positive worker deadline of at most 285 seconds.")
    if app.poll() is not None:
        raise RuntimeError(
            f"Command: {subprocess.list2cmdline(command)}\n"
            f"Owned app exited with status {app.returncode} before worker launch.\n"
            "Worker exit: <not started>\nstdout: <absent>\nstderr: <absent>"
        )
    operation = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8", errors="backslashreplace",
        env={**(os.environ if environment is None else environment), "PYTHONIOENCODING": "utf-8"},
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    deadline, stopped_at = time.monotonic() + timeout_seconds, None
    cause, output, errors = None, "", ""
    try:
        while True:
            try:
                output, errors = operation.communicate(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                status = app.poll()
                if status is not None:
                    if expected_app_stop is not None and expected_app_stop.is_set():
                        break
                    if stopped_at is None:
                        stopped_at = time.monotonic()
                    if status or time.monotonic() - stopped_at > 2:
                        cause = f"Owned app exited with status {status} while its worker was active."
                        break
                if time.monotonic() >= deadline:
                    cause = f"Behavior worker exceeded its {timeout_seconds:g}-second deadline."
                    break
    finally:
        if operation.poll() is None:
            operation.terminate()
            output, errors = operation.communicate(timeout=10)
        (root / "_commands.log").write_text(output, encoding="utf-8")
        (root / "_errors.log").write_text(errors, encoding="utf-8")
    if cause:
        app_output = (root / "_app.log").read_text(encoding="utf-8", errors="replace")[
            -5000:
        ]
        raise RuntimeError(
            f"Command: {subprocess.list2cmdline(command)}\n{cause}\n"
            f"Worker exit: {operation.returncode}\nstdout:\n{output[-14000:] or '<absent>'}\n"
            f"stderr:\n{errors or '<absent>'}\nApp output:\n{app_output or '<absent>'}"
        )
    return subprocess.CompletedProcess(command, operation.returncode, output, errors)


def select_test_display(displays):
    """Prefer screen 2; renumbered GDI names must never fall back to the primary."""
    if not displays or len({row["device"] for row in displays}) != len(displays):
        raise RuntimeError("Connected test displays are missing or ambiguous.")
    candidates = (
        displays
        if len(displays) == 1
        else [row for row in displays if row["device"] != r"\\.\DISPLAY1" and (row["device"] == r"\\.\DISPLAY2" or not row["primary"])]
    )
    if not candidates:
        raise RuntimeError("No test display other than screen 1 is available.")
    target = min(
        candidates, key=lambda row: (row["device"] != r"\\.\DISPLAY2", row["device"])
    )
    left, top, right, bottom = target["work"]
    if right - left < 640 or bottom - top < 480:
        raise RuntimeError(
            "The selected test display needs a work area of at least 640 by 480."
        )
    return target


def connected_test_displays():
    """Read work areas in this thread's DPI coordinate space; change no display settings."""
    from ctypes import wintypes as w

    class MonitorInfo(ctypes.Structure):
        _fields_ = [
            ("size", w.DWORD),
            ("monitor", w.RECT),
            ("work", w.RECT),
            ("flags", w.DWORD),
            ("device", w.WCHAR * 32),
        ]

    callback_type = ctypes.WINFUNCTYPE(
        w.BOOL, w.HMONITOR, w.HDC, ctypes.POINTER(w.RECT), w.LPARAM
    )
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetMonitorInfoW.argtypes = [w.HMONITOR, ctypes.POINTER(MonitorInfo)]
    user.EnumDisplayMonitors.argtypes = [
        w.HDC,
        ctypes.c_void_p,
        callback_type,
        w.LPARAM,
    ]
    displays, errors = [], []

    @callback_type
    def visit(monitor, dc, rectangle, data):
        info = MonitorInfo()
        info.size = ctypes.sizeof(info)
        if not user.GetMonitorInfoW(monitor, ctypes.byref(info)):
            errors.append(ctypes.get_last_error())
            return False
        displays.append(
            {
                "device": info.device,
                "primary": bool(info.flags & 1),
                "work": [
                    info.work.left,
                    info.work.top,
                    info.work.right,
                    info.work.bottom,
                ],
            }
        )
        return True

    if not user.EnumDisplayMonitors(None, None, visit, 0) or errors:
        raise RuntimeError(
            f"Read connected displays: Windows errors {errors or [ctypes.get_last_error()]}"
        )
    return displays


def require_test_display_guard(module):
    """Read the module's exported contract data without running it or its dependencies."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
    ]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel.GetProcAddress.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    image = kernel.LoadLibraryExW(str(module), None, 1)  # DONT_RESOLVE_DLL_REFERENCES
    if not image:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        address = kernel.GetProcAddress(image, b"AuTopsyTestDisplayGuardVersion")
        if not address or ctypes.c_uint.from_address(address).value != 1:
            raise RuntimeError(
                "Rebuild mod-script-pipe before GUI tests: its pre-show display guard is missing."
            )
    finally:
        kernel.FreeLibrary(image)


def assert_owned_test_display(pid, include_window=None):
    """Check the child's windows before input/capture; never move another process."""
    from ctypes import wintypes as w

    deadline = time.monotonic() + 2
    while True:
        target = select_test_display(connected_test_displays())
        if target["device"] == os.environ["AUTOPSY_TEST_DISPLAY"]:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("The selected test display disconnected or its identity changed.")
        time.sleep(0.02)  # No input/capture occurs until the same device returns.
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    rows, errors = [], []

    @callback_type
    def visit(window, data):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and (
            user.IsWindowVisible(window) or window == include_window
        ):
            rectangle = w.RECT()
            text = ctypes.create_unicode_buffer(256)
            if not user.GetWindowRect(window, ctypes.byref(rectangle)):
                errors.append(
                    f"Read window {window}: Windows error {ctypes.get_last_error()}"
                )
                return False
            user.GetWindowTextW(window, text, len(text))
            rect = [rectangle.left, rectangle.top, rectangle.right, rectangle.bottom]
            left, top, right, bottom = target["work"]
            if not (
                left <= rect[0] < rect[2] <= right
                and top <= rect[1] < rect[3] <= bottom
            ):
                errors.append(f"Test window {text.value!r} at {rect} escaped {target}")
            rows.append({"handle": window, "title": text.value, "rectangle": rect})
        return True

    if not user.EnumWindows(visit, 0) or errors:
        raise RuntimeError("; ".join(errors) or "Read owned test windows failed.")
    if include_window and not any(row["handle"] == include_window for row in rows):
        raise RuntimeError(
            "The requested placement-test window belongs to another process or disappeared."
        )
    return rows


def check_test_display_selection():
    """No GUI: cover reordered, negative-origin, mixed-size, and missing display layouts."""

    def display(number, work, primary=False):
        return {"device": rf"\\.\DISPLAY{number}", "work": work, "primary": primary}

    first = display(1, [0, 0, 1920, 1040], True)
    second = display(2, [-1280, 113, 0, 785])
    third = display(3, [1920, -1200, 3000, 720])
    for _ in range(3):
        for layout, expected in [
            ([first], first),
            ([second], second),
            ([first, second], second),
            ([second, first], second),
            ([third, first, second], second),
            ([first, third], third),
            (
                [dict(second, primary=True), dict(first, primary=False)],
                dict(second, primary=True),
            ),
        ]:
            assert select_test_display(layout) == expected, layout
        for layout in ([], [first, first], [first, display(2, [0, 0, 639, 479])]):
            try:
                select_test_display(layout)
            except RuntimeError:
                pass
            else:
                raise AssertionError(layout)
    print(
        "display-policy: PASS (identity selection, screen-1 exclusion, failure boundaries)"
    )


def check_owned_display_guard(client):
    """Exercise the real module's placement hooks without pointer/key injection.

    Each operation checks the current HWND's process, including after failures;
    cleanup never touches a recycled handle. Caller DPI changes, repeated native
    moves, visibility changes and topology messages all travel through Windows.
    No actual display configuration is changed. The caller's DPI context and
    the owned project's original placement are restored on every exit path.
    """
    from ctypes import wintypes as w

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetAncestor.argtypes = [w.HWND, w.UINT]
    user.GetAncestor.restype = w.HWND
    user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.IsWindow.argtypes = [w.HWND]
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, w.UINT]
    user.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    user.SetThreadDpiAwarenessContext.argtypes = [w.HANDLE]
    user.SetThreadDpiAwarenessContext.restype = w.HANDLE
    user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                        w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
    pane = client.data("GetVideoPreviewInfo")["videoTrackPanelHandle"]
    window = user.GetAncestor(pane, 2)
    owner = w.DWORD()
    user.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if not window or not owner.value:
        raise RuntimeError("The display probe has no owned project window.")
    original = w.RECT()
    if not user.GetWindowRect(window, ctypes.byref(original)):
        raise ctypes.WinError(ctypes.get_last_error())

    def is_owned():
        current = w.DWORD()
        return (user.IsWindow(window)
                and user.GetWindowThreadProcessId(window, ctypes.byref(current))
                and current.value == owner.value)

    def check_window():
        if not is_owned():
            raise RuntimeError("The private display-probe window exited or changed owner.")
        assert_owned_test_display(owner.value, include_window=window)

    completed = 0
    try:
        for _ in range(2):
            for context in (-1, -2, -3, -4, -5):
                previous = user.SetThreadDpiAwarenessContext(w.HANDLE(context))
                if not previous:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    target = select_test_display(connected_test_displays())
                    left, top, right, bottom = target["work"]
                    for width, height in ((700, 500), (right-left-32, bottom-top-32)):
                        check_window()
                        if not user.SetWindowPos(window, None, left+16, top+16,
                                                 width, height, 0x14):
                            raise ctypes.WinError(ctypes.get_last_error())
                        check_window()
                        completed += 1
                    check_window()
                    reply = ctypes.c_size_t()
                    if not user.SendMessageTimeoutW(window, 0x7E, 32,
                            ((bottom-top) << 16) | (right-left), 2, 2000, ctypes.byref(reply)):
                        raise ctypes.WinError(ctypes.get_last_error())
                    check_window()
                    user.ShowWindow(window, 0)
                    check_window()
                    user.ShowWindow(window, 4)
                    check_window()
                    completed += 3
                finally:
                    user.SetThreadDpiAwarenessContext(previous)
    finally:
        if is_owned():
            user.ShowWindow(window, 4)
            check_window()
            if not user.SetWindowPos(window, None, original.left, original.top,
                    original.right-original.left, original.bottom-original.top, 0x14):
                raise ctypes.WinError(ctypes.get_last_error())
            check_window()
    print(json.dumps({"display_guard": "passed", "operations": completed,
                      "caller_dpi_contexts": 5, "repetitions": 2}))


def check_behavior_worker_lifecycle(work_root):
    """No GUI: repeat normal exit, bounded expiry and both app-failure boundaries."""
    import tempfile

    work_root.mkdir(parents=True, exist_ok=True)
    for boundary in ("normal", "before", "after", "deadline"):
        for _ in range(2):
            with tempfile.TemporaryDirectory(
                prefix="_worker-lifecycle-", dir=work_root
            ) as directory:
                root = Path(directory)
                (root / "_app.log").write_text(
                    "owned app failure evidence", encoding="utf-8"
                )
                # Observe the worker's effect instead of guessing its startup
                # speed: loaded CI hosts must reach the same interruption point.
                ready = root / "_worker_ready"
                app_code = (
                    (
                        "import sys,time;from pathlib import Path\n"
                        "while not Path(sys.argv[1]).exists(): time.sleep(.01)\n"
                        "sys.exit(197)"
                    )
                    if boundary == "after"
                    else (
                        "import time,sys;time.sleep("
                        + ("10" if boundary in ("normal", "deadline") else "0")
                        + ");sys.exit(197)"
                    )
                )
                app = subprocess.Popen(
                    [sys.executable, "-c", app_code, str(ready)],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                )
                if boundary == "before":
                    assert app.wait(timeout=5) == 197
                command = [
                    sys.executable,
                    "-c",
                    "import sys,time;print('worker output \\u05e9\\u05dc\\u05d5\\u05dd',flush=True);"
                    "print('worker errors \\u05e9\\u05dc\\u05d5\\u05dd',file=sys.stderr,flush=True);"
                    "from pathlib import Path;Path(sys.argv[1]).write_text('ready');time.sleep("
                    + ("0" if boundary == "normal" else "10")
                    + ")",
                    str(ready),
                ]
                started = time.monotonic()
                try:
                    try:
                        result = run_behavior_worker(
                            command, os.environ.copy(), app, root,
                            timeout_seconds=0.25 if boundary == "deadline" else 240,
                        )
                    except RuntimeError as error:
                        assert boundary != "normal", str(error)
                        if boundary == "deadline":
                            assert "0.25-second deadline" in str(error), str(error)
                            assert "Worker exit:" in str(error) and "stdout:" in str(error) and "stderr:" in str(error)
                            assert app.poll() is None, "The worker owner terminated an app it does not own."
                        elif boundary == "before":
                            assert "status 197" in str(error), str(error)
                            assert "<not started>" in str(error)
                            assert not (root / "_commands.log").exists()
                        else:
                            assert "status 197" in str(error), str(error)
                            assert (
                                "worker output \u05e9\u05dc\u05d5\u05dd" in (root / "_commands.log").read_text(encoding="utf-8")
                            )
                            assert "worker errors \u05e9\u05dc\u05d5\u05dd" in (root / "_errors.log").read_text(encoding="utf-8")
                            assert "owned app failure evidence" in str(error)
                    else:
                        assert boundary == "normal" and result.returncode == 0
                        assert (
                            "worker output \u05e9\u05dc\u05d5\u05dd" in result.stdout
                            and "worker errors \u05e9\u05dc\u05d5\u05dd" in result.stderr
                        )
                    assert time.monotonic() - started < 5, (
                        "Child failure did not stop the worker promptly."
                    )
                finally:
                    if app.poll() is None:
                        app.terminate()
                    app.wait(timeout=5)
    print(
        "worker-lifecycle: PASS (repeated exit, interruption, streams, and child cleanup)"
    )


def write_tone(path, channels=1):
    with wave.open(str(path), "wb") as output:
        output.setparams((channels, 2, 48000, 0, "NONE", "not compressed"))
        block = b"".join(
            struct.pack("<h", int(10000 * math.sin(2 * math.pi * 440 * i / 48000)))
            * channels
            for i in range(240000)
        )
        output.writeframes(block)


class ScriptClient:
    def __init__(self):
        import pipeclient

        self.client = pipeclient.PipeClient(enc="utf-8")

    def send(self, command, success=True):
        assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]))
        self.client.write(command)
        if not self.client.reply_ready.wait(240):
            raise RuntimeError("Scripting timeout: " + command)
        reply = self.client.read()
        print(command + "\n" + reply, flush=True)
        assert ("BatchCommand finished: OK" in reply) == success, reply
        assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]))
        return reply

    def data(self, command):
        return json.loads(self.send(command).split("BatchCommand", 1)[0].strip())

    def file(self, operation, path, extra="", success=True):
        return self.send(f'{operation}: Filename="{path.as_posix()}" {extra}', success)


def save_reopen(client, project, save_existing=False):
    # SaveProject2 is Save As and compares path strings. A reopened Windows
    # path may use different separators; use native Save for its current file.
    if save_existing:
        client.send("Save")
    else:
        client.file("SaveProject2", project)
    client.send("Close")
    client.file("OpenProject2", project)


def painted_video_tracks(client):
    """Wait for the native TrackPanel's asynchronous paint, not a proxy window."""
    deadline = time.monotonic() + 5
    while True:
        info = client.data("GetVideoPreviewInfo")
        if info["videoTrackThumbnails"] and all(
            row["dataWidth"] > 0 for row in info["videoTrackRows"]
        ):
            return info
        if time.monotonic() >= deadline:
            try:
                capture_owned_project(
                    int(info["videoTrackPanelHandle"]),
                    Path(os.environ["AUTOPSY_BEHAVIOR_ROOT"]) / "_unpainted-video.png",
                )
            except Exception as error:
                # Optional image capture must not replace the observed pane
                # failure with a missing-package or screenshot-process error.
                raise AssertionError({**info, "screenshot_error": f"{type(error).__name__}: {error}"}) from error
            raise AssertionError(info)
        time.sleep(0.1)


def ready_video_preview(client):
    """File loading and project-open callbacks complete asynchronously."""
    deadline = time.monotonic() + 15
    while True:
        info = client.data("GetVideoPreviewInfo")
        if info["ready"]:
            return info
        if time.monotonic() >= deadline:
            raise AssertionError(info)
        time.sleep(0.1)


def scale_pane_pointer_position(native_point, physical_size, reported_size):
    """Map the app's pane units to physical client pixels exactly once.

    Captured drags may extend beyond a pane for scrolling or track migration;
    only unusable geometry is rejected here. The press owner checks the actual
    screen target before sending a button transition.
    """
    if any(size <= 0 for size in (*physical_size, *reported_size)):
        raise ValueError("The owned pane has no usable physical/reported dimensions.")
    return tuple(round(position * physical / reported)
                 for position, physical, reported in zip(native_point, physical_size, reported_size, strict=True))


def operate_owned_video_track(
    client, start_time, end_time=None, track=None, to_track=None, cancel=False,
    gesture="drag", area="header", menu_path=(), key=None, painted=None, control=False,
    prior_click_time=None, alt=False
):
    """Exercise native clip/visibility gestures and menus, targeting only the owned PID.

    Coordinates come from the painted native row, including its actual data
    origin. Menu choices use caption-resolved native rectangles in the owned window;
    cancellation always releases held input before restoring the previous focus.
    Additive clicks own their Ctrl transition and release it even on failure.
    An optional preceding click and Escape stay in the same pane, exercising
    native hit-target continuity before the requested gesture.
    """
    if end_time is None:
        end_time = start_time
    if gesture not in ("drag", "click", "double-click", "context", "key"):
        raise ValueError("Unknown native video gesture: " + gesture)
    if control and gesture not in ("click", "drag", "key"):
        raise ValueError("Ctrl is supported for clip clicks, time-locked drags and pane keys.")
    if gesture == "key" and key is None:
        raise ValueError("A pane-key gesture requires its virtual key.")
    if alt and gesture != "drag":
        raise ValueError("Alt is supported only for unsnapped clip drags.")
    user = ctypes.WinDLL("user32", use_last_error=True)
    # Wait before reading window/paint facts: project-open callbacks may replace
    # those controls while the native double-click interval expires.
    time.sleep((user.GetDoubleClickTime() + 20) / 1000)
    info = painted if painted is not None else painted_video_tracks(client)
    if not all(
        info["viewStart"] <= time <= info["viewEnd"] for time in (start_time, end_time)
    ):
        client.send("FitInWindow")
        info = painted_video_tracks(client)
    panel = int(info["videoTrackPanelHandle"])
    rows = info["videoTrackRows"]
    source = next((row for row in rows if track is None or row["index"] == track), None)
    target = (
        source
        if to_track is None
        else next((row for row in rows if row["index"] == to_track), None)
    )
    if source is None or target is None:
        raise RuntimeError("The requested native video row is missing.")
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    owner = ctypes.c_uint32()
    user.GetWindowThreadProcessId(panel, ctypes.byref(owner))
    if owner.value != int(os.environ["AUTOPSY_OWNED_APP_PID"]):
        raise RuntimeError("The native TrackPanel belongs to another process.")

    def position(time, row):
        if area in ("mute", "solo", "toggle"):
            button = row[area]
            if button["width"] <= 0 or button["height"] <= 0:
                raise RuntimeError("The requested native video button is not painted.")
            return button["x"] + button["width"] // 2, button["y"] + button["height"] // 2
        x = row["dataLeft"] + round(
            (time - info["viewStart"])
            / (info["viewEnd"] - info["viewStart"])
            * row["dataWidth"]
        )
        if area == "controls":
            x = row["x"] + (row["dataLeft"] - row["x"]) // 2
        else:
            # At time zero, physical-pixel rounding can put an exact border
            # point in the ruler cell. Stay inside the painted data pane; edge
            # hit tolerance and source-frame snapping retain the intended time.
            x = max(row["dataLeft"] + 2, min(row["dataLeft"] + row["dataWidth"] - 2, x))
        return x, row["dataTop"] + (50 if area == "picture" else 12)

    x0, y0 = position(start_time, source)
    x1, y1 = position(end_time, target)

    # The scripting progress wrapper can still be disabling the pane when its
    # reply arrives. Inspect that precondition before input. A real held button
    # also prevents native mouse-leave handling from cancelling synthetic motion.
    class Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    class Mouse(ctypes.Structure):
        _fields_ = [
            ("x", ctypes.c_long),
            ("y", ctypes.c_long),
            ("data", ctypes.c_uint32),
            ("flags", ctypes.c_uint32),
            ("time", ctypes.c_uint32),
            ("extra", ctypes.c_size_t),
        ]

    class Key(ctypes.Structure):
        _fields_ = [("code", ctypes.c_uint16), ("scan", ctypes.c_uint16),
                    ("flags", ctypes.c_uint32), ("time", ctypes.c_uint32),
                    ("extra", ctypes.c_size_t)]

    class Union(ctypes.Union):
        _fields_ = [("mouse", Mouse), ("keyboard", Key)]

    class Input(ctypes.Structure):
        _fields_ = [("kind", ctypes.c_uint32), ("value", Union)]

    class Gui(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32),
            ("flags", ctypes.c_uint32),
            *[
                (name, ctypes.c_void_p)
                for name in ("active", "focus", "capture", "menu", "move", "caret")
            ],
            ("rect", ctypes.c_long * 4),
        ]

    callback = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    user.GetAncestor.restype = ctypes.c_void_p
    user.GetForegroundWindow.restype = ctypes.c_void_p
    user.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user.GetParent.argtypes = [ctypes.c_void_p]
    user.GetParent.restype = ctypes.c_void_p
    user.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    user.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetWindowLongPtrW.restype = ctypes.c_ssize_t
    user.SetWindowPos.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    user.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_long * 4)]
    user.GetPhysicalCursorPos.argtypes = [ctypes.POINTER(Point)]
    user.GetPhysicalCursorPos.restype = ctypes.c_int
    user.SetPhysicalCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
    user.SetPhysicalCursorPos.restype = ctypes.c_int
    user.ClientToScreen.argtypes = [ctypes.c_void_p, ctypes.POINTER(Point)]
    user.ScreenToClient.argtypes = [ctypes.c_void_p, ctypes.POINTER(Point)]
    user.WindowFromPoint.argtypes = [Point]
    user.WindowFromPoint.restype = ctypes.c_void_p
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.EnumWindows.argtypes = [callback, ctypes.c_ssize_t]
    user.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetGUIThreadInfo.argtypes = [ctypes.c_uint32, ctypes.POINTER(Gui)]
    user.SendInput.argtypes = [ctypes.c_uint, ctypes.POINTER(Input), ctypes.c_int]
    user.SendMessageTimeoutW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    user.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    user.GetWindowDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
    previous_dpi = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    assert_owned_test_display(owner.value)
    root = user.GetAncestor(panel, 2)
    foreground = user.GetForegroundWindow()
    pinned = bool(user.GetWindowLongPtrW(root, -20) & 8)
    original = Point()
    if not user.GetPhysicalCursorPos(ctypes.byref(original)):
        raise RuntimeError("The owned input worker cannot read the physical cursor.")
    rectangle = (ctypes.c_long * 4)()
    user.GetClientRect(panel, ctypes.byref(rectangle))
    point = Point(*scale_pane_pointer_position((x0, y0), (rectangle[2], rectangle[3]),
        (info["videoTrackPanelWidth"], info["videoTrackPanelHeight"])))
    user.ClientToScreen(panel, ctypes.byref(point))
    initial_screen_point = (point.x, point.y)
    hidden = []
    pressed = False
    control_pressed = False
    alt_pressed = False
    positioned = False

    @callback
    def hide_preview(window, _):
        pid = ctypes.c_uint32()
        user.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == owner.value and user.IsWindowVisible(window):
            title = ctypes.create_unicode_buffer(256)
            user.GetWindowTextW(window, title, len(title))
            if title.value.startswith("Video Preview"):
                hidden.append(window)
                user.ShowWindow(window, 0)
        return 1

    def position_pointer(x, y):
        """Use physical pixels throughout, without virtualizing cursor reads."""
        destination = Point(*scale_pane_pointer_position((x, y), (rectangle[2], rectangle[3]),
            (info["videoTrackPanelWidth"], info["videoTrackPanelHeight"])))
        if not user.ClientToScreen(panel, ctypes.byref(destination)) or not user.SetPhysicalCursorPos(destination.x, destination.y):
            raise RuntimeError(f"Cannot place the owned physical pointer: {ctypes.WinError(ctypes.get_last_error())}")
        current = Point()
        if not user.GetPhysicalCursorPos(ctypes.byref(current)) or abs(current.x - destination.x) > 1 or abs(current.y - destination.y) > 1:
            raise RuntimeError(f"The physical pointer did not reach the owned pane; expected={(destination.x,destination.y)}, actual={(current.x,current.y)}.")
        point.x, point.y = destination.x, destination.y

    def button(flags, expected_window=panel):
        # Position and button transition share one OS event, so another mouse
        # movement cannot turn a narrow edge press into a clip-header press.
        hit = user.WindowFromPoint(point)
        if flags & (2 | 8) and hit != expected_window:
            bounds = (ctypes.c_long * 4)()
            user.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_long * 4)]
            user.GetWindowRect(expected_window, ctypes.byref(bounds))
            hit_owner, hit_title = ctypes.c_uint32(), ctypes.create_unicode_buffer(256)
            user.GetWindowThreadProcessId(hit, ctypes.byref(hit_owner))
            user.GetWindowTextW(user.GetAncestor(hit, 2), hit_title, len(hit_title))
            raise RuntimeError(
                f"Owned mouse press target changed before input; expected_window={expected_window}; "
                f"hit={hit}; hit_owner={hit_owner.value}; hit_title={hit_title.value!r}; "
                f"foreground={user.GetForegroundWindow()}; initial_screen={initial_screen_point}; "
                f"screen={(point.x, point.y)}; bounds={list(bounds)}; native_point={(x0, y0)}; "
                f"physical_client={list(rectangle)}; reported_client="
                f"{(info['videoTrackPanelWidth'], info['videoTrackPanelHeight'])}."
            )
        left, top, width, height = (user.GetSystemMetrics(index) for index in (76, 77, 78, 79))
        x = round((point.x - left) * 65535 / max(1, width - 1))
        y = round((point.y - top) * 65535 / max(1, height - 1))
        packet = Input(0, Union(mouse=Mouse(x, y, 0, flags | 0xC001, 0, 0)))
        if user.SendInput(1, ctypes.byref(packet), ctypes.sizeof(packet)) != 1:
            raise RuntimeError("Windows rejected the owned test's mouse-button event.")

    def keyboard(code):
        if user.GetForegroundWindow() != root:
            raise RuntimeError("The owned window lost foreground before keyboard input.")
        # wx distinguishes dedicated navigation/Delete keys from their keypad
        # counterparts by the extended flag, just as a physical keyboard does.
        extended = 1 if 0x21 <= code <= 0x28 or code in (0x2D, 0x2E) else 0
        for flags in (extended, extended | 2):
            packet = Input(1, Union(keyboard=Key(code, user.MapVirtualKeyW(code, 0), flags, 0, 0)))
            if user.SendInput(1, ctypes.byref(packet), ctypes.sizeof(packet)) != 1:
                raise RuntimeError("Windows rejected the owned test's keyboard event.")
        time.sleep(0.08)

    def set_modifier(code, down):
        packet = Input(1, Union(keyboard=Key(code, user.MapVirtualKeyW(code, 0), 0 if down else 2, 0, 0)))
        if user.SendInput(1, ctypes.byref(packet), ctypes.sizeof(packet)) != 1:
            raise RuntimeError(f"Windows rejected the owned test's modifier transition: {code}.")
        time.sleep(0.03)

    def choose_popup_path():
        # OBJID_CLIENT on a native popup exposes its actual HMENU. Resolve
        # captions and item rectangles at the moment of input, never by order.
        class MenuBar(ctypes.Structure):
            _fields_ = [("size", ctypes.c_uint32), ("rect", ctypes.c_long * 4),
                        ("menu", ctypes.c_void_p), ("submenu", ctypes.c_void_p),
                        ("flags", ctypes.c_uint32)]
        user.GetMenuBarInfo.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_long, ctypes.POINTER(MenuBar)]
        user.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
        user.GetMenuItemCount.argtypes = [ctypes.c_void_p]
        user.GetMenuStringW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_uint]
        user.GetSubMenu.argtypes = [ctypes.c_void_p, ctypes.c_int]
        user.GetSubMenu.restype = ctypes.c_void_p
        user.GetMenuState.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        submenu = False
        for label in menu_path:
            deadline = time.monotonic() + 3
            matches: list[tuple[int, int, int]] = []
            observed: list[str] = []
            while True:
                matches.clear()
                observed.clear()

                @callback
                def inspect_popup(window, _, label=label, matches=matches, observed=observed):
                    pid = ctypes.c_uint32()
                    user.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    if pid.value != owner.value or not user.IsWindowVisible(window):
                        return 1
                    kind = ctypes.create_unicode_buffer(64)
                    user.GetClassNameW(window, kind, len(kind))
                    if kind.value != "#32768":
                        return 1
                    bar = MenuBar()
                    bar.size = ctypes.sizeof(bar)
                    if not user.GetMenuBarInfo(window, -4, 0, ctypes.byref(bar)):
                        return 1
                    for index in range(min(64, user.GetMenuItemCount(bar.menu))):
                        text = ctypes.create_unicode_buffer(256)
                        user.GetMenuStringW(bar.menu, index, text, len(text), 0x400)
                        caption = text.value.replace("&", "").split("\t", 1)[0]
                        observed.append(caption)
                        if caption == label:
                            matches.append((bar.menu, index, window))
                    return 1

                user.EnumWindows(inspect_popup, 0)
                if len(matches) == 1:
                    break
                if len(matches) > 1 or time.monotonic() >= deadline:
                    raise RuntimeError(f"Expected one visible popup item {label!r}; matches={matches}; captions={observed!r}")
                time.sleep(0.01)
            menu, index, popup = matches[0]
            if user.GetMenuState(menu, index, 0x400) & 3:
                raise RuntimeError("The requested popup item is disabled: " + label)
            # Menu item rectangles retain their owner's logical scale. Map
            # within current logical/physical popup bounds, including its origin.
            user.GetMenuItemRect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_long * 4)]
            user.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_long * 4)]
            bounds, logical, physical = ((ctypes.c_long * 4)() for _ in range(3))
            prior = user.SetThreadDpiAwarenessContext(user.GetWindowDpiAwarenessContext(root))
            try:
                readable = user.GetMenuItemRect(None, menu, index, ctypes.byref(bounds)) and user.GetWindowRect(popup, ctypes.byref(logical))
            finally:
                user.SetThreadDpiAwarenessContext(prior)
            if not readable or not user.GetWindowRect(popup, ctypes.byref(physical)) or logical[2] <= logical[0] or logical[3] <= logical[1] or bounds[2] <= bounds[0] or bounds[3] <= bounds[1]:
                raise RuntimeError("The owned popup item has no current screen rectangle: " + label)
            point.x = round(physical[0] + ((bounds[0] + bounds[2]) / 2 - logical[0]) * (physical[2] - physical[0]) / (logical[2] - logical[0]))
            point.y = round(physical[1] + ((bounds[1] + bounds[3]) / 2 - logical[1]) * (physical[3] - physical[1]) / (logical[3] - logical[1]))
            if user.WindowFromPoint(point) != popup or user.GetForegroundWindow() != root:
                raise RuntimeError("The owned popup item is obstructed or lost foreground: " + label)
            user.SetPhysicalCursorPos(point.x, point.y)
            try:
                button(2, popup)
            finally:
                button(4)
            submenu = bool(user.GetSubMenu(menu, index))
            deadline = time.monotonic() + 3
            while not submenu and user.IsWindowVisible(popup):
                if time.monotonic() >= deadline:
                    raise RuntimeError("The owned popup did not acknowledge its item click: " + label)
                time.sleep(0.01)
        if not menu_path or submenu:
            for _ in range(len(menu_path) + 1):
                keyboard(0x1B)

    def send(kind, flags, x, y):
        reply = ctypes.c_size_t()
        if not user.SendMessageTimeoutW(
            panel,
            kind,
            flags,
            ((y & 65535) << 16) | (x & 65535),
            2,
            2000,
            ctypes.byref(reply),
        ):
            raise RuntimeError(
                f"SendMessageTimeoutW(HWND={panel}, message={kind:#x}, flags={flags}, point={(x, y)}) "
                f"failed with Windows error {ctypes.get_last_error()}; "
                + startup_window_messages(owner.value)
            )

    try:
        if any(user.GetAsyncKeyState(key) & 0x8000 for key in (1, 16, 17, 18, 91, 92)):
            raise RuntimeError(
                "Human input is held; the test will not take over its drag meaning."
            )
        user.EnumWindows(hide_preview, 0)
        if not user.SetWindowPos(root, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x53):
            raise RuntimeError(f"Cannot expose owned project {root}: Windows error {ctypes.get_last_error()}.")
        activated = user.SetForegroundWindow(root)
        thread = user.GetWindowThreadProcessId(panel, None)
        deadline = time.monotonic() + 3
        while True:
            enabled = True
            ancestor = panel
            while ancestor:
                enabled = enabled and bool(user.IsWindowEnabled(ancestor))
                if ancestor == root:
                    break
                ancestor = user.GetParent(ancestor)
            hit = user.WindowFromPoint(point)
            state = Gui()
            state.size = ctypes.sizeof(state)
            active = user.GetGUIThreadInfo(thread, ctypes.byref(state)) and state.active == root
            # Script replies can precede their progress window's destruction.
            # Exposed pixels alone do not make the pane an active input target;
            # wait for that owned command window to return activation first.
            if enabled and hit == panel and active:
                break
            obscurer = user.GetAncestor(hit, 2) if hit else None
            title = ctypes.create_unicode_buffer(256)
            obscurer_pid = ctypes.c_uint32()
            if obscurer:
                user.GetWindowTextW(obscurer, title, len(title))
                user.GetWindowThreadProcessId(obscurer, ctypes.byref(obscurer_pid))
            # Activating a wx parent can restore an owned floating preview.
            # Enforce exposure after activation, using current PID and title;
            # never hide another application or send input through an overlay.
            if obscurer_pid.value == owner.value and title.value.startswith("Video Preview"):
                hide_preview(obscurer, 0)
            if time.monotonic() >= deadline:
                root_rect = (ctypes.c_long * 4)()
                user.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_long * 4)]
                user.GetWindowRect(root, ctypes.byref(root_rect))
                pane_origin = Point()
                user.ClientToScreen(panel, ctypes.byref(pane_origin))
                raise RuntimeError(
                    f"The owned native pane did not become enabled and exposed: enabled={enabled}; "
                    f"root={root}; active={state.active}; visible={user.IsWindowVisible(root)}; rect={list(root_rect)}; "
                    f"activated={activated}; topmost={bool(user.GetWindowLongPtrW(root, -20) & 8)}; "
                    f"pane_origin={(pane_origin.x,pane_origin.y)}; client_size={list(rectangle)}; "
                    f"panel={panel}; hit={hit}; obscurer_pid={obscurer_pid.value}; title={title.value!r}; "
                    f"point={(point.x, point.y)}; view={(info['viewStart'], info['viewEnd'])}; row={source}."
                )
            time.sleep(0.01)
        user.SetPhysicalCursorPos(point.x, point.y)
        positioned = True
        position_pointer(x0, y0)
        if any(user.GetAsyncKeyState(key) & 0x8000 for key in (1, 16, 17, 18, 91, 92)):
            raise RuntimeError("Human input changed before the owned drag began.")
        if prior_click_time is not None:
            position_pointer(*position(prior_click_time, source))
            button(2)
            pressed = True
            button(4)
            pressed = False
            time.sleep(0.06)
            keyboard(0x1B)
            position_pointer(x0, y0)
        if control:
            set_modifier(0x11, True)
            control_pressed = True
        if alt:
            set_modifier(0x12, True)
            alt_pressed = True
        if gesture == "context":
            button(8)
            pressed = True
            time.sleep(0.08)
            button(16)
            pressed = False
            deadline = time.monotonic() + 3
            while True:
                state = Gui()
                state.size = ctypes.sizeof(state)
                if user.GetGUIThreadInfo(thread, ctypes.byref(state)) and state.flags & 4:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"Right-click did not open an owned native popup menu: flags={state.flags}; root={root}; foreground={user.GetForegroundWindow()}; point={(point.x, point.y)}; hit={user.WindowFromPoint(point)}; panel={panel}.")
                time.sleep(0.01)
            choose_popup_path()
        elif gesture != "key":
            for _ in range(2 if gesture == "double-click" else 1):
                button(2)
                pressed = True
                if gesture == "drag":
                    deadline = time.monotonic() + 1
                    while True:
                        state = Gui()
                        state.size = ctypes.sizeof(state)
                        if user.GetGUIThreadInfo(thread, ctypes.byref(state)) and state.capture == panel:
                            break
                        if time.monotonic() >= deadline:
                            current = Point()
                            user.GetPhysicalCursorPos(ctypes.byref(current))
                            if abs(current.x - point.x) > 2 or abs(current.y - point.y) > 2:
                                raise RuntimeError(
                                    f"Shared pointer input moved away from the owned test window; "
                                    f"expected={(point.x, point.y)}, actual={(current.x, current.y)}."
                                )
                            raise RuntimeError(
                                f"The owned pane did not capture held input; GUI={(state.flags, state.active, state.focus, state.capture, state.menu)}; "
                                f"button={user.GetAsyncKeyState(1) & 0x8000}; cursor={(current.x, current.y)}; "
                                f"native_point={(x0, y0)}; row={source}; "
                                + startup_window_messages(owner.value)
                            )
                        time.sleep(0.01)
                    # Native timers consult the real pointer, so move it as
                    # well as the button state. One OS input route avoids a
                    # synthetic motion disagreeing with the global cursor.
                    for step in range(1, 6):
                        position_pointer(round(x0 + (x1-x0)*step/5), round(y0 + (y1-y0)*step/5))
                        time.sleep(0.04)
                    if cancel:
                        # Ctrl/Alt+Escape belong to Windows. Releasing a modifier
                        # can also finish native time-shift gestures, so deliver
                        # Escape through the owned pane's normal key-message
                        # route while the drag is still captured. Other drags
                        # keep the physical keyboard route.
                        if control_pressed or alt_pressed:
                            # Queue a full key transition so wx's application
                            # filter sees Escape before the mouse release. A
                            # synchronous HWND send bypasses that queue and can
                            # leave a migration active; physical Ctrl+Esc opens Start.
                            for message, flags in ((0x100, 1), (0x101, 0xC0000001)):
                                if not user.PostMessageW(panel, message, 0x1B, flags):
                                    raise RuntimeError("Windows rejected the owned drag's queued Escape.")
                            deadline = time.monotonic() + 3
                            while True:
                                state = Gui()
                                state.size = ctypes.sizeof(state)
                                if user.GetGUIThreadInfo(thread, ctypes.byref(state)) and state.capture != panel:
                                    break
                                if time.monotonic() >= deadline:
                                    raise RuntimeError("The owned modified drag did not cancel before mouse release.")
                                time.sleep(0.01)
                        else:
                            keyboard(0x1B)
                button(4)
                pressed = False
                deadline = time.monotonic() + 3
                while gesture == "drag":
                    state = Gui()
                    state.size = ctypes.sizeof(state)
                    if user.GetGUIThreadInfo(thread, ctypes.byref(state)) and state.capture != panel:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("The owned pane did not release the completed drag.")
                    time.sleep(0.01)
                time.sleep(0.06)
        if key is not None:
            if gesture == "key":
                state = Gui()
                state.size = ctypes.sizeof(state)
                if not user.GetGUIThreadInfo(thread, ctypes.byref(state)) or state.focus != panel:
                    raise RuntimeError("The owned pane must have keyboard focus before sending a selection key.")
            keyboard(key)

    finally:
        control_release_error = None
        if alt_pressed:
            try:
                set_modifier(0x12, False)
            except RuntimeError as error:
                control_release_error = error
        if control_pressed:
            try:
                set_modifier(0x11, False)
            except RuntimeError as error:
                control_release_error = error
        if pressed:
            try:
                # A failed/closed owned app has no remaining capture to cancel.
                user.IsWindow.argtypes = [ctypes.c_void_p]
                if user.IsWindow(panel):
                    send(0x100, 0x1B, 0, 0)
            finally:
                # Input release must survive a missing HWND or failed Escape.
                button(16 if gesture == "context" else 4)
        user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        current = Point()
        user.GetPhysicalCursorPos(ctypes.byref(current))
        if positioned and (current.x, current.y) == (point.x, point.y):
            user.SetPhysicalCursorPos(original.x, original.y)
        if not pinned:
            user.SetWindowPos(root, ctypes.c_void_p(-2), 0, 0, 0, 0, 0x13)
        for window in hidden:
            user.ShowWindow(window, 4)
        # A selected menu command may be creating a modal window after its
        # popup disappears. Leave focus with the owned app for that handoff.
        if user.GetForegroundWindow() == root and foreground and not (gesture == "context" and menu_path):
            user.SetForegroundWindow(foreground)
        if previous_dpi:
            user.SetThreadDpiAwarenessContext(previous_dpi)
        if control_release_error:
            raise control_release_error

    return panel


def write_project_capture(ffmpeg, path, width, height, pixels):
    """Encode owned-window pixels with the test suite's existing media runtime.

    The caller's marked case root owns both the picture and its pending sibling.
    A failed encoder or interrupted write cannot replace the last complete PNG;
    a fresh call clears abandoned staging before publishing the current picture.
    """
    path = Path(path)
    if not (0 < width < 6000 and 0 < height < 4000) or len(pixels) != width * height * 4:
        raise ValueError("Unexpected project capture dimensions or pixel count.")
    pending = path.with_name("_" + path.name + ".pending")
    pending.unlink(missing_ok=True)
    encoded = run_process(
        [ffmpeg, "-v", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "bgr0",
         "-video_size", f"{width}x{height}", "-i", "pipe:0", "-frames:v", "1",
         "-c:v", "png", "-f", "image2pipe", "pipe:1"],
        input=bytes(pixels), timeout=30,
    )
    if not encoded.startswith(b"\x89PNG\r\n\x1a\n"):
        raise RuntimeError("The project capture encoder returned no PNG picture.")
    if path.is_file() and path.read_bytes() == encoded:
        return
    try:
        pending.write_bytes(encoded)
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def capture_owned_project(panel, path):
    """Capture this test's project window to an artifact, without reading other windows."""
    from ctypes import wintypes

    user, gdi = ctypes.WinDLL("user32"), ctypes.WinDLL("gdi32")
    user.GetAncestor.argtypes = [wintypes.HWND, ctypes.c_uint]
    user.GetAncestor.restype = wintypes.HWND
    window = user.GetAncestor(panel, 2)
    owner = wintypes.DWORD()
    user.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if owner.value != int(os.environ["AUTOPSY_OWNED_APP_PID"]):
        raise RuntimeError("Screenshot target has another owner.")
    assert_owned_test_display(owner.value)
    rectangle = wintypes.RECT()
    user.GetWindowRect(window, ctypes.byref(rectangle))
    width, height = rectangle.right - rectangle.left, rectangle.bottom - rectangle.top
    if not (0 < width < 6000 and 0 < height < 4000):
        raise RuntimeError("Unexpected project window size.")
    user.GetDC.argtypes = [wintypes.HWND]
    user.GetDC.restype = wintypes.HDC
    user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    gdi.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
    gdi.SelectObject.restype = wintypes.HANDLE
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]
    gdi.DeleteDC.argtypes = [wintypes.HDC]
    user.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, ctypes.c_uint]
    screen = user.GetDC(window)
    memory = gdi.CreateCompatibleDC(screen)
    bitmap = gdi.CreateCompatibleBitmap(screen, width, height)
    previous = gdi.SelectObject(memory, bitmap)
    try:
        if not user.PrintWindow(window, memory, 2):
            raise RuntimeError("The owned project could not be captured.")
        gdi.SelectObject(memory, previous)

        class Header(ctypes.Structure):
            _fields_ = [
                ("size", wintypes.DWORD),
                ("width", wintypes.LONG),
                ("height", wintypes.LONG),
                ("planes", wintypes.WORD),
                ("bits", wintypes.WORD),
                ("compression", wintypes.DWORD),
                ("image_size", wintypes.DWORD),
                ("x", wintypes.LONG),
                ("y", wintypes.LONG),
                ("used", wintypes.DWORD),
                ("important", wintypes.DWORD),
            ]

        header = Header(ctypes.sizeof(Header), width, -height, 1, 32)
        pixels = ctypes.create_string_buffer(width * height * 4)
        gdi.GetDIBits.argtypes = [
            wintypes.HDC,
            wintypes.HBITMAP,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        if (
            gdi.GetDIBits(screen, bitmap, 0, height, pixels, ctypes.byref(header), 0)
            != height
        ):
            raise RuntimeError("The owned project pixels could not be read.")
        write_project_capture(os.environ["AUTOPSY_TEST_FFMPEG"], path, width, height, pixels.raw)
    finally:
        gdi.SelectObject(memory, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(window, screen)


def check_video_interactions(client, root, case):
    """Mouse/keyboard regressions on synthetic clips, independent of export/playback."""
    client.file("ImportVideo", root / "source.mp4")
    if case != "video-linked-edges":
        client.send('UnlinkMediaClips: Clips="0:0"')
        client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
        client.send("SelectTime: Start=2 End=2")
        client.send("Split")
        client.send("MoveVideoClip: Track=0 Id=2 Start=3")
    client.send("FitInWindow")

    def clips():
        return client.data("GetVideoClips")["clips"]

    def bounds(expected):
        actual = [(round(row["start"], 2), round(row["end"], 2)) for row in clips()]
        assert actual == expected, actual

    original = clips()
    if case == "video-selection":
        for gesture in ("click", "double-click", "click"):
            operate_owned_video_track(client, 0.8, track=0, gesture=gesture, area="picture")
            selection = client.data("GetInfo: Type=Selection Format=JSON")
            if gesture == "double-click":
                assert selection["Start"] == 0 and selection["End"] == 2, selection
            else:
                assert abs(selection["Start"] - 0.8) < 0.02 and selection["End"] == selection["Start"], selection
            assert clips() == original
        operate_owned_video_track(client, 0.5, 1.5, track=0, area="picture")
        selection = client.data("GetInfo: Type=Selection Format=JSON")
        assert abs(selection["Start"] - 0.5) < 0.02 and abs(selection["End"] - 1.5) < 0.02, selection
        assert clips() == original
        # The same single click must make Delete work through the keyboard command,
        # not a script that bypasses its selection preconditions.
        operate_owned_video_track(client, 0.8, track=0, gesture="click", area="picture", key=0x2E)
        bounds([(3, 5)])
        client.send("Undo")
        assert clips() == original
        operate_owned_video_track(client, 3.8, track=0, gesture="double-click", area="picture", key=0x08)
        bounds([(0, 2)])
        client.send("Undo")
        assert clips() == original

    elif case == "video-edges":
        # Selecting one of two touching clips must not move its neighbor.
        client.send("MoveVideoClip: Track=0 Id=2 Start=2")
        operate_owned_video_track(client, 0.8, track=0, gesture="click")
        operate_owned_video_track(client, 0.8, 0.3, track=0, cancel=True)
        bounds([(0, 2), (2, 4)])
        operate_owned_video_track(client, 2.8, track=0, gesture="click")
        operate_owned_video_track(client, 2.8, 3.3, track=0)
        bounds([(0, 2), (2.5, 4.5)])
        client.send("Undo")
        bounds([(0, 2), (2, 4)])
        client.send("MoveVideoClip: Track=0 Id=2 Start=3")
        original = clips()
        for _ in range(2):
            operate_owned_video_track(client, 2, 1.5, track=0)
            bounds([(0, 1.5), (3, 5)])
            operate_owned_video_track(client, 2.99, 3.5, track=0)
            bounds([(0, 1.5), (3.5, 5)])
            operate_owned_video_track(client, 3.5, 3, track=0, cancel=True)
            bounds([(0, 1.5), (3.5, 5)])
            operate_owned_video_track(client, 3.5, 3, track=0)
            bounds([(0, 1.5), (3, 5)])
            operate_owned_video_track(client, 1.5, 2, track=0)
            assert clips() == original

    elif case == "video-linked-edges":
        def linked_bounds(start, end):
            rows = client.data("GetLinkedMediaClips")
            assert {row["kind"] for row in rows} == {"video", "audio"}, rows
            assert all(abs(row["start"]-start) < 0.04 and abs(row["end"]-end) < 0.04 for row in rows), rows
            return rows
        linked_bounds(0, 4)
        for _ in range(2):
            operate_owned_video_track(client, 4, 3.5, track=0)
            linked_bounds(0, 3.5)
            operate_owned_video_track(client, 0, 0.5, track=0)
            trimmed = linked_bounds(0.5, 3.5)
            operate_owned_video_track(client, 0.5, 0, track=0, cancel=True)
            assert client.data("GetLinkedMediaClips") == trimmed
            operate_owned_video_track(client, 0.5, 0, track=0)
            linked_bounds(0, 3.5)
            operate_owned_video_track(client, 3.5, 4, track=0)
            linked_bounds(0, 4)
    elif case == "video-clipboard":
        # Popup Copy on a clip, cancel, then Paste in a gap and on an empty row.
        operate_owned_video_track(client, 0.8, track=0, gesture="context", area="picture",
                                 menu_path=("Copy",))
        assert clips() == original
        operate_owned_video_track(client, 2.5, track=0, gesture="context", area="picture")
        assert clips() == original
        for _ in range(2):
            operate_owned_video_track(client, 2.5, track=0, gesture="context", area="picture",
                                     menu_path=("Paste",))
            inserted = clips()
            assert len(inserted) == 3 and abs(inserted[1]["start"] - 2.5) < 0.05, inserted
            client.send("Undo")
            assert clips() == original
        client.send("NewVideoTrack")
        for _ in range(2):
            operate_owned_video_track(client, 0.5, track=2, gesture="context", area="picture",
                                     menu_path=("Paste",))
            pasted = [clip for clip in clips() if clip["track"] == 2]
            assert len(pasted) == 1 and abs(pasted[0]["start"] - 0.5) < 0.05, pasted
            client.send("Undo")
            assert clips() == original
    elif case == "video-track-menu":
        client.send("NewVideoTrack")
        def track_content():
            return [{key: value for key, value in row.items() if key not in ("selected", "focused")}
                    for row in client.data("GetInfo: Type=Tracks Format=JSON")]
        before = track_content()
        # Open and cancel the submenu, then repeat each relative insertion.
        client.send("FirstTrack")
        operate_owned_video_track(client, 0.5, track=0, gesture="context", area="controls",
                                 menu_path=("Add track...",))
        assert track_content() == before
        for label, index, kind in (
            ("Add Audio Track Before This Track", 0, "wave"),
            ("Add Audio Track After This Track", 1, "wave"),
            ("Add Video Track Before This Track", 0, "video"),
            ("Add Video Track After This Track", 1, "video")
        ):
            for _ in range(2):
                client.send("FirstTrack")
                operate_owned_video_track(client, 0.5, track=0, gesture="context", area="controls",
                                         menu_path=("Add track...", label))
                after = track_content()
                assert len(after) == len(before) + 1 and after[index]["kind"] == kind, after
                client.send("Undo")
                assert track_content() == before
    else:
        raise ValueError("Unknown video interaction case: " + case)
    client.file("SaveProject2", root / "_video-interactions.aup3")
