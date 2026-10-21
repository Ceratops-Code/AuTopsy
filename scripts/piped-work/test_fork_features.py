"""Exercise AuTopsy through its public scripting and media interfaces.

CTest supplies an executable and a dedicated work root. Tests own only marked
temporary directories and their child process. The native Portable Settings
name is required by Audacity; a marker protects any unrelated existing profile.
No installed app, user project, source snapshot or fixed upstream layout is used.
Visible tests use DISPLAY2 when connected, otherwise another display except
DISPLAY1 if several are connected. The scripting module constrains windows
before showing them; the runner refuses modules without that contract.
"""

import argparse
import ctypes
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid
import wave
from datetime import datetime
from pathlib import Path


def run_process(arguments, **options):
    if os.name == "nt":
        options.setdefault("creationflags", subprocess.CREATE_NO_WINDOW)
    try:
        result = subprocess.run(arguments, capture_output=True, **options)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"Command timed out: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
            f"stdout: {error.stdout or '<absent>'!r}\nstderr: {error.stderr or '<absent>'!r}"
        ) from error
    if result.returncode:

        def display(value):
            if not value:
                return "<absent>"
            return value.decode(errors="replace") if isinstance(value, bytes) else value

        raise RuntimeError(
            f"Command: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
            f"Exit: {result.returncode}\nstdout:\n{display(result.stdout)}\n"
            f"stderr:\n{display(result.stderr)}"
        )
    return result.stdout


