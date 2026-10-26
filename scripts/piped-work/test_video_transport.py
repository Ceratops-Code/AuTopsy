"""Owned preview-window controls, transport journeys and pixel observations.

All input and surface reads are confined to the private process on its required
secondary display. Helpers create no persistent output on successful checks.
"""
import ctypes
import math
import os
import re
import time

from run_fork_test_harness import (
    assert_owned_test_display,
    operate_owned_video_track,
    painted_video_tracks,
    ready_video_preview,
    run_process,
)


def check_preview_close(client, root):
    """Close projects before/during/after queued preview creation, then exit."""
    client.send("New")
    client.file("ImportVideo", root / "source.mp4")
    project = root / "_preview-close.aup3"
    client.file("SaveProject2", project)
    client.send("Close")
    for pause in (0, 0.025, 0.12)*6:
        client.file("OpenProject2", project)
        if pause:
            time.sleep(pause)
        # No readiness query may serialize away the queued load/close boundary.
        client.send("Close")


def crop_pixel_counts(rgb):
    """Classify decoded/captured fixture pixels; no source-text assumptions."""
    counts = {"red": 0, "green": 0, "blue": 0, "white": 0, "purple": 0}
    for offset in range(0, len(rgb) - 2, 3):
        red, green, blue = rgb[offset:offset + 3]
        if min(red, green, blue) > 180:
            counts["white"] += 1
        elif red > 160 and green < 60 and blue < 60:
            counts["red"] += 1
        elif green > 80 and red < 60 and blue < 60:
            counts["green"] += 1
        elif blue > 160 and red < 60 and green < 60:
            counts["blue"] += 1
        elif 60 < red < 195 and 60 < blue < 195 and green < 40:
            counts["purple"] += 1
    return counts


def preview_colours_match(counts, expected, edge_colours=()):
    """Require primary regions, allowing only explicitly named blended edges."""
    bad = sum(count for colour, count in counts.items() if colour not in expected and colour not in edge_colours)
    return all(counts[colour] > 20 for colour in expected) and bad < 5


