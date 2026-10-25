"""Focused preview-quality, native control and exact video-cursor cases.

Only the owned process on the secondary display receives input. Generated
media and the existing cache owner supply fixtures; no personal files are read.
"""
import ctypes
import json
import os
import shutil
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
        self.api.mouse_event.argtypes = [w.DWORD, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
        self.api.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
        self.api.GetCursorPos.argtypes = [ctypes.POINTER(w.POINT)]
        self.api.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
        self.api.WindowFromPoint.argtypes, self.api.WindowFromPoint.restype = [w.POINT], w.HWND
        self.api.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        self.api.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p

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
        from ctypes import wintypes as w
        previous, bounds = w.POINT(), w.RECT()
        dpi = self.api.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        self.api.GetCursorPos(ctypes.byref(previous))
        try:
            assert self.api.GetWindowRect(self.status, ctypes.byref(bounds))
            point = w.POINT((bounds.left + bounds.right) // 2, (bounds.top + bounds.bottom) // 2)
            assert self.api.SetCursorPos(point.x, point.y)
            assert self.windows.owner(self.api.WindowFromPoint(point)) == self.pid
            self.api.mouse_event(2, 0, 0, 0, 0)
            self.api.mouse_event(4, 0, 0, 0, 0)
            wait_for_native_state(lambda: self.api.GetForegroundWindow() == self.frame, "owned preview focus")
            self.api.keybd_event(code, 0, 1, 0)
        finally:
            self.api.keybd_event(code, 0, 3, 0)
            self.api.mouse_event(4, 0, 0, 0, 0)
            self.api.SetCursorPos(previous.x, previous.y)
            if dpi:
                self.api.SetThreadDpiAwarenessContext(dpi)
        time.sleep(0.12)

    def quality(self, handle, index):
        self.message(handle, 0x14E, index)  # CB_SETCURSEL
        # wxChoice commits on CBN_SELENDOK. SELCHANGE only records a tentative
        # highlighted item; it deliberately does not dispatch wxEVT_CHOICE.
        self.message(self.frame, 0x111, (self.api.GetDlgCtrlID(handle) & 0xFFFF) | (9 << 16), handle)

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
                       lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")), needs_focus=False)
    canceled = ready_video_preview(client)
    assert canceled["previewQuality"] == 0 and canceled["playbackPath"] == info["playbackPath"], canceled
    assert abs(canceled["audioPosition"] - 1.25) < 0.002
    seen: dict[int, str] = {}
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
    client.send("New")
    for _ in range(2):
        client.file("ImportVideo", source)
        assert ready_video_preview(client)["previewQualitySources"] == 1, "Repeated imports retained duplicate source leases."
    native = PreviewInput(ready_video_preview(client))
    native.quality(int(client.data("GetVideoPreviewInfo")["previewQualityHandle"]), 1)
    other = root / "_quality-import.mp4"
    shutil.copyfile(source, other)
    for _ in range(2):
        client.file("ImportVideo", other)
        client.send("FitV")
        imported = ready_video_preview(client)
        assert imported["previewQuality"] == 1 and imported["previewQualitySources"] == 2, imported
        clips = client.data("GetVideoClips")["clips"]
        index = max(row["track"] for row in clips if row["source"] == other.as_posix())
        client.send("LastTrack")
        deadline = time.monotonic() + 5
        while True:
            painted = client.data("GetVideoPreviewInfo")
            row = next(row for row in painted["videoTrackRows"] if row["index"] == index)
            if row["dataWidth"] > 0 and row["thumbnails"]:
                break
            assert time.monotonic() < deadline, ("Custom-quality import has no filmstrip", row)
            time.sleep(0.1)
    client.send("SelectAll")
    client.send("RemoveTracks")
    deadline = time.monotonic() + 5
    while client.data("GetVideoPreviewInfo")["previewQualitySources"]:
        assert time.monotonic() < deadline, "Deleted source clips retained their quality leases."
        time.sleep(0.12)
    client.file("Import2", root / "tone-a.wav")
    client.file("SaveProject2", root / "_preview-quality-cleanup.aup3")


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
    client.send("SelectTime: Start=0.4 End=2.8")
    time.sleep(0.08)
    press_preview_button(int(info["previewLoopHandle"]))
    loop = client.data("GetVideoPreviewInfo")
    assert loop["loopActive"] and abs(loop["loopStart"] - 0.4) < 0.001 and abs(loop["loopEnd"] - 2.8) < 0.001, loop
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: preview_project_seconds(status) > 0.55, "loop before a seek")
    press_preview_button(int(info["previewForwardHandle"]))
    wait_for_native_state(lambda: preview_project_seconds(status) > 1.4, "one-second seek within a loop")
    # Observe a real wrap to the original loop's left edge, not merely a clock
    # somewhere inside the region. A restart accidentally bounded by the seek
    # destination would loop from that destination and fail this observation.
    wait_for_native_state(lambda: preview_project_seconds(status) > 2.5, "approaching the retained loop endpoint")
    wait_for_native_state(lambda: 0.38 <= preview_project_seconds(status) < 1.2, "wraps to the original loop start")
    loop_observation = {"playing": native.windows.enabled(buttons["stop"]),
                        "position": preview_project_seconds(status)}
    if not (loop_observation["playing"] and 0.38 <= loop_observation["position"] < 2.85):
        loop_observation["project"] = client.data("GetVideoPreviewInfo")
        raise AssertionError(loop_observation)
    press_preview_button(buttons["stop"])
    press_preview_button(int(info["previewLoopHandle"]))
    assert client.data("GetInfo: Type=Clips Format=JSON") == original_audio
    client.file("SaveProject2", root / "_preview-controls.aup3")


def preview_soak_seconds():
    """Keep a requested soak finite and bind its length to the native receipt."""
    import math
    import os

    seconds = float(os.environ.get("AUTOPSY_PREVIEW_SOAK_SECONDS", "180"))
    if not math.isfinite(seconds) or not 60 <= seconds <= 240:
        raise ValueError("Preview soak duration must be between 60 and 240 seconds.")
    return seconds


def read_owned_process_resources(pid):
    """Read private bytes and handles from the test PID; always close our handle."""
    from ctypes import wintypes as w

    class MemoryCounters(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD), *[
            (name, ctypes.c_size_t) for name in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory_api = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.GetProcessHandleCount.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
    memory_api.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(MemoryCounters), w.DWORD]
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        memory, handles = MemoryCounters(), w.DWORD()
        memory.cb = ctypes.sizeof(memory)
        if not memory_api.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.GetProcessHandleCount(handle, ctypes.byref(handles)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {"private_bytes": int(memory.PrivateUsage), "handles": int(handles.value)}
    finally:
        kernel.CloseHandle(handle)


def check_preview_soak(client, root):
    """Measure sustained proxy playback, loop sync, response time and resource growth.

    This is a machine-scoped smoke bound, not a promise about every long project.
    Keep detailed numbers in the native result so slow or unsupported hardware
    cannot acquire an unmeasured performance claim from a portable model pass.
    """
    import json
    import os

    seconds = preview_soak_seconds()
    client.file("ImportVideo", root / "source.mp4")
    client.send("SelectAll")
    client.send("Copy")
    for index in range(1, 32):
        at = index * 20
        client.send(f"SelectTime: Start={at} End={at}")
        client.send("Paste")
    clips = client.data("GetVideoClips")["clips"]
    assert len(clips) == 32, clips
    last = max(clips, key=lambda clip: clip["start"])
    origin = 6 * 60 * 60
    client.send(f'SetClipProperties: Track={last["track"]} At={last["start"] + 0.5} Start={origin}')
    client.send(f"SelectTime: Start={origin + 1} End={origin + 15}")
    info, buttons = owned_preview_controls(client)
    audio = client.data("GetInfo: Type=Clips Format=JSON")
    assert len(audio) == 32 and any(abs(clip["start"] - origin) < 0.001 for clip in audio), audio
    video = [(clip["id"], clip["start"], clip["end"]) for clip in client.data("GetVideoClips")["clips"]]
    status = int(info["previewStatusHandle"])
    windows = PreviewNativeWindows()
    press_preview_button(int(info["previewLoopHandle"]))
    press_preview_button(buttons["play"])
    wait_for_native_state(lambda: preview_project_seconds(status) > origin + 1.2,
                          "soak audio clock starts")
    time.sleep(3)  # Exclude first decoder/renderer allocation from growth limits.
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    baseline = read_owned_process_resources(pid)
    started, samples = time.monotonic(), []
    previous, wraps = preview_project_seconds(status), 0
    try:
        while time.monotonic() - started < seconds:
            call_started = time.monotonic()
            # Configured scripting commands stop audio before executing. Read
            # the real status HWND while live, then inspect the application's
            # bounded timing samples after stopping through its normal button.
            current = preview_project_seconds(status)
            response = time.monotonic() - call_started
            assert windows.enabled(buttons["pause"]) and windows.visible(int(info["previewSurfaceHandle"]))
            assert origin + 1 <= current <= origin + 15, current
            assert response < 2, f"Preview command stalled for {response:.3f}s."
            if current < previous - 1:
                wraps += 1
            samples.append({"elapsed": time.monotonic() - started,
                            "audio": current - origin,
                            "response": response, **read_owned_process_resources(pid)})
            previous = current
            time.sleep(0.25)
    finally:
        press_preview_button(buttons["stop"])
        loop = client.data("GetVideoPreviewInfo")
        if loop["loopActive"]:
            press_preview_button(int(info["previewLoopHandle"]))
    latency = sorted(row["response"] for row in samples)
    recent = loop["samples"]
    drift = sorted(abs(row["expectedPosition"] - row["position"]) for row in recent
                   if not row["paused"] and 1.5 < row["audioPosition"] - origin < 14.5)
    drops = max(0, loop["droppedFrames"] - info["droppedFrames"])
    decoder_drops = max(0, loop["decoderDroppedFrames"] - info["decoderDroppedFrames"])
    assert drift and wraps >= int(seconds / 14) - 1, "Playback did not sustain the selected loop."
    observed = {
        "seconds": time.monotonic() - started, "samples": len(samples), "wraps": wraps,
        "video_clips": len(clips), "timeline_origin_seconds": origin,
        "timing_sample_count": len(recent), "timing_scope": "last bounded playback sample window",
        "response_p95_seconds": latency[int((len(latency) - 1) * 0.95)],
        "drift_p95_seconds": drift[int((len(drift) - 1) * 0.95)],
        "dropped_frames": drops, "decoder_dropped_frames": decoder_drops,
        "private_growth_bytes": max(row["private_bytes"] for row in samples) - baseline["private_bytes"],
        "handle_growth": max(row["handles"] for row in samples) - baseline["handles"],
    }
    (root / "_playback.json").write_text(json.dumps(observed, indent=2), encoding="utf-8")
    assert observed["response_p95_seconds"] < 0.5, observed
    assert observed["drift_p95_seconds"] < 0.15, observed
    assert drops + decoder_drops <= seconds * 30 * 0.02 + 3, observed
    assert observed["private_growth_bytes"] <= 128 * 1024 * 1024, observed
    assert observed["handle_growth"] <= 64, observed
    assert client.data("GetInfo: Type=Clips Format=JSON") == audio
    assert [(clip["id"], clip["start"], clip["end"]) for clip in client.data("GetVideoClips")["clips"]] == video
    for _ in range(2):
        press_preview_button(buttons["play"])
        wait_for_native_state(lambda: windows.enabled(buttons["pause"]), "restart after soak")
        press_preview_button(buttons["stop"])
    client.file("SaveProject2", root / "_soak.aup3")


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
