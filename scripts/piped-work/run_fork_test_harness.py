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
        if not user.SetWindowPos(root, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x53):
            raise RuntimeError(f"Cannot expose owned project {root}: Windows error {ctypes.get_last_error()}.")
        activated = user.SetForegroundWindow(root)
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
            if enabled and hit == panel:
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
                    f"root={root}; visible={user.IsWindowVisible(root)}; rect={list(root_rect)}; "
                    f"activated={activated}; topmost={bool(user.GetWindowLongPtrW(root, -20) & 8)}; "
                    f"pane_origin={(pane_origin.x,pane_origin.y)}; client_size={list(rectangle)}; "
                    f"panel={panel}; hit={hit}; obscurer_pid={obscurer_pid.value}; title={title.value!r}; "
                    f"point={(point.x, point.y)}; view={(info['viewStart'], info['viewEnd'])}; row={source}."
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
            try:
                # A failed/closed owned app has no remaining capture to cancel.
                user.IsWindow.argtypes = [ctypes.c_void_p]
                if user.IsWindow(panel):
                    send(0x100, 0x1B, 0, 0)
            finally:
                # Input release must survive a missing HWND or failed Escape.
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


