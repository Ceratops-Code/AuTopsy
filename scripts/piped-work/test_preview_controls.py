"""Focused preview-quality, native control and exact video-cursor cases.

Only the owned process on the secondary display receives input. Generated
media and the existing cache owner supply fixtures; no personal files are read.
"""
import ctypes
import json
import os
import time

from run_fork_test_harness import (
    assert_owned_test_display, operate_owned_video_track, painted_video_tracks,
    ready_video_preview, run_process,
)
from test_video_clip_edits import (
    PreviewNativeWindows, owned_preview_controls, press_preview_button,
    preview_project_seconds, wait_for_native_state,
)
from test_clip_actions import with_native_dialog


class PreviewInput:
    """Native messages and focused keys, scoped to the owned preview HWND."""

    def __init__(self, info):
        from ctypes import wintypes as w
        self.windows = PreviewNativeWindows()
        self.api = self.windows.api
        self.frame = int(info["previewFrameHandle"])
        self.status = int(info["previewStatusHandle"])
        self.pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
        assert self.windows.owner(self.frame) == self.pid
        assert_owned_test_display(self.pid, include_window=self.frame)
        self.api.GetDlgCtrlID.argtypes, self.api.GetDlgCtrlID.restype = [w.HWND], ctypes.c_int
        self.api.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        self.api.SetForegroundWindow.argtypes = [w.HWND]
        self.api.GetForegroundWindow.restype = w.HWND
        self.api.GetAsyncKeyState.argtypes, self.api.GetAsyncKeyState.restype = [ctypes.c_int], ctypes.c_short
        self.api.keybd_event.argtypes = [w.BYTE, w.BYTE, w.DWORD, ctypes.c_size_t]

    def message(self, handle, code, first=0, second=0):
        assert self.windows.owner(handle) == self.pid
        result = ctypes.c_size_t()
        if not self.api.SendMessageTimeoutW(handle, code, first, second, 2, 60000, ctypes.byref(result)):
            raise RuntimeError(f"Owned preview message {code:#x} to {handle} failed: {ctypes.WinError()}")
        return result.value

    def key(self, code):
        # Physical key input traverses wx's focused CharHook path. Refuse held
        # shared input and release this key even when an observation fails.
        assert_owned_test_display(self.pid, include_window=self.frame)
        held = [value for value in (1, 2, 4, 0x10, 0x11, 0x12, 0x5B, 0x5C, code)
                if self.api.GetAsyncKeyState(value) & 0x8000]
        if held:
            raise RuntimeError(f"Release shared input before preview key input: {held}")
        self.api.SetForegroundWindow(self.frame)
        wait_for_native_state(lambda: self.api.GetForegroundWindow() == self.frame, "owned preview focus")
        try:
            self.api.keybd_event(code, 0, 1, 0)
        finally:
            self.api.keybd_event(code, 0, 3, 0)
        time.sleep(0.12)

    def quality(self, handle, index):
        self.message(handle, 0x14E, index)  # CB_SETCURSEL
        # CBN_SELCHANGE is the native wxChoice command, not a scripting bypass.
        self.message(self.frame, 0x111, self.api.GetDlgCtrlID(handle) | (1 << 16), handle)

    def volume(self, handle, percent):
        self.message(handle, 0x405, 1, percent)  # TBM_SETPOS
        self.message(self.frame, 0x114, 4, handle)  # SB_THUMBPOSITION

    def cancel_click(self, handle):
        self.message(handle, 0x201, 1, 12 | (12 << 16))
        self.message(handle, 0x202, 0, -1)