def expect_preview_crop_pixels(client, root, ffmpeg, expected, *, preview_info=None, edge_colours=()):
    """Read only the owned preview surface on its required display.

    The bundled FFmpeg GDI input reads the decoder's GPU output without adding
    packages to the persistent plain-Python test runtime. The owned window is
    temporarily raised without activation and its prior topmost state is
    restored. Successful observations create no screenshot artifact.
    """
    from ctypes import wintypes as w

    # A caller observing a live/paused transport supplies its previously read
    # handles: a fresh configured script command would stop that transport.
    info = ready_video_preview(client) if preview_info is None else preview_info
    surface = int(info["previewSurfaceHandle"])
    native = PreviewNativeWindows()
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    assert native.owner(surface) == pid
    user = native.api
    for name, arguments, result in (
        ("GetAncestor", [w.HWND, w.UINT], w.HWND),
        ("GetWindowRect", [w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
        ("GetWindowLongW", [w.HWND, ctypes.c_int], w.LONG),
        ("SetWindowPos", [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT], w.BOOL),
        ("SetThreadDpiAwarenessContext", [ctypes.c_void_p], ctypes.c_void_p),
        ("WindowFromPoint", [w.POINT], w.HWND),
    ):
        operation = getattr(user, name)
        operation.argtypes, operation.restype = arguments, result
    window = user.GetAncestor(surface, 2)
    assert native.owner(window) == pid
    assert user.IsWindowVisible(surface), "Open the owned preview before pixel capture."
    assert_owned_test_display(pid, window)
    previous_dpi = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    was_topmost = bool(user.GetWindowLongW(window, -20) & 8)
    try:
        assert user.SetWindowPos(window, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x13)
        deadline = time.monotonic() + 8
        while True:
            rectangle = w.RECT()
            assert user.GetWindowRect(surface, ctypes.byref(rectangle))
            bounds = (rectangle.left, rectangle.top, rectangle.right, rectangle.bottom)
            assert 0 < rectangle.right - rectangle.left < 6000
            assert 0 < rectangle.bottom - rectangle.top < 4000
            for x, y in ((rectangle.left + 4, rectangle.top + 4),
                         (rectangle.right - 4, rectangle.bottom - 4),
                         ((rectangle.left + rectangle.right) // 2,
                          (rectangle.top + rectangle.bottom) // 2)):
                assert user.GetAncestor(user.WindowFromPoint(w.POINT(x, y)), 2) == window, "Owned preview capture is obstructed."
            rgb = run_process([
                ffmpeg, "-v", "error", "-nostdin", "-f", "gdigrab", "-draw_mouse", "0",
                "-framerate", "30", "-offset_x", str(bounds[0]), "-offset_y", str(bounds[1]),
                "-video_size", f"{bounds[2] - bounds[0]}x{bounds[3] - bounds[1]}", "-i", "desktop",
                "-frames:v", "1", "-vf", "scale=64:64:flags=neighbor",
                "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1",
            ], timeout=15)
            assert len(rgb) == 64 * 64 * 3, len(rgb)
            counts = crop_pixel_counts(rgb)
            if preview_colours_match(counts, expected, edge_colours):
                return counts
            if time.monotonic() >= deadline:
                evidence = root / "_crop-preview.ppm"
                evidence.write_bytes(b"P6\n64 64\n255\n" + rgb)
                raise AssertionError(f"Preview crop pixels {counts}; expected {expected}; {evidence}")
            time.sleep(0.05)
    finally:
        if not was_topmost:
            user.SetWindowPos(window, ctypes.c_void_p(-2), 0, 0, 0, 0, 0x13)
        if previous_dpi:
            user.SetThreadDpiAwarenessContext(previous_dpi)


def wait_for_native_state(condition, description, timeout=8):
    """Wait for an asynchronous GUI/audio state; never replay its effect."""
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() >= deadline:
            raise AssertionError("Native state did not arrive: " + (description() if callable(description) else description)
                                 + "; owned windows: " + str(read_owned_native_windows()))
        time.sleep(0.025)


def read_owned_native_windows():
    """Bound failure evidence to visible windows owned by this test process."""
    observations: list[dict[str, object]] = []
    pid = int(os.environ.get("AUTOPSY_OWNED_APP_PID", "0"))
    if pid:
        native = PreviewNativeWindows()
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
        native.api.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
        native.api.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
        native.api.GetForegroundWindow.restype = ctypes.c_void_p
        @callback_type
        def collect(handle, _):
            if native.owner(handle) == pid and native.visible(handle) and len(observations) < 12:
                controls: list[dict[str, object]] = []
                @callback_type
                def child(control, _):
                    if native.visible(control) and len(controls) < 16:
                        text = native.caption(control)
                        if text:
                            controls.append({"text": text[:160], "enabled": bool(native.enabled(control))})
                    return 1
                native.api.EnumChildWindows(handle, child, 0)
                observations.append({"handle": handle, "title": native.caption(handle),
                                     "enabled": bool(native.enabled(handle)),
                                     "foreground": native.api.GetForegroundWindow() == handle,
                                     "controls": controls})
            return 1
        native.api.EnumWindows(collect, 0)
    return observations


class PreviewNativeWindows:
    """Use the harness's retained plain Python and the stable Win32 window API.

    The compiler records that interpreter outside uv's disposable overlays.
    ctypes keeps GUI test dependencies available for a later ordinary CTest call;
    pointer-sized signatures and PID checks bound effects to the owned process.
    """

    def __init__(self):
        from ctypes import wintypes as w

        self.api = ctypes.WinDLL("user32", use_last_error=True)
        signatures = {
            "GetWindowThreadProcessId": ([w.HWND, ctypes.POINTER(w.DWORD)], w.DWORD),
            "IsWindowEnabled": ([w.HWND], w.BOOL),
            "IsWindowVisible": ([w.HWND], w.BOOL),
            "GetWindowTextW": ([w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
            "SendMessageTimeoutW": ([w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT,
                                     ctypes.POINTER(ctypes.c_size_t)], ctypes.c_size_t),
        }
        for name, (arguments, result) in signatures.items():
            operation = getattr(self.api, name)
            operation.argtypes, operation.restype = arguments, result

    def owner(self, handle):
        owner = ctypes.c_uint32()
        if not self.api.GetWindowThreadProcessId(handle, ctypes.byref(owner)):
            raise RuntimeError(f"Cannot read native window owner for HWND={handle}: {ctypes.WinError()}")
        return owner.value

    def enabled(self, handle):
        return bool(self.api.IsWindowEnabled(handle))

    def visible(self, handle):
        return bool(self.api.IsWindowVisible(handle))

    def caption(self, handle):
        text = ctypes.create_unicode_buffer(512)
        self.api.GetWindowTextW(handle, text, len(text))
        return text.value

    def control_text(self, handle):
        """Read live owned control text; cross-process captions can be stale.

        Windows marshals WM_GETTEXT for another process. A bounded send avoids
        hanging the worker if that process stops responding; caption() remains
        the inexpensive identity lookup for window titles and button labels.
        """
        assert self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        text = ctypes.create_unicode_buffer(512)
        reply = ctypes.c_size_t()
        if not self.api.SendMessageTimeoutW(handle, 0xD, len(text), ctypes.addressof(text),
                                           2, 1000, ctypes.byref(reply)):
            raise RuntimeError(f"Native WM_GETTEXT failed for HWND={handle}: {ctypes.WinError()}")
        return text.value

    def active_in_thread(self, handle):
        """Dialog activation is thread-local; foreground belongs to the desktop.

        BM_CLICK requires the former. Requiring the latter unnecessarily blocks
        owned direct messages whenever another app retains foreground focus.
        """
        from ctypes import wintypes as w
        class Gui(ctypes.Structure):
            _fields_ = [("size", w.DWORD), ("flags", w.DWORD),
                        ("active", w.HWND), ("focus", w.HWND), ("capture", w.HWND),
                        ("menu", w.HWND), ("move", w.HWND), ("caret", w.HWND), ("rectangle", w.RECT)]
        assert self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        self.api.GetGUIThreadInfo.argtypes = [w.DWORD, ctypes.POINTER(Gui)]
        self.api.GetGUIThreadInfo.restype = w.BOOL
        state = Gui()
        state.size = ctypes.sizeof(state)
        thread = self.api.GetWindowThreadProcessId(handle, None)
        return bool(self.api.GetGUIThreadInfo(thread, ctypes.byref(state))) and state.active == handle

    def click(self, handle):
        assert self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        wait_for_native_state(lambda: self.enabled(handle), f"preview button HWND={handle} enabled")
        reply = ctypes.c_size_t()
        from ctypes import wintypes as w
        self.api.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        kind = ctypes.create_unicode_buffer(128)
        self.api.GetClassNameW(handle, kind, len(kind))
        if kind.value == "Button":
            # Dialog buttons use Windows' button command; AButton below owns a
            # different press/release protocol. Block an inactive dialog before
            # the effect: Windows can otherwise silently ignore BM_CLICK.
            self.api.GetAncestor.argtypes = [w.HWND, w.UINT]
            self.api.GetAncestor.restype = w.HWND
            self.api.GetForegroundWindow.restype = w.HWND
            dialog = self.api.GetAncestor(handle, 2)
            assert self.owner(dialog) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
            wait_for_native_state(lambda: self.active_in_thread(dialog),
                                  f"owned dialog HWND={dialog} active before button HWND={handle}")
            if not self.api.SendMessageTimeoutW(handle, 0xF5, 0, 0, 2, 10000, ctypes.byref(reply)):
                raise RuntimeError(f"Native dialog button click failed for HWND={handle}: {ctypes.WinError()}")
            return
        self.api.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
        rectangle = w.RECT()
        assert self.api.GetClientRect(handle, ctypes.byref(rectangle))
        centre = rectangle.right // 2 | (rectangle.bottom // 2 << 16)
        for code, flags, point in ((0x201, 1, centre), (0x202, 0, centre), (0x202, 0, -1)):
            if not self.api.SendMessageTimeoutW(handle, code, flags, point, 2, 10000, ctypes.byref(reply)):
                raise RuntimeError(f"Native transport mouse message {code:#x} failed for HWND={handle}: {ctypes.WinError()}")


def check_transport_toolbar(client, root):
    """Exercise native-size controls and cursor visibility without shared input.

    Window identity comes from the owned button's caption and parent, never
    toolbar positions or upstream numeric IDs. Native mouse messages traverse
    AButton's normal press/release path, including cancellation before release.
    The worker owns its generated audio and saves before its normal shutdown.
    """
    import wave
    from ctypes import wintypes as w

    native = PreviewNativeWindows()
    user = native.api
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.EnumChildWindows.argtypes = [w.HWND, callback_type, w.LPARAM]
    user.GetParent.argtypes, user.GetParent.restype = [w.HWND], w.HWND
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.GetWindowDpiAwarenessContext.argtypes = [w.HWND]
    user.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    matches = []

    @callback_type
    def visit_child(handle, parent):
        if native.caption(handle) == "Go to Cursor":
            matches.append((parent, handle))
        return True

    @callback_type
    def visit_window(handle, _):
        if native.owner(handle) == pid and native.visible(handle):
            user.EnumChildWindows(handle, visit_child, handle)
        return True

    user.EnumWindows(visit_window, 0)
    assert len(matches) == 1, matches
    main, cursor_button = matches[0]
    assert_owned_test_display(pid, main)
    toolbar = user.GetParent(cursor_button)
    buttons = {}

    @callback_type
    def visit_button(handle, _):
        if user.GetParent(handle) == toolbar:
            caption = native.caption(handle)
            if caption:
                assert caption not in buttons, caption
                buttons[caption] = handle
        return True

    user.EnumChildWindows(toolbar, visit_button, 0)
    expected = {"Pause", "Play", "Stop", "Skip to Start", "Skip to End", "Record", "Enable Looping", "Go to Cursor"}
    assert expected <= set(buttons), buttons
    previous_dpi = user.SetThreadDpiAwarenessContext(user.GetWindowDpiAwarenessContext(toolbar))
    try:
        for label in expected:
            handle = buttons[label]
            assert native.owner(handle) == pid
            rectangle = w.RECT()
            assert user.GetWindowRect(handle, ctypes.byref(rectangle))
            assert (rectangle.right - rectangle.left, rectangle.bottom - rectangle.top) == (52, 52), (label, rectangle)
    finally:
        if previous_dpi:
            user.SetThreadDpiAwarenessContext(previous_dpi)
    def press(handle, cancel=False):
        assert native.owner(handle) == pid and native.enabled(handle)
        reply = ctypes.c_size_t()

        def send(message, flags, position):
            if not user.SendMessageTimeoutW(handle, message, flags, position, 2, 2000, ctypes.byref(reply)):
                raise RuntimeError(f"Owned transport button message {message:#x} failed: {ctypes.WinError()}")

        # Resolve the actual client rectangle; native theme/DPI sizing owns it.
        # An outside release cancels through the same path as a user's gesture.
        rectangle = w.RECT()
        assert user.GetClientRect(handle, ctypes.byref(rectangle))
        centre = ((rectangle.right - rectangle.left) // 2
                  | ((rectangle.bottom - rectangle.top) // 2 << 16))
        send(0x201, 1, centre)
        try:
            send(0x202, 0, 0xFFFFFFFF if cancel else centre)
        finally:
            send(0x202, 0, 0xFFFFFFFF)

    # AButton deliberately leaves its Windows HWND enabled for tooltips even
    # when its application state is disabled. Check the resulting behavior,
    # rather than mistaking IsWindowEnabled for the custom button's state.
    empty_view = client.data("GetInfo: Type=View Format=JSON")
    press(cursor_button)
    assert client.data("GetInfo: Type=View Format=JSON") == empty_view
    assert not client.data("GetInfo: Type=Tracks Format=JSON")

    client.file("ImportVideo", root / "source.mp4")
    initial_content = client.data("GetInfo: Type=Clips Format=JSON")
    preview, preview_buttons = owned_preview_controls(client)
    status = int(preview["previewStatusHandle"])
    source = root / "_toolbar-tone.wav"
    with wave.open(str(root / "tone-a.wav"), "rb") as original:
        parameters, audio = original.getparams(), original.readframes(original.getnframes())
    with wave.open(str(source), "wb") as output:
        output.setparams(parameters)
        output.writeframes(audio * 4)
    client.file("Import2", source)
    content = client.data("GetInfo: Type=Clips Format=JSON")

    def viewport():
        view = client.data("GetInfo: Type=View Format=JSON")
        view["horizontalEnd"] = client.data("GetVideoPreviewInfo")["viewEnd"]
        return view

    for target, view_start in ((18, 1), (1, 18)):
        client.send(f"SelectTime: Start={view_start} End={view_start + 0.5}")
        client.send("ZoomSel")
        client.send(f"SelectTime: Start={target} End={target + 0.25}")
        before = viewport()
        selection = client.data("GetInfo: Type=Selection Format=JSON")
        assert not before["horizontalStart"] <= target <= before["horizontalEnd"], before
        press(cursor_button, cancel=True)
        assert viewport() == before
        for _ in range(2):
            press(cursor_button)
            view = viewport()
            assert view["horizontalStart"] <= target <= view["horizontalEnd"], view
            assert view["pixelsPerSecond"] == before["pixelsPerSecond"]
            assert client.data("GetInfo: Type=Selection Format=JSON") == selection
            assert client.data("GetInfo: Type=Clips Format=JSON") == content

    # Scripted data commands stop transport before their handler runs. Read the
    # live preview label during playback, and query project data only after Stop.
    for paused in (False, True):
        client.send("SelectTime: Start=18 End=18.5")
        client.send("ZoomSel")
        before = viewport()
        client.send("SelectTime: Start=1 End=19")
        press(buttons["Play"])
        wait_for_native_state(lambda: preview_project_seconds(status) > 1.15,
                              "native toolbar playback clock")
        if paused:
            press(buttons["Pause"])
            # Pause remains available as Resume, matching the main toolbar.
            # Availability plus a stationary clock establishes the actual
            # paused state without assuming an obsolete disabled-button style.
            wait_for_native_state(lambda: native.enabled(preview_buttons["play"]) and
                                  native.enabled(preview_buttons["stop"]),
                                  "native toolbar paused")
            # Control availability can update before the decoder's last clock
            # sample. Establish a stationary clock before testing navigation;
            # an actually running stream cannot satisfy this precondition.
            clock = [preview_project_seconds(status), time.monotonic()]

            def paused_clock_settled(clock=clock):
                current = preview_project_seconds(status)
                if not math.isfinite(current) or current != clock[0]:
                    clock[:] = [current, time.monotonic()]
                return math.isfinite(current) and time.monotonic() - clock[1] >= 0.15

            wait_for_native_state(paused_clock_settled, "stationary paused preview clock")
        position = preview_project_seconds(status)
        press(cursor_button)
        if paused:
            time.sleep(0.15)
            assert abs(preview_project_seconds(status) - position) < 0.05, (position, native.control_text(status))
            press(buttons["Pause"])
            wait_for_native_state(lambda position=position: preview_project_seconds(status) > position + 0.08,
                                  "Pause resumes playback after cursor navigation")
        else:
            wait_for_native_state(lambda position=position: preview_project_seconds(status) > position + 0.08,
                                  "cursor navigation preserves playback")
        press(buttons["Stop"])
        wait_for_native_state(lambda: not native.enabled(preview_buttons["stop"]),
                              "native toolbar stopped")
        view = viewport()
        assert view["horizontalStart"] - 0.2 <= position <= view["horizontalEnd"] + 0.2, (position, view)
        assert view["pixelsPerSecond"] == before["pixelsPerSecond"]
        assert client.data("GetInfo: Type=Selection Format=JSON") == {"Start": 1, "End": 19}
    client.send("Undo")
    assert client.data("GetInfo: Type=Clips Format=JSON") == initial_content, "Cursor navigation added a history entry."
    client.send("Redo")
    assert client.data("GetInfo: Type=Clips Format=JSON") == content
    client.file("SaveProject2", root / "_transport-toolbar.aup3")


def owned_preview_controls(client):
    """Use real native button HWNDs, bound to this test PID and second display."""
    windows = PreviewNativeWindows()

    info = ready_video_preview(client)
    frame = int(info["previewFrameHandle"])
    owner = windows.owner(frame)
    assert owner == int(os.environ["AUTOPSY_OWNED_APP_PID"])
    assert_owned_test_display(owner, include_window=frame)
    handles = {item["action"]: int(item["handle"]) for item in info["transportControls"]}
    for handle in handles.values():
        assert handle and windows.owner(handle) == owner
    status = int(info["previewStatusHandle"])
    wait_for_native_state(lambda: bool(re.search(r"\|\s+(-?\d+(?:\.\d+)?) s", windows.control_text(status))),
                          lambda: "preview timeline label: " + windows.control_text(status))
    return info, handles


def press_preview_button(handle):
    """A native button event does not borrow shared mouse or keyboard input."""
    PreviewNativeWindows().click(handle)


def preview_project_seconds(status):
    caption = PreviewNativeWindows().control_text(status)
    # Frame inspection may switch from a proxy to the original source. Its
    # transient loading label is an unavailable observation, never time zero.
    if caption == "Loading video...":
        return float("nan")
    match = re.search(r"\|\s+(-?\d+(?:\.\d+)?) s", caption)
    assert match, caption
    return float(match[1])


def check_video_transport(client, root):
    """Real controls share native audio, pause/resume, silent clock and ownership."""
    windows = PreviewNativeWindows()

    client.file("ImportVideo", root / "source.mp4")
    client.send("FitInWindow")
    for silent in (False, True):
        if silent:
            videos = {row["index"] for row in client.data("GetVideoClips")["tracks"]}
            tracks = client.data("GetInfo: Type=Tracks Format=JSON")
            for track in reversed([index for index in range(len(tracks)) if index not in videos]):
                client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
                client.send("RemoveTracks")
        for _ in range(2):
            client.send("SelectTime: Start=0 End=0")
            info, buttons = owned_preview_controls(client)
            status = int(info["previewStatusHandle"])
            press_preview_button(buttons["play"])
            wait_for_native_state(lambda buttons=buttons: windows.enabled(buttons["pause"]), "project playing")
            wait_for_native_state(lambda status=status: preview_project_seconds(status) > 0.15, "project clock advances")
            assert not windows.enabled(buttons["play"])
            press_preview_button(buttons["pause"])
            wait_for_native_state(lambda buttons=buttons: windows.enabled(buttons["play"]), "project paused")
            time.sleep(0.08)
            paused = preview_project_seconds(status)
            time.sleep(0.2)
            assert abs(preview_project_seconds(status) - paused) < 0.025
            assert windows.enabled(buttons["stop"])
            press_preview_button(buttons["play"])
            wait_for_native_state(lambda status=status, paused=paused: preview_project_seconds(status) > paused + 0.12, "resumed project clock")
            press_preview_button(buttons["stop"])
            wait_for_native_state(lambda buttons=buttons: not windows.enabled(buttons["stop"]), "project stopped")
    # Live picture mute must keep native playback running. This gesture uses
    # the same already-painted, fixed control row, avoiding a scripting command
    # whose native dispatch would stop the project before the click.
    client.send("SelectTime: Start=0 End=0")
    painted = painted_video_tracks(client)
    info, buttons = owned_preview_controls(client)
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: windows.enabled(buttons["pause"]), "silent video playback")
    video_track = painted["videoTrackRows"][0]["index"]
    operate_owned_video_track(client, 0.5, track=video_track, gesture="click", area="mute", painted=painted)
    wait_for_native_state(lambda: not windows.visible(int(info["previewSurfaceHandle"])), "muted picture hidden")
    assert windows.enabled(buttons["pause"]), "Picture mute stopped native playback"
    operate_owned_video_track(client, 0.5, track=video_track, gesture="click", area="mute", painted=painted)
    wait_for_native_state(lambda: windows.visible(int(info["previewSurfaceHandle"])), "picture unmuted")
    press_preview_button(buttons["stop"])
    wait_for_native_state(lambda: not windows.enabled(buttons["stop"]), "live visibility stopped")
    client.file("SaveProject2", root / "_transport-first.aup3")
    first = buttons
    client.send("New")
    client.file("ImportVideo", root / "source.mp4")
    _, second = owned_preview_controls(client)
    press_preview_button(second["play"])
    wait_for_native_state(lambda: windows.enabled(second["pause"]) and
                          not any(windows.enabled(handle) for handle in first.values()),
                          "other project owns playback")
    time.sleep(0.15)
    assert windows.enabled(second["stop"])
    press_preview_button(second["pause"])
    wait_for_native_state(lambda: windows.enabled(second["play"]), "other project paused")
    assert not any(windows.enabled(handle) for handle in first.values())
    press_preview_button(second["stop"])
    wait_for_native_state(lambda: windows.enabled(first["play"]), "first project regains controls")
    client.file("SaveProject2", root / "_transport-second.aup3")


def check_preview_navigation(client, root, ffmpeg):
    """Exercise focused preview keys and the real slider without live script queries.

    The preview's status is the clock observer. Scripted commands would stop the
    stream before returning their data, so content/volume are read only at rest.
    Every message is bound to the test PID and its guarded preview window.
    """
    from ctypes import wintypes as w

    # The 120-fps source forces a 30-fps lightweight proxy. One blue source frame
    # in each four-frame block makes a missed paused frame step observable in
    # actual displayed pixels, independently of its timecode label.
    source = root / "_frame-navigation.mp4"
    run_process([
        ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        "color=c=red:s=320x180:r=120:d=16,drawbox=color=blue:t=fill:enable='eq(mod(n,4),1)'",
        "-i", root / "source.mp4", "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "0", "-c:a", "copy", source,
    ], timeout=60)
    client.file("ImportVideo", source)
    client.send("FitInWindow")
    original = client.data("GetLinkedMediaClips")
    info, buttons = owned_preview_controls(client)
    assert info["lightweight"], info
    frame, slider, status = (int(info[key]) for key in
                             ("previewFrameHandle", "previewTimelineHandle", "previewStatusHandle"))
    windows = PreviewNativeWindows()
    assert windows.owner(slider) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
    user = windows.api
    class Mouse(ctypes.Structure):
        _fields_ = [("x", w.LONG), ("y", w.LONG), ("data", w.DWORD), ("flags", w.DWORD),
                    ("time", w.DWORD), ("extra", ctypes.c_size_t)]

    class Keyboard(ctypes.Structure):
        _fields_ = [("code", w.WORD), ("scan", w.WORD), ("flags", w.DWORD),
                    ("time", w.DWORD), ("extra", ctypes.c_size_t)]

    class Payload(ctypes.Union):
        _fields_ = [("mouse", Mouse), ("keyboard", Keyboard)]

    class Input(ctypes.Structure):
        _fields_ = [("kind", w.DWORD), ("payload", Payload)]

    user.SendInput.argtypes = [w.UINT, ctypes.POINTER(Input), ctypes.c_int]
    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.GetCursorPos.argtypes = [ctypes.POINTER(w.POINT)]
    user.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
    user.GetForegroundWindow.restype = w.HWND
    user.WindowFromPoint.argtypes, user.WindowFromPoint.restype = [w.POINT], w.HWND
    user.GetAsyncKeyState.argtypes, user.GetAsyncKeyState.restype = [ctypes.c_int], ctypes.c_short
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p

    def message(handle, code, first=0, second=0):
        assert windows.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        reply = ctypes.c_size_t()
        if not user.SendMessageTimeoutW(handle, code, first, second, 2, 10000, ctypes.byref(reply)):
            raise RuntimeError(f"Preview navigation message {code:#x} to HWND={handle} failed: {ctypes.WinError()}")
        return reply.value

    def key(value):
        # wx routes CharHook through the focused window before dispatching a
        # keyboard message. Posting to an unfocused control does not reproduce
        # that path. A real click on the owned status label focuses the preview
        # without changing its timeline or project, then real input uses that path.
        assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]), include_window=frame)
        held = [code for code in (1, 2, 4, 0x10, 0x11, 0x12, 0x5B, 0x5C, value)
                if user.GetAsyncKeyState(code) & 0x8000]
        if held:
            raise RuntimeError(f"Release shared input before preview key input: {held}")
        previous_dpi = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        previous, bounds = w.POINT(), w.RECT()
        user.GetCursorPos(ctypes.byref(previous))

        def send(kind, payload):
            event = Input(kind=kind, payload=payload)
            if user.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
                raise RuntimeError(f"Preview input {value} failed: {ctypes.WinError()}")

        try:
            assert user.GetWindowRect(status, ctypes.byref(bounds))
            point = w.POINT((bounds.left + bounds.right) // 2, (bounds.top + bounds.bottom) // 2)
            assert user.SetCursorPos(point.x, point.y)
            hit = user.WindowFromPoint(point)
            assert hit and windows.owner(hit) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
            send(0, Payload(mouse=Mouse(flags=2)))
            send(0, Payload(mouse=Mouse(flags=4)))
            wait_for_native_state(lambda: user.GetForegroundWindow() == frame, "owned preview focus")
            send(1, Payload(keyboard=Keyboard(code=value, flags=1)))
            send(1, Payload(keyboard=Keyboard(code=value, flags=3)))
            time.sleep(0.1)
        finally:
            send(1, Payload(keyboard=Keyboard(code=value, flags=3)))
            send(0, Payload(mouse=Mouse(flags=4)))
            user.SetCursorPos(previous.x, previous.y)
            if previous_dpi:
                user.SetThreadDpiAwarenessContext(previous_dpi)

    def seek(value, release=True):
        message(slider, 0x405, 1, value)  # TBM_SETPOS, real native slider state
        message(frame, 0x114, 5, slider)  # SB_THUMBTRACK, no seek until release
        if release:
            message(frame, 0x114, 4, slider)  # SB_THUMBPOSITION

    def settled_seconds():
        wait_for_native_state(lambda: math.isfinite(preview_project_seconds(status)),
                              lambda: "preview source ready: " + windows.control_text(status))
        return preview_project_seconds(status)

    for _ in range(2):
        seek(500000, release=False)
        time.sleep(0.08)
        assert abs(preview_project_seconds(status)) < 0.01
        key(0x1B)  # Escape cancels the unpublished drag
        wait_for_native_state(lambda: message(slider, 0x400) == 0, "canceled preview drag")
    for value in (500000, 500000, 250000):
        seek(value)
        expected = 16 * value / 1000000
        wait_for_native_state(lambda expected=expected: abs(preview_project_seconds(status) - expected) < 0.015,
                              f"preview seek to {expected}")
        assert abs(client.data("GetInfo: Type=Selection Format=JSON")["Start"] - expected) < 0.0001

    for direction, virtual_key in ((1, 0x27), (-1, 0x25)):
        before = settled_seconds()
        key(virtual_key)
        wait_for_native_state(lambda before=before, direction=direction: abs(preview_project_seconds(status) - before - direction) < 0.003,
                              "stopped preview one-second key")
    volume = client.data("GetVideoPreviewInfo")["playbackVolume"]
    for _ in range(2):
        key(0x28)
        time.sleep(0.1)
    lower = client.data("GetVideoPreviewInfo")["playbackVolume"]
    assert lower < volume, (volume, lower)
    for _ in range(2):
        key(0x26)
        time.sleep(0.1)
    assert abs(client.data("GetVideoPreviewInfo")["playbackVolume"] - volume) < 0.001

    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: windows.enabled(buttons["pause"]), "preview navigation playback")
    before = settled_seconds()
    key(0x27)
    wait_for_native_state(lambda: preview_project_seconds(status) > before + 0.95, "live one-second preview seek")
    assert windows.enabled(buttons["pause"])
    seek(125000)
    wait_for_native_state(lambda: 1.9 <= preview_project_seconds(status) < 3.5, "live preview timeline seek")
    press_preview_button(buttons["pause"])
    wait_for_native_state(lambda: windows.enabled(buttons["play"]), "preview paused before navigation")
    seek(750000)
    wait_for_native_state(lambda: abs(preview_project_seconds(status) - 12) < 0.015,
                          lambda: "paused preview timeline seek: " + windows.control_text(status))
    time.sleep(0.15)
    assert abs(preview_project_seconds(status) - 12) < 0.015
    # Start at a known frame boundary. A paused live clock can fall between
    # source frames, where a next-frame step is shorter than one full frame.
    for target, virtual_key in ((13, 0x27), (12, 0x25)):
        key(virtual_key)
        wait_for_native_state(lambda target=target: abs(preview_project_seconds(status) - target) < 0.003,
                              lambda: "paused preview one-second key: " + windows.control_text(status))
        assert windows.enabled(buttons["play"]) and windows.enabled(buttons["pause"])
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: preview_project_seconds(status) > 12.15, "resume at the paused destination")
    press_preview_button(buttons["stop"])
    wait_for_native_state(lambda: not windows.enabled(buttons["stop"]), "preview navigation stopped")
    # One-second seeks retain the selected 30-fps proxy, whose frame phase can
    # differ. Explicit frame inspection must still reveal original 120-fps frames.
    client.send("SelectTime: Start=12 End=12")
    for direction, colour in ((1, "blue"), (-1, "red")):
        client.send(f"StepVideoFrame: Direction={direction}")
        expect_preview_crop_pixels(client, root, ffmpeg, {colour})
    assert client.data("GetLinkedMediaClips") == original
    client.file("SaveProject2", root / "_preview-navigation.aup3")