def run_behavior_worker(command, environment, app, root):
    """Collect both worker streams and stop promptly when the owned app fails."""
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
        env=environment,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    deadline, stopped_at = time.monotonic() + 240, None
    cause, output, errors = None, "", ""
    try:
        while True:
            try:
                output, errors = operation.communicate(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                status = app.poll()
                if status is not None:
                    if stopped_at is None:
                        stopped_at = time.monotonic()
                    if status or time.monotonic() - stopped_at > 2:
                        cause = f"Owned app exited with status {status} while its worker was active."
                        break
                if time.monotonic() >= deadline:
                    cause = "Behavior worker exceeded its 240-second deadline."
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


def executable_version(executable):
    """Read the PE resource before creating a first-run app profile."""
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
    version.GetFileVersionInfoW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    version.VerQueryValueW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    size = version.GetFileVersionInfoSizeW(str(executable), None)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(executable), 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    value = ctypes.c_void_p()
    length = ctypes.c_uint32()
    if not version.VerQueryValueW(
        buffer, "\\", ctypes.byref(value), ctypes.byref(length)
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    fields = ctypes.cast(value, ctypes.POINTER(ctypes.c_uint32))
    if length.value < 52 or fields[0] != 0xFEEF04BD:
        raise RuntimeError("The executable has no fixed Windows version resource.")
    return fields[2] >> 16, fields[2] & 0xFFFF, fields[3] >> 16


def select_test_display(displays):
    """Choose by Windows device identity, never enumeration order or coordinates."""
    if not displays or len({row["device"] for row in displays}) != len(displays):
        raise RuntimeError("Connected test displays are missing or ambiguous.")
    candidates = (
        displays
        if len(displays) == 1
        else [row for row in displays if row["device"] != r"\\.\DISPLAY1"]
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

    target = select_test_display(connected_test_displays())
    if target["device"] != os.environ["AUTOPSY_TEST_DISPLAY"]:
        raise RuntimeError(
            "The selected test display disconnected or its identity changed."
        )
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


def check_behavior_worker_lifecycle(work_root):
    """No GUI: repeat normal exit and app failure on both sides of worker launch."""
    import tempfile

    work_root.mkdir(parents=True, exist_ok=True)
    for boundary in ("normal", "before", "after"):
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
                        + ("10" if boundary == "normal" else "0")
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
                    "import sys,time;print('worker output',flush=True);"
                    "print('worker errors',file=sys.stderr,flush=True);"
                    "from pathlib import Path;Path(sys.argv[1]).write_text('ready');time.sleep("
                    + ("0" if boundary == "normal" else "10")
                    + ")",
                    str(ready),
                ]
                started = time.monotonic()
                try:
                    try:
                        result = run_behavior_worker(
                            command, os.environ.copy(), app, root
                        )
                    except RuntimeError as error:
                        assert boundary != "normal", str(error)
                        assert "status 197" in str(error), str(error)
                        if boundary == "before":
                            assert "<not started>" in str(error)
                            assert not (root / "_commands.log").exists()
                        else:
                            assert (
                                "worker output" in (root / "_commands.log").read_text()
                            )
                            assert "worker errors" in (root / "_errors.log").read_text()
                            assert "owned app failure evidence" in str(error)
                    else:
                        assert boundary == "normal" and result.returncode == 0
                        assert (
                            "worker output" in result.stdout
                            and "worker errors" in result.stderr
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


def startup_window_messages(pid):
    """Keep dialogs from the owned child in startup failure diagnostics."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
    messages = []

    @callback_type
    def child(window, _):
        text = ctypes.create_unicode_buffer(2048)
        user.GetWindowTextW(window, text, len(text))
        if text.value:
            messages.append(text.value)
        return 1

    @callback_type
    def parent(window, _):
        owner = ctypes.c_uint32()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            child(window, 0)
            user.EnumChildWindows(window, child, 0)
        return 1

    user.EnumWindows(parent, 0)
    return " | ".join(messages) or "<no window text>"


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

        self.client = pipeclient.PipeClient()

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
            capture_owned_project(
                int(info["videoTrackPanelHandle"]),
                Path(os.environ["AUTOPSY_BEHAVIOR_ROOT"]) / "_unpainted-video.png",
            )
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


def drag_owned_video_clip(
    client, start_time, end_time, track=None, to_track=None, cancel=False
):
    """Exercise the real TrackPanel's clip header; target only the owned PID."""
    info = painted_video_tracks(client)
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
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    owner = ctypes.c_uint32()
    user.GetWindowThreadProcessId(panel, ctypes.byref(owner))
    if owner.value != int(os.environ["AUTOPSY_OWNED_APP_PID"]):
        raise RuntimeError("The native TrackPanel belongs to another process.")

    def position(time, row):
        x = row["dataLeft"] + round(
            (time - info["viewStart"])
            / (info["viewEnd"] - info["viewStart"])
            * info["videoTrackDataWidth"]
        )
        return x, row.get("dataTop", row["y"]) + 12

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

    class Union(ctypes.Union):
        _fields_ = [("mouse", Mouse)]

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
    user.GetCursorPos.argtypes = [ctypes.POINTER(Point)]
    user.ClientToScreen.argtypes = [ctypes.c_void_p, ctypes.POINTER(Point)]
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
    user.GetCursorPos(ctypes.byref(original))
    rectangle = (ctypes.c_long * 4)()
    user.GetClientRect(panel, ctypes.byref(rectangle))
    point = Point(
        round(x0 * rectangle[2] / info["videoTrackPanelWidth"]),
        round(y0 * rectangle[3] / info["videoTrackPanelHeight"]),
    )
    user.ClientToScreen(panel, ctypes.byref(point))
    hidden = []
    pressed = False
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

    def button(flags):
        packet = Input(0, Union(mouse=Mouse(0, 0, 0, flags, 0, 0)))
        if user.SendInput(1, ctypes.byref(packet), ctypes.sizeof(packet)) != 1:
            raise RuntimeError("Windows rejected the owned test's mouse-button event.")

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
        # Consecutive drags at the same header must start as single clicks.
        # A real OS down inside its double-click interval is a different action.
        time.sleep((user.GetDoubleClickTime() + 20) / 1000)
        user.EnumWindows(hide_preview, 0)
        user.SetWindowPos(root, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x53)
        user.SetForegroundWindow(root)
        deadline = time.monotonic() + 3
        while True:
            enabled = True
            ancestor = panel
            while ancestor:
                enabled = enabled and bool(user.IsWindowEnabled(ancestor))
                if ancestor == root:
                    break
                ancestor = user.GetParent(ancestor)
            if enabled and user.WindowFromPoint(point) == panel:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "The owned native pane did not become enabled and exposed."
                )
            time.sleep(0.01)
        user.SetCursorPos(point.x, point.y)
        positioned = True
        if any(user.GetAsyncKeyState(key) & 0x8000 for key in (1, 16, 17, 18, 91, 92)):
            raise RuntimeError("Human input changed before the owned drag began.")
        button(2)
        pressed = True
        thread = user.GetWindowThreadProcessId(panel, None)
        deadline = time.monotonic() + 1
        while True:
            state = Gui()
            state.size = ctypes.sizeof(state)
            if (
                user.GetGUIThreadInfo(thread, ctypes.byref(state))
                and state.capture == panel
            ):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "The owned pane did not capture the held mouse input."
                )
            time.sleep(0.01)
        user.SetThreadDpiAwarenessContext(user.GetWindowDpiAwarenessContext(panel))
        for step in range(1, 6):
            send(
                0x200,
                1,
                round(x0 + (x1 - x0) * step / 5),
                round(y0 + (y1 - y0) * step / 5),
            )
        if cancel:
            for kind in (0x100, 0x101):
                send(kind, 0x1B, 0, 0)
        send(0x202, 0, x1, y1)
        button(4)
        pressed = False
    finally:
        if pressed:
            send(0x100, 0x1B, 0, 0)
            button(4)
        user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        current = Point()
        user.GetCursorPos(ctypes.byref(current))
        if positioned and (current.x, current.y) == (point.x, point.y):
            user.SetCursorPos(original.x, original.y)
        if not pinned:
            user.SetWindowPos(root, ctypes.c_void_p(-2), 0, 0, 0, 0, 0x13)
        for window in hidden:
            user.ShowWindow(window, 4)
        if user.GetForegroundWindow() == root and foreground:
            user.SetForegroundWindow(foreground)
        if previous_dpi:
            user.SetThreadDpiAwarenessContext(previous_dpi)

    return panel


def capture_owned_project(panel, path):
    """Capture this test's project window to an artifact, without reading other windows."""
    from ctypes import wintypes

    from PIL import Image

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
        Image.frombuffer("RGB", (width, height), pixels, "raw", "BGRX", 0, 1).save(path)
    finally:
        gdi.SelectObject(memory, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(window, screen)


def check_groups(client, root):
    for name in ("tone-a.wav", "tone-b.wav"):
        client.file("Import2", root / name)
    client.send('GroupAudioClips: Clips="0:0,1:0"')
    groups = client.data("GetAudioClipGroups")
    group = groups[0]["group"]
    assert re.fullmatch("[0-9a-f]{32}", group) and groups[1]["group"] == group
    client.send('GroupAudioClips: Clips="0:0,1:0"')
    assert client.data("GetAudioClipGroups") == groups
    client.send("MoveAudioClipGroup: Track=0 At=1 Start=2")
    moved = client.data("GetAudioClipGroups")
    assert all(x["start"] == 2 and x["group"] == group for x in moved)
    client.send("MoveAudioClipGroup: Track=0 At=3 Start=2")
    assert client.data("GetAudioClipGroups") == moved
    client.send('GroupAudioClips: Clips="999:0"', success=False)
    assert client.data("GetAudioClipGroups") == moved
    client.send("Undo")
    assert client.data("GetAudioClipGroups") == groups
    client.send("Redo")
    save_reopen(client, root / "groups.aup3")
    assert client.data("GetAudioClipGroups") == moved
    client.send('UngroupAudioClips: Clips="0:0"')
    assert all(not x["group"] for x in client.data("GetAudioClipGroups"))
    client.send("Undo")
    assert client.data("GetAudioClipGroups") == moved
    client.send("Save")


def check_tracks(client, root):
    for name in ("tone-a.wav", "tone-b.wav", "tone-stereo.wav"):
        client.file("Import2", root / name)
    original = client.data("GetInfo: Type=Clips Format=JSON")
    client.send("AddAudioTrackBefore: Track=1")
    tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert len(tracks) == 4 and tracks[1]["channels"] == 1
    assert tracks[2]["name"] == "tone-b"
    client.send("Undo")
    assert len(client.data("GetInfo: Type=Tracks Format=JSON")) == 3
    client.send("Redo")
    client.send("AddAudioTrackAfter: Track=3")
    tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert len(tracks) == 5 and tracks[-1]["channels"] == 2
    client.send("AddAudioTrackBefore: Track=0")
    tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert len(tracks) == 6 and tracks[1]["name"] == "tone-a"
    client.send("AddAudioTrackBefore: Track=999", success=False)
    assert client.data("GetInfo: Type=Tracks Format=JSON") == tracks
    clips = client.data("GetInfo: Type=Clips Format=JSON")
    assert [(x["name"], x["start"], x["end"]) for x in clips] == [
        (x["name"], x["start"], x["end"]) for x in original
    ]
    save_reopen(client, root / "tracks.aup3")
    reopened = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [(x["name"], x["channels"]) for x in reopened] == [
        (x["name"], x["channels"]) for x in tracks
    ]


def decoded_frame_hashes(ffmpeg, path):
    data = run_process(
        [
            ffmpeg,
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:v:0",
            "-f",
            "framemd5",
            "-",
        ],
        text=True,
        timeout=60,
    )
    return [
        line.rsplit(",", 1)[-1].strip()
        for line in data.splitlines()
        if line and not line.startswith("#")
    ]


def check_video(client, root, ffmpeg, ffprobe):
    source = root / "source.mp4"
    source_bytes = hashlib.sha256(source.read_bytes()).hexdigest()
    import_children = client.data("GetVideoPreviewInfo")["importMenu"]
    assert import_children[0]["id"] == "ImportVideo", import_children
    video_import = next(row for row in import_children if row["id"] == "ImportVideo")
    assert video_import["label"].rstrip(".") == "Video", import_children
    client.file("ImportVideo", source)
    native_tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [track["kind"] for track in native_tracks] == ["video", "wave"], (
        native_tracks
    )
    strip = painted_video_tracks(client)
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    assert (
        strip["videoTrackLeftOffset"] > 0 and strip["viewEnd"] > strip["viewStart"]
    ), strip
    client.send("ZoomIn")
    assert client.data("GetVideoPreviewInfo")["viewEnd"] < strip["viewEnd"]
    client.send("ZoomOut")
    original = client.data("GetVideoClips")
    assert len(original["clips"]) == 1 and abs(original["end"] - 4) < 0.04
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=2 End=2")
    client.send("Split")
    split = client.data("GetVideoClips")
    assert len(split["clips"]) == 2 and split["clips"][1]["sourceStart"] == 2
    client.send("SplitVideo: At=2")
    assert client.data("GetVideoClips") == split
    copied = root / "mix with spaces & [picture].mp4"
    client.file("ExportVideoMix", copied)
    assert decoded_frame_hashes(ffmpeg, copied) == decoded_frame_hashes(ffmpeg, source)
    before = copied.read_bytes()
    client.file("ExportVideoMix", copied, success=False)
    assert copied.read_bytes() == before
    client.file("ExportVideoMix", source, "Overwrite=1", success=False)
    client.send("SelectAll")
    drag_owned_video_clip(client, 2.5, 3.5)
    together = client.data("GetVideoClips")["clips"]
    moved_tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    delta = together[0]["start"]
    # A mouse target resolves to pixels; preserve exact inter-track timing and
    # allow its endpoint to differ from a typed timestamp by one screen pixel.
    view = client.data("GetVideoPreviewInfo")
    assert (
        abs(delta - 1)
        <= (view["viewEnd"] - view["viewStart"]) / view["videoTrackDataWidth"]
    ), (together, moved_tracks)
    assert abs(together[1]["start"] - delta - 2) < 1e-8, together
    assert abs(moved_tracks[1]["start"] - delta) < 1e-8, (together, moved_tracks)
    client.send("Undo")
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("DeleteVideoClip: Id=1")
    client.send("DeleteVideoClip: Id=1")
    before_drag = client.data("GetVideoClips")["clips"]
    drag_owned_video_clip(client, 2.5, 1.5, cancel=True)
    assert client.data("GetVideoClips")["clips"] == before_drag
    panel = drag_owned_video_clip(client, 2.5, 1.5)
    moved = client.data("GetVideoClips")
    assert len(moved["clips"]) == 1 and moved["clips"][0]["start"] == 1
    capture_owned_project(panel, root / "_main-video-track.png")
    drag_owned_video_clip(client, 1.5, 1.5)
    assert client.data("GetVideoClips") == moved
    client.send("MoveVideoClip: Id=2 Start=1")
    assert client.data("GetVideoClips") == moved
    client.send("MoveVideoClip: Id=999 Start=1", success=False)
    assert client.data("GetVideoClips") == moved
    client.send("Undo")
    assert client.data("GetVideoClips")["clips"][0]["start"] == 2
    client.send("Redo")
    edited = root / "edited.mp4"
    client.file("ExportVideoMix", edited)
    info = json.loads(
        run_process(
            [
                ffprobe,
                "-v",
                "error",
                "-show_streams",
                "-show_format",
                "-of",
                "json",
                str(edited),
            ],
            text=True,
            timeout=30,
        )
    )
    assert abs(float(info["format"]["duration"]) - 3) < 0.08, info
    assert [s["codec_name"] for s in info["streams"]] == ["mpeg4", "aac"], info
    for when, blue in ((0.5, False), (1.5, True)):
        pixels = run_process(
            [
                ffmpeg,
                "-v",
                "error",
                "-ss",
                str(when),
                "-i",
                str(edited),
                "-frames:v",
                "1",
                "-vf",
                "scale=1:1",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "-",
            ],
            timeout=30,
        )
        assert len(pixels) == 3
        assert (pixels[2] > 200 and pixels[0] < 20) if blue else max(pixels) < 8, pixels
    height = painted_video_tracks(client)["videoTrackRows"][0]["height"]
    save_reopen(client, root / "video.aup3")
    assert client.data("GetVideoClips") == moved
    strip = painted_video_tracks(client)
    assert strip["videoTrackRows"][0]["height"] == height, strip
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("RemoveTracks")
    remaining = client.data("GetInfo: Type=Tracks Format=JSON")
    assert len(remaining) == 1 and remaining[0]["kind"] == "video", remaining
    assert client.data("GetVideoClips")["clips"] == moved["clips"]
    silent = root / "silent.mkv"
    client.file("ExportVideoMix", silent)
    samples = run_process(
        [ffmpeg, "-v", "error", "-i", str(silent), "-map", "0:a:0", "-f", "s16le", "-"],
        timeout=30,
    )
    assert len(samples) > 48000 and not any(samples)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_bytes
    assert not list(root.glob("_autopsy-video-export-*"))
    client.send("Save")
    check_video_tracks(client, root, ffmpeg, ffprobe)


def check_video_tracks(client, root, ffmpeg, ffprobe):
    """Exercise native track ownership, mixed sources and shared editing actions."""
    other = root / "other-source.mp4"
    client.file("ImportVideo", other)
    assert [
        track["kind"] for track in client.data("GetInfo: Type=Tracks Format=JSON")
    ] == ["video", "video"]
    layers = root / "layered.mp4"
    client.file("ExportVideoMix", layers)
    streams = json.loads(
        run_process(
            [ffprobe, "-v", "error", "-show_streams", "-of", "json", str(layers)],
            text=True,
            timeout=30,
        )
    )["streams"]
    assert (streams[0]["width"], streams[0]["height"]) == (90, 160), streams
    for when, blue in ((0.5, False), (1.5, True)):
        pixel = run_process(
            [
                ffmpeg,
                "-v",
                "error",
                "-ss",
                str(when),
                "-i",
                str(layers),
                "-frames:v",
                "1",
                "-vf",
                "crop=2:2:44:78,scale=1:1",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "-",
            ],
            timeout=30,
        )
        assert len(pixel) == 3
        assert (
            (pixel[2] > 200 and pixel[0] < 20)
            if blue
            else (pixel[0] > 200 and pixel[1] > 200)
        ), pixel
    # Seeking switches the same preview between sources and uses source time,
    # including an edited clip's offset. Await the decoder's asynchronous seek.
    for when, path, expected in ((0.5, other, 0.5), (1.5, root / "source.mp4", 2.5)):
        client.send(f"SelectTime: Start={when} End={when}")
        deadline = time.monotonic() + 5
        while True:
            info = client.data("GetVideoPreviewInfo")
            if (
                info["ready"]
                and Path(info["playbackPath"]).name == path.name
                and abs(info["position"] - expected) < 0.08
            ):
                break
            if time.monotonic() >= deadline:
                raise AssertionError(info)
            time.sleep(0.1)
    before = client.data("GetVideoClips")["clips"]
    client.send("MoveVideoClip: Track=0 Id=2 ToTrack=1 Start=0", success=False)
    assert client.data("GetVideoClips")["clips"] == before
    drag_owned_video_clip(client, 1.5, 1.5, track=0, to_track=1)
    assert client.data("GetVideoClips")["clips"] == before
    client.send("MoveVideoClip: Track=1 Id=1 Start=4")
    # With the soundtrack deleted, Space must still use the native transport
    # clock, play each source at its edited offset, and cross a black gap.
    client.send("SelectTime: Start=0 End=6")
    client.send("DefaultPlayStop")
    try:
        time.sleep(5.5)
    finally:
        client.send("Stop")
    report = client.data("GetVideoPreviewInfo")
    (root / "_playback.json").write_text(json.dumps(report["samples"], indent=2))
    playing = [sample for sample in report["samples"] if not sample["paused"]]
    assert {Path(sample["source"]).name for sample in playing} == {
        "source.mp4",
        "other-source.mp4",
    }, report
    stable = [
        sample
        for sample in playing
        if 1.3 < sample["audioPosition"] < 2.8 or sample["audioPosition"] > 4.3
    ]
    assert (
        len(stable) >= 10
        and max(
            abs(sample["position"] - sample["expectedPosition"]) for sample in stable
        )
        < 0.2
    ), report
    client.send("SelectTime: Start=0 End=0")
    painted_video_tracks(client)
    drag_owned_video_clip(client, 1.5, 1.5, track=0, to_track=1)
    combined = client.data("GetVideoClips")
    assert len(combined["tracks"]) == 2 and {
        clip["track"] for clip in combined["clips"]
    } == {1}, combined
    assert [
        (clip["start"], Path(clip["source"]).name) for clip in combined["clips"]
    ] == [(1, "source.mp4"), (4, "other-source.mp4")], combined
    client.send("Undo")
    assert {clip["track"] for clip in client.data("GetVideoClips")["clips"]} == {0, 1}
    client.send("Redo")
    assert client.data("GetVideoClips")["clips"] == combined["clips"]
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=1 End=3")
    client.send("Copy")
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=0 End=0")
    client.send("Paste")
    copied = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert (
        len(copied) == 1 and copied[0]["start"] == 0 and copied[0]["sourceStart"] == 2
    ), copied
    client.send("SelectTime: Start=0 End=2")
    client.send("Cut")
    assert not [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    client.send("Paste")
    pasted = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert [
        {key: value for key, value in clip.items() if key != "id"} for clip in pasted
    ] == [
        {key: value for key, value in clip.items() if key != "id"} for clip in copied
    ], pasted
    client.send("SelectTime: Start=.5 End=1.5")
    client.send("SplitNew")
    split = client.data("GetVideoClips")
    assert (
        len(split["tracks"]) == 3
        and len([clip for clip in split["clips"] if clip["track"] == 2]) == 1
    ), split
    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Set")
    client.send("Duplicate")
    assert len(client.data("GetVideoClips")["tracks"]) == 4
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=.1 End=1.9")
    client.send("Trim")
    trimmed = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert (
        abs(trimmed[0]["start"] - 0.1) < 0.001 and abs(trimmed[-1]["end"] - 1.9) < 0.001
    ), trimmed
    unchanged = client.data("GetVideoClips")
    client.send("AddVideoTrackBefore: Track=999", success=False)
    assert client.data("GetVideoClips") == unchanged
    client.send("AddVideoTrackBefore: Track=1")
    client.send("AddVideoTrackAfter: Track=1")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 6 and not [
        clip for clip in state["clips"] if clip["track"] in (1, 2)
    ], state
    for _ in range(3):
        client.send("NewVideoTrack")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 9
    # New native rows scroll into view; earlier rows scroll out with audio rows.
    time.sleep(0.2)
    rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
    assert rows[0]["y"] < 0 and rows[-1]["y"] >= 0, rows
    save_reopen(client, root / "video.aup3", save_existing=True)
    restored = client.data("GetVideoClips")
    assert restored["clips"] == state["clips"] and len(restored["tracks"]) == 9, (
        restored
    )
    client.send("SelectTracks: Track=8 TrackCount=1 Mode=Set")
    client.send("RemoveTracks")
    assert len(client.data("GetVideoClips")["tracks"]) == 8
    client.send("Save")
    client.send("Close")
    offline = other.with_name("_offline-source.mp4")
    other.rename(offline)
    try:
        client.file("OpenProject2", root / "video.aup3")
        client.send("FitV")
        time.sleep(0.2)
        rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
        assert rows[3]["y"] >= 0 and rows[3]["dataWidth"] > 0, rows
        unchanged = layers.read_bytes()
        for _ in range(2):
            client.file("ExportVideoMix", layers, "Overwrite=1", success=False)
        assert layers.read_bytes() == unchanged
        client.send("Save")
        client.send("Close")
    finally:
        offline.rename(other)
    client.file("OpenProject2", root / "video.aup3")


def check_branding(client, root):
    """Read native menus and exercise only the owned child's welcome/about UI."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.GetMenu.argtypes = [ctypes.c_void_p]
    user.GetMenu.restype = ctypes.c_void_p
    user.GetSubMenu.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetSubMenu.restype = ctypes.c_void_p
    user.GetMenuItemCount.argtypes = [ctypes.c_void_p]
    user.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    user.GetMenuItemID.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetMenuStringW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_wchar_p,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    user.PostMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])

    def windows():
        result = {}

        @callback_type
        def visit(window, _):
            owner = ctypes.c_uint32()
            user.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == pid:
                text = ctypes.create_unicode_buffer(256)
                user.GetWindowTextW(window, text, len(text))
                result[window] = text.value
            return 1

        user.EnumWindows(visit, 0)
        return result

    def wait_title(title):
        deadline = time.monotonic() + 5
        while True:
            found = [window for window, text in windows().items() if text == title]
            if len(found) == 1:
                return found[0]
            if time.monotonic() >= deadline:
                raise AssertionError(windows())
            time.sleep(0.05)

    welcome = wait_title("Welcome to AuTopsy!")
    assert user.PostMessageW(welcome, 0x10, 0, 0), ctypes.WinError(
        ctypes.get_last_error()
    )

    def entries(menu):
        result = []
        for index in range(user.GetMenuItemCount(menu)):
            text = ctypes.create_unicode_buffer(256)
            user.GetMenuStringW(menu, index, text, len(text), 0x400)
            result.append(
                (
                    text.value.replace("&", "").split("\t")[0],
                    user.GetMenuItemID(menu, index),
                    user.GetSubMenu(menu, index),
                )
            )
        return result

    # wx also owns hidden menu-bearing windows. Select the editor through its
    # Help menu contract, never the enumeration order, and wait for startup UI.
    deadline = time.monotonic() + 5
    while True:
        menu_windows = {
            window: entries(user.GetMenu(window))
            for window in windows()
            if user.GetMenu(window)
        }
        candidates = [
            (window, child)
            for window, menu in menu_windows.items()
            for text, _, child in menu
            if text == "Help" and user.IsWindowEnabled(window)
        ]
        if len(candidates) == 1:
            main, help_menu = candidates[0]
            break
        if time.monotonic() >= deadline:
            raise AssertionError({"windows": windows(), "menus": menu_windows})
        time.sleep(0.05)

    # Deliberately request screen 1 while hidden and visible. The in-process
    # placement adapter must constrain the request before Windows applies it,
    # including repeated hide/show and an oversized window. No mouse is used.
    from ctypes import wintypes as w

    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    user.SetWindowPos.argtypes = [
        w.HWND,
        w.HWND,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    original = w.RECT()
    assert user.GetWindowRect(main, ctypes.byref(original))
    displays = connected_test_displays()
    target = select_test_display(displays)
    outside = next(
        (row for row in displays if row["device"] != target["device"]), target
    )
    for _ in range(2):
        user.ShowWindow(main, 0)
        assert user.SetWindowPos(
            main, None, outside["work"][0], outside["work"][1], 2000, 1500, 0x14
        ), ctypes.WinError(ctypes.get_last_error())
        assert_owned_test_display(pid, main)
        user.ShowWindow(main, 4)
        assert_owned_test_display(pid, main)
        assert user.SetWindowPos(
            main, None, outside["work"][0], outside["work"][1], 2000, 1500, 0x14
        ), ctypes.WinError(ctypes.get_last_error())
        assert_owned_test_display(pid, main)
        assert user.SetWindowPos(
            main,
            None,
            original.left,
            original.top,
            original.right - original.left,
            original.bottom - original.top,
            0x14,
        )
        assert_owned_test_display(pid, main)
    help_entries = entries(help_menu)
    labels = [text for text, _, _ in help_entries]
    assert {
        "AuTopsy Guide",
        "Audio Manual (Upstream)",
        "AuTopsy Support",
        "About AuTopsy",
    } <= set(labels), labels
    assert not any(
        "Audacity" in text or "Check for Updates" in text for text in labels
    ), labels
    about_id = next(
        command for text, command, _ in help_entries if text == "About AuTopsy"
    )
    for _ in range(2):
        assert_owned_test_display(pid)
        assert user.PostMessageW(main, 0x111, about_id, 0), ctypes.WinError(
            ctypes.get_last_error()
        )
        about = wait_title("About AuTopsy")
        assert_owned_test_display(pid, about)
        assert user.PostMessageW(about, 0x10, 0, 0), ctypes.WinError(
            ctypes.get_last_error()
        )
        deadline = time.monotonic() + 5
        while any(text == "About AuTopsy" for text in windows().values()):
            if time.monotonic() >= deadline:
                raise AssertionError(windows())
            time.sleep(0.05)
    (root / "_branding.json").write_text(
        json.dumps(
            {"help": labels, "welcome": "AuTopsy", "about_open_close_count": 2},
            indent=2,
        ),
        encoding="utf-8",
    )


def worker(args, root):
    client = ScriptClient()
    if args.case == "groups":
        check_groups(client, root)
    elif args.case == "tracks":
        check_tracks(client, root)
    elif args.case == "video":
        check_video(client, root, args.ffmpeg, args.ffprobe)
    elif args.case == "branding":
        check_branding(client, root)
    else:
        source = args.video_source or root / "source.mp4"
        client.file("ImportVideo", source)
        info = ready_video_preview(client)
        assert info["lightweight"] and info["thumbnails"], info
        assert 0 < info["width"] <= 960 and 0 < info["height"] <= 540, info
        assert client.data("GetInfo: Type=Tracks Format=JSON"), (
            "The original soundtrack was not imported."
        )
        client.send("SelectAll")
        client.send("SetTrackAudio: Mute=1")
        client.send("SelectTime: Start=2 End=14")
        # Batch Play waits for the selection to finish. The normal Space action
        # starts immediately, allowing performance samples during playback.
        client.send("DefaultPlayStop")
        samples = []
        try:
            time.sleep(7)
        finally:
            client.send("Stop")
        report = client.data("GetVideoPreviewInfo")
        samples = report["samples"]
        assert len(samples) >= 30, report
        samples = [
            sample
            for sample in samples
            if sample["audioPosition"] > samples[0]["audioPosition"] + 1
        ]
        (root / "_playback.json").write_text(json.dumps(samples, indent=2))
        assert all(not sample["paused"] for sample in samples), samples
        assert (
            max(abs(sample["audioPosition"] - sample["position"]) for sample in samples)
            < 0.2
        ), samples
        assert samples[-1]["position"] - samples[0]["position"] > 4, samples
        assert samples[-1]["droppedFrames"] - samples[0]["droppedFrames"] <= 2, samples
        assert (
            samples[-1]["decoderDroppedFrames"] - samples[0]["decoderDroppedFrames"]
            <= 2
        ), samples
        save_reopen(client, root / "preview.aup3")
        assert ready_video_preview(client)["lightweight"]
        client.send("Save")
    client.send("Close")
    client.send("Exit")


def retain_behavior_result(work_root, record=None, metrics=None):
    """Keep immutable result identities across fresh test runs; bound each scope to three.

    The owner marker confines cleanup to this runner's receipts. Receipts describe
    accepted work only: they never skip execution or resume an interrupted test.
    """
    archive = work_root / "_accepted"
    owner = archive / "_owner"
    signature = "AuTopsy accepted behavior results 1"
    if archive.exists():
        if not owner.is_file() or owner.read_text() != signature:
            raise RuntimeError("The result archive has another owner: " + str(archive))
    else:
        archive.mkdir()
        owner.write_text(signature)
    if record is not None:
        if not re.fullmatch(r"[0-9a-f]{32}", record["run"]):
            raise RuntimeError("The result identity is malformed.")
        target = archive / ("_" + record["run"] + ".json")
        if not target.exists():
            payload = dict(record)
            if metrics is not None:
                payload["observations"] = metrics
            # The exclusive name is never reused; interrupted writes are discarded
            # rather than accepted as a successful record.
            staging = archive / ("_" + record["run"] + ".tmp")
            staging.write_text(json.dumps(payload, indent=2), encoding="utf-8")
            staging.replace(target)
    rows = []
    for path in archive.glob("_*.json"):
        value = json.loads(path.read_text(encoding="utf-8"))
        scope = (value["case"], json.dumps(value.get("input"), sort_keys=True))
        rows.append((path.stat().st_mtime_ns, path, scope))
    counts: dict[tuple[str, str], int] = {}
    retained = 0
    for _, path, scope in sorted(rows, reverse=True):
        counts[scope] = counts.get(scope, 0) + 1
        if counts[scope] > 3 or retained >= 36:
            path.unlink()
        else:
            retained += 1
    for path in archive.glob("_*.tmp"):
        path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument(
        "--case",
        choices=(
            "groups",
            "tracks",
            "video",
            "preview",
            "branding",
            "display-policy",
            "worker-lifecycle",
        ),
        required=True,
    )
    parser.add_argument(
        "--video-source", type=Path, default=os.environ.get("AUTOPSY_PREVIEW_SOURCE")
    )
    parser.add_argument(
        "--interrupt-cache",
        choices=("before-publish", "after-publish"),
        default=os.environ.get("AUTOPSY_INTERRUPT_PREVIEW"),
    )
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--ffprobe", default="")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--reject-display", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.case == "display-policy":
        check_test_display_selection()
        return
    if args.case == "worker-lifecycle":
        if not args.work_root:
            parser.error("Worker lifecycle checks require --work-root.")
        check_behavior_worker_lifecycle(args.work_root.resolve())
        return
    if not args.executable or not args.work_root:
        parser.error("GUI cases require --executable and --work-root.")
    root = args.work_root.resolve() / ("_" + args.case)
    if args.reject_display:
        root = root.with_name(root.name + "_display_rejection")
    if args.worker:
        worker(args, root)
        return
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
    retain_behavior_result(root.parent)
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
        for result_path in root.glob("_result-*.json"):
            observations = root / "_playback.json"
            retain_behavior_result(
                root.parent,
                json.loads(result_path.read_text()),
                json.loads(observations.read_text())
                if observations.is_file()
                else None,
            )
        shutil.rmtree(root)
    root.mkdir()
    marker.write_text(signature)
    for name, channels in (
        ("tone-a.wav", 1),
        ("tone-b.wav", 1),
        ("tone-stereo.wav", 2),
    ):
        write_tone(root / name, channels)
    if args.case == "video":
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
    if args.case == "preview" and not args.video_source:
        run_process(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=s=1920x1080:r=30:d=16",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=48000:duration=16",
                "-c:v",
                "mpeg4",
                "-q:v",
                "5",
                "-c:a",
                "aac",
                "-shortest",
                str(root / "source.mp4"),
            ],
            timeout=60,
        )
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
    (profile / "autopsy.cfg").write_text(
        f"[Version]\nMajor={version_numbers[0]}\nMinor={version_numbers[1]}\nMicro={version_numbers[2]}\n"
        f"[Directories]\nTempDir={audio.as_posix()}\n[GUI]\nShowSplashScreen={int(args.case == 'branding')}\n"
        "[FileFormats]\nMusicFileImportSettingChoice=No\n[Module]\nmod-script-pipe=1\n"
        f"[ModuleDateTime]\nmod-script-pipe={stamp}\n" + placement,
        encoding="utf-8",
    )
    environment = dict(
        os.environ,
        TEMP=str(gui),
        TMP=str(gui),
        PYTHONDONTWRITEBYTECODE="1",
        AUTOPSY_BEHAVIOR_ROOT=str(args.work_root.resolve()),
        AUTOPSY_TEST_DISPLAY=display["device"]
        if not args.reject_display
        else "missing-test-display",
    )
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    run_id = uuid.uuid4().hex
    identities = {
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "script_module_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
        "check_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "pipe_client_sha256": hashlib.sha256(
            Path(__file__).with_name("pipeclient.py").read_bytes()
        ).hexdigest(),
    }
    with (root / "_app.log").open("w") as log:
        app = subprocess.Popen(
            [str(executable)],
            cwd=executable.parent,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            if args.reject_display:
                status = app.wait(timeout=30)
                log.flush()
                report = (root / "_app.log").read_text(
                    encoding="utf-8", errors="replace"
                )
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
                        "The owned test app did not expose its scripting pipe."
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
                str(Path(__file__)),
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
            result = run_behavior_worker(worker_command, environment, app, root)
            if result.returncode:
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
                raise RuntimeError(f"The owned app exited with status {status}.")
            log.flush()
            shows = re.findall(
                r"^AUTOPSY_TEST_WINDOW show (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$",
                (root / "_app.log").read_text(encoding="utf-8", errors="replace"),
                re.MULTILINE,
            )
            assert shows, "No pre-show placement observations were recorded."
            for _, x, y, w, h in shows:
                x, y, w, h = map(int, (x, y, w, h))
                assert left <= x and top <= y and x + w <= right and y + h <= bottom, (
                    shows
                )
            source = (
                args.video_source.resolve(strict=True)
                if args.video_source
                else root / "source.mp4"
            )
            source_identity = None
            if source.is_file():
                info = source.stat()
                source_identity = {
                    "source": str(source)
                    if args.video_source
                    else "generated:" + args.case,
                    "bytes": info.st_size,
                    "modified_ns": info.st_mtime_ns if args.video_source else None,
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
            (root / ("_result-" + run_id + ".json")).write_text(
                json.dumps(record, indent=2), encoding="utf-8"
            )
            observations = root / "_playback.json"
            retain_behavior_result(
                root.parent,
                record,
                json.loads(observations.read_text())
                if observations.is_file()
                else None,
            )
            print(args.case + ": PASS (native command, project and media checks)")
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=20)
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


if __name__ == "__main__":
    main()