def check_preview_quality(client, root, ffmpeg, ffprobe):
    """Every profile opens its actual decode dimensions; repeats reuse its cache."""
    source = root / "_quality-source.mp4"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                 "color=c=blue:s=1920x1080:r=60:d=4", "-f", "lavfi", "-i",
                 "sine=frequency=440:sample_rate=48000:duration=4", "-c:v", "libx264",
                 "-preset", "ultrafast", "-crf", "30", "-c:a", "aac", source], timeout=60)
    client.file("ImportVideo", source)
    client.send("SelectTime: Start=1.25 End=1.25")
    original = client.data("GetVideoClips")
    audio = client.data("GetInfo: Type=Clips Format=JSON")
    info = ready_video_preview(client)
    native = PreviewInput(info)
    choice = int(info["previewQualityHandle"])
    with_native_dialog(lambda: native.quality(choice, 3), "Prepare Video Preview",
                       lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")))
    canceled = ready_video_preview(client)
    assert canceled["previewQuality"] == 0 and canceled["playbackPath"] == info["playbackPath"], canceled
    assert abs(canceled["audioPosition"] - 1.25) < 0.002
    seen = {}
    for index, width, height in ((0, 960, 540), (1, 640, 360), (2, 1280, 720),
                                 (3, 1920, 1080), (4, 1920, 1080), (1, 640, 360), (1, 640, 360)):
        native.quality(choice, index)
        info = ready_video_preview(client)
        assert (info["width"], info["height"]) == (width, height), info
        assert info["previewQuality"] == index, info
        assert abs(info["audioPosition"] - 1.25) < 0.002, info
        assert info["originalFrames"] == (index == 4), info
        path = str(source) if index == 4 else info["playbackPath"]
        stream = json.loads(run_process([ffprobe, "-v", "error", "-select_streams", "v:0",
                                        "-show_entries", "stream=width,height,r_frame_rate", "-of", "json", path]))["streams"][0]
        assert (stream["width"], stream["height"]) == (width, height), stream
        assert stream["r_frame_rate"] == ("60/1" if index == 4 else "30/1"), stream
        if index in seen:
            assert path == seen[index], "Repeated quality produced a different cache identity."
        seen[index] = path
    _, buttons = owned_preview_controls(client)
    status = int(info["previewStatusHandle"])
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: preview_project_seconds(status) > 1.4, "quality-test playback")
    native.quality(choice, 2)
    before = preview_project_seconds(status)
    wait_for_native_state(lambda: preview_project_seconds(status) > before + 0.15, "quality change retains playback")
    press_preview_button(buttons["pause"])
    wait_for_native_state(lambda: native.windows.enabled(buttons["play"]), "quality-test pause")
    time.sleep(0.08)
    paused = preview_project_seconds(status)
    native.quality(choice, 4)
    wait_for_native_state(lambda: abs(preview_project_seconds(status) - paused) < 0.02, "quality change retains pause")
    time.sleep(0.15)
    assert abs(preview_project_seconds(status) - paused) < 0.02
    press_preview_button(buttons["stop"])
    assert client.data("GetVideoClips") == original
    assert client.data("GetInfo: Type=Clips Format=JSON") == audio
    client.file("SaveProject2", root / "_preview-quality.aup3")


def check_preview_controls(client, root):
    """Native icons, exact seconds, shared mixer and selected-section looping."""
    from ctypes import wintypes as w
    client.file("ImportVideo", root / "source.mp4")
    client.send("SelectTime: Start=2 End=2")
    info, buttons = owned_preview_controls(client)
    native = PreviewInput(info)
    native.api.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    for handle in buttons.values():
        bounds = w.RECT()
        assert native.api.GetClientRect(handle, ctypes.byref(bounds))
        assert bounds.right == bounds.bottom == 52, bounds
    status = int(info["previewStatusHandle"])
    original_audio = client.data("GetInfo: Type=Clips Format=JSON")
    volume = info["playbackVolume"]
    capture = info["captureVolume"]
    native.cancel_click(buttons["play"])
    time.sleep(0.12)
    assert not native.windows.enabled(buttons["stop"]), "An outside release started playback."
    for _ in range(2):
        for key, expected in ((0x27, 3), (0x25, 2)):
            native.key(key)
            wait_for_native_state(lambda expected=expected: abs(preview_project_seconds(status) - expected) < 0.02,
                                  f"one-second key destination {expected}")
        for handle, expected in ((info["previewForwardHandle"], 3), (info["previewBackHandle"], 2)):
            press_preview_button(int(handle))
            wait_for_native_state(lambda expected=expected: abs(preview_project_seconds(status) - expected) < 0.02,
                                  f"one-second button destination {expected}")
    native.volume(int(info["previewVolumeHandle"]), 40)
    adjusted = client.data("GetVideoPreviewInfo")
    assert abs(adjusted["playbackVolume"] - 0.4) < 0.001, adjusted
    for code in (0x28, 0x26):
        before = client.data("GetVideoPreviewInfo")["playbackVolume"]
        native.key(code)
        after = client.data("GetVideoPreviewInfo")["playbackVolume"]
        assert (after < before) if code == 0x28 else (after > before), (before, after)
    client.send("OutputGainDec")
    time.sleep(0.12)
    changed = client.data("GetVideoPreviewInfo")
    assert native.message(int(info["previewVolumeHandle"]), 0x400) == round(changed["playbackVolume"] * 100)
    assert changed["captureVolume"] == capture
    native.volume(int(info["previewVolumeHandle"]), round(volume * 100))
    # Seek while paused retains pause, and Pause itself resumes like the main toolbar.
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: preview_project_seconds(status) > 2.15, "preview playback")
    press_preview_button(buttons["pause"])
    wait_for_native_state(lambda: native.windows.enabled(buttons["play"]), "preview pause")
    time.sleep(0.12)
    paused = preview_project_seconds(status)
    native.key(0x27)
    wait_for_native_state(lambda: abs(preview_project_seconds(status) - paused - 1) < 0.02, "paused one-second seek")
    time.sleep(0.15)
    assert abs(preview_project_seconds(status) - paused - 1) < 0.02
    press_preview_button(buttons["pause"])
    wait_for_native_state(lambda: preview_project_seconds(status) > paused + 1.15, "pause button resumes")
    before = preview_project_seconds(status)
    press_preview_button(int(info["previewForwardHandle"]))
    wait_for_native_state(lambda: preview_project_seconds(status) > before + 0.95, "playing one-second button seek")
    press_preview_button(buttons["stop"])
    wait_for_native_state(lambda: not native.windows.enabled(buttons["stop"]), "preview stop")
    for _ in range(2):
        client.send("SelectTime: Start=0.4 End=0.8")
        time.sleep(0.08)
        press_preview_button(int(info["previewLoopHandle"]))
        loop = client.data("GetVideoPreviewInfo")
        assert loop["loopActive"] and abs(loop["loopStart"] - 0.4) < 0.001 and abs(loop["loopEnd"] - 0.8) < 0.001, loop
        press_preview_button(buttons["play"])
        time.sleep(1.1)
        assert 0.38 <= preview_project_seconds(status) <= 0.85, preview_project_seconds(status)
        press_preview_button(buttons["stop"])
        press_preview_button(int(info["previewLoopHandle"]))
        assert not client.data("GetVideoPreviewInfo")["loopActive"]
    assert client.data("GetInfo: Type=Clips Format=JSON") == original_audio
    client.file("SaveProject2", root / "_preview-controls.aup3")


def check_video_cursor(client, root):
    """Single clicks retain exact pressed time after ranges and viewport changes."""
    client.file("ImportVideo", root / "source.mp4")
    client.send("FitInWindow")
    for start, end in ((0, 0), (1, 3)):
        client.send(f"SelectTime: Start={start} End={end}")
        if end > start:
            client.send("ZoomSel")
        for _ in range(2):
            for area in ("header", "picture"):
                for position in (1.35, 2.6):
                    painted = painted_video_tracks(client)
                    operate_owned_video_track(client, position, track=0, gesture="click", area=area, painted=painted)
                    selection = client.data("GetInfo: Type=Selection Format=JSON")
                    row = painted["videoTrackRows"][0]
                    tolerance = (painted["viewEnd"] - painted["viewStart"]) / row["dataWidth"] * 1.5
                    assert abs(selection["Start"] - position) <= tolerance and selection["Start"] == selection["End"], (area, position, selection)
                    client.send("SelectTime: Start=0 End=4")
    client.file("SaveProject2", root / "_video-cursor.aup3")
