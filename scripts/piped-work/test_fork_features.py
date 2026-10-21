"""Exercise AuTopsy through its public scripting and media interfaces.

CTest supplies an executable and a dedicated work root. Tests own only marked
temporary directories and their child process. The native Portable Settings
name is required by Audacity; a marker protects any unrelated existing profile.
No installed app, user project, source snapshot or fixed upstream layout is used.
"""
from pathlib import Path
from datetime import datetime
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


def run_process(arguments, **options):
    try:
        result = subprocess.run(arguments, capture_output=True, **options)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"Command timed out: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
                           f"stdout: {error.stdout or '<absent>'}\nstderr: {error.stderr or '<absent>'}") from error
    if result.returncode:
        def display(value):
            if not value: return "<absent>"
            return value.decode(errors="replace") if isinstance(value, bytes) else value
        raise RuntimeError(f"Command: {subprocess.list2cmdline([str(x) for x in arguments])}\n"
                           f"Exit: {result.returncode}\nstdout:\n{display(result.stdout)}\n"
                           f"stderr:\n{display(result.stderr)}")
    return result.stdout


def executable_version(executable):
    """Read the PE resource before creating a first-run app profile."""
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
    version.GetFileVersionInfoW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    version.VerQueryValueW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p,
                                     ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
    size = version.GetFileVersionInfoSizeW(str(executable), None)
    if not size: raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(executable), 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    value = ctypes.c_void_p(); length = ctypes.c_uint32()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(value), ctypes.byref(length)):
        raise ctypes.WinError(ctypes.get_last_error())
    fields = ctypes.cast(value, ctypes.POINTER(ctypes.c_uint32))
    if length.value < 52 or fields[0] != 0xFEEF04BD:
        raise RuntimeError("The executable has no fixed Windows version resource.")
    return fields[2] >> 16, fields[2] & 0xFFFF, fields[3] >> 16


def startup_window_messages(pid):
    """Keep dialogs from the owned child in startup failure diagnostics."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
    messages = []
    @callback_type
    def child(window, _):
        text = ctypes.create_unicode_buffer(2048)
        user.GetWindowTextW(window, text, len(text))
        if text.value: messages.append(text.value)
        return 1
    @callback_type
    def parent(window, _):
        owner = ctypes.c_uint32()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            child(window, 0); user.EnumChildWindows(window, child, 0)
        return 1
    user.EnumWindows(parent, 0)
    return " | ".join(messages) or "<no window text>"


def write_tone(path, channels=1):
    with wave.open(str(path), "wb") as output:
        output.setparams((channels, 2, 48000, 0, "NONE", "not compressed"))
        block = b"".join(struct.pack("<h", int(10000 * math.sin(2 * math.pi * 440 * i / 48000)))
                         * channels for i in range(240000))
        output.writeframes(block)


class ScriptClient:
    def __init__(self):
        import pipeclient
        self.client = pipeclient.PipeClient()

    def send(self, command, success=True):
        self.client.write(command)
        if not self.client.reply_ready.wait(240):
            raise RuntimeError("Scripting timeout: " + command)
        reply = self.client.read()
        print(command + "\n" + reply, flush=True)
        assert ("BatchCommand finished: OK" in reply) == success, reply
        return reply

    def data(self, command):
        return json.loads(self.send(command).split("BatchCommand", 1)[0].strip())

    def file(self, operation, path, extra="", success=True):
        return self.send(f'{operation}: Filename="{path.as_posix()}" {extra}', success)


def save_reopen(client, project, save_existing=False):
    # SaveProject2 is Save As and compares path strings. A reopened Windows
    # path may use different separators; use native Save for its current file.
    if save_existing: client.send("Save")
    else: client.file("SaveProject2", project)
    client.send("Close")
    client.file("OpenProject2", project)


def painted_video_tracks(client):
    """Wait for the native TrackPanel's asynchronous paint, not a proxy window."""
    deadline = time.monotonic() + 5
    while True:
        info = client.data("GetVideoPreviewInfo")
        if info["videoTrackThumbnails"] and all(row["dataWidth"] > 0 for row in info["videoTrackRows"]):
            return info
        if time.monotonic() >= deadline:
            capture_owned_project(int(info["videoTrackPanelHandle"]),
                                  Path(os.environ["AUTOPSY_BEHAVIOR_ROOT"]) / "_unpainted-video.png")
            raise AssertionError(info)
        time.sleep(.1)


def ready_video_preview(client):
    """File loading and project-open callbacks complete asynchronously."""
    deadline = time.monotonic() + 15
    while True:
        info = client.data("GetVideoPreviewInfo")
        if info["ready"]: return info
        if time.monotonic() >= deadline: raise AssertionError(info)
        time.sleep(.1)


def drag_owned_video_clip(client, start_time, end_time, track=None, to_track=None, cancel=False):
    """Exercise the real TrackPanel's clip header; target only the owned PID."""
    info = painted_video_tracks(client)
    if not all(info["viewStart"] <= time <= info["viewEnd"] for time in (start_time, end_time)):
        client.send("FitInWindow")
        info = painted_video_tracks(client)
    panel = int(info["videoTrackPanelHandle"])
    rows = info["videoTrackRows"]
    source = next((row for row in rows if track is None or row["index"] == track), None)
    target = source if to_track is None else next((row for row in rows if row["index"] == to_track), None)
    if source is None or target is None: raise RuntimeError("The requested native video row is missing.")
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    owner = ctypes.c_uint32()
    user.GetWindowThreadProcessId(panel, ctypes.byref(owner))
    if owner.value != int(os.environ["AUTOPSY_OWNED_APP_PID"]):
        raise RuntimeError("The native TrackPanel belongs to another process.")
    def position(time, row):
        x = row["dataLeft"] + round((time-info["viewStart"]) /
             (info["viewEnd"]-info["viewStart"]) * info["videoTrackDataWidth"])
        return x, row.get("dataTop", row["y"])+12
    x0,y0 = position(start_time,source); x1,y1 = position(end_time,target)
    # The scripting progress wrapper can still be disabling the pane when its
    # reply arrives. Inspect that precondition before input. A real held button
    # also prevents native mouse-leave handling from cancelling synthetic motion.
    class Point(ctypes.Structure):
        _fields_ = [("x",ctypes.c_long),("y",ctypes.c_long)]
    class Mouse(ctypes.Structure):
        _fields_ = [("x",ctypes.c_long),("y",ctypes.c_long),("data",ctypes.c_uint32),
                    ("flags",ctypes.c_uint32),("time",ctypes.c_uint32),("extra",ctypes.c_size_t)]
    class Union(ctypes.Union): _fields_ = [("mouse",Mouse)]
    class Input(ctypes.Structure): _fields_ = [("kind",ctypes.c_uint32),("value",Union)]
    class Gui(ctypes.Structure):
        _fields_ = [("size",ctypes.c_uint32),("flags",ctypes.c_uint32),
                    *[(name,ctypes.c_void_p) for name in ("active","focus","capture","menu","move","caret")],
                    ("rect",ctypes.c_long*4)]
    callback = ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_ssize_t)
    user.GetAncestor.argtypes=[ctypes.c_void_p,ctypes.c_uint]; user.GetAncestor.restype=ctypes.c_void_p
    user.GetForegroundWindow.restype=ctypes.c_void_p
    user.SetForegroundWindow.argtypes=[ctypes.c_void_p]
    user.GetParent.argtypes=[ctypes.c_void_p]; user.GetParent.restype=ctypes.c_void_p
    user.IsWindowEnabled.argtypes=[ctypes.c_void_p]
    user.IsWindowVisible.argtypes=[ctypes.c_void_p]
    user.GetWindowLongPtrW.argtypes=[ctypes.c_void_p,ctypes.c_int]; user.GetWindowLongPtrW.restype=ctypes.c_ssize_t
    user.SetWindowPos.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_uint]
    user.GetClientRect.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_long*4)]
    user.GetCursorPos.argtypes=[ctypes.POINTER(Point)]
    user.ClientToScreen.argtypes=[ctypes.c_void_p,ctypes.POINTER(Point)]
    user.WindowFromPoint.argtypes=[Point]; user.WindowFromPoint.restype=ctypes.c_void_p
    user.GetWindowTextW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_int]
    user.EnumWindows.argtypes=[callback,ctypes.c_ssize_t]; user.ShowWindow.argtypes=[ctypes.c_void_p,ctypes.c_int]
    user.GetGUIThreadInfo.argtypes=[ctypes.c_uint32,ctypes.POINTER(Gui)]
    user.SendInput.argtypes=[ctypes.c_uint,ctypes.POINTER(Input),ctypes.c_int]
    user.SendMessageTimeoutW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t,
                                      ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_size_t)]
    user.SetThreadDpiAwarenessContext.argtypes=[ctypes.c_void_p]; user.SetThreadDpiAwarenessContext.restype=ctypes.c_void_p
    user.GetWindowDpiAwarenessContext.argtypes=[ctypes.c_void_p]; user.GetWindowDpiAwarenessContext.restype=ctypes.c_void_p
    previous_dpi=user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    root=user.GetAncestor(panel,2); foreground=user.GetForegroundWindow()
    pinned=bool(user.GetWindowLongPtrW(root,-20)&8)
    original=Point(); user.GetCursorPos(ctypes.byref(original))
    rectangle=(ctypes.c_long*4)(); user.GetClientRect(panel,ctypes.byref(rectangle))
    point=Point(round(x0*rectangle[2]/info["videoTrackPanelWidth"]),
                round(y0*rectangle[3]/info["videoTrackPanelHeight"]))
    user.ClientToScreen(panel,ctypes.byref(point))
    hidden=[]; pressed=False; positioned=False
    @callback
    def hide_preview(window,_):
        pid=ctypes.c_uint32(); user.GetWindowThreadProcessId(window,ctypes.byref(pid))
        if pid.value==owner.value and user.IsWindowVisible(window):
            title=ctypes.create_unicode_buffer(256); user.GetWindowTextW(window,title,len(title))
            if title.value.startswith("Video Preview"):
                hidden.append(window); user.ShowWindow(window,0)
        return 1
    def button(flags):
        packet=Input(0,Union(mouse=Mouse(0,0,0,flags,0,0)))
        if user.SendInput(1,ctypes.byref(packet),ctypes.sizeof(packet))!=1:
            raise RuntimeError("Windows rejected the owned test's mouse-button event.")
    def send(kind,flags,x,y):
        reply=ctypes.c_size_t()
        if not user.SendMessageTimeoutW(panel,kind,flags,((y&65535)<<16)|(x&65535),2,2000,ctypes.byref(reply)):
            raise RuntimeError(f"SendMessageTimeoutW(HWND={panel}, message={kind:#x}, flags={flags}, point={(x,y)}) "
                               f"failed with Windows error {ctypes.get_last_error()}; "
                               + startup_window_messages(owner.value))
    try:
        if any(user.GetAsyncKeyState(key)&0x8000 for key in (1,16,17,18,91,92)):
            raise RuntimeError("Human input is held; the test will not take over its drag meaning.")
        # Consecutive drags at the same header must start as single clicks.
        # A real OS down inside its double-click interval is a different action.
        time.sleep((user.GetDoubleClickTime()+20)/1000)
        user.EnumWindows(hide_preview,0)
        user.SetWindowPos(root,ctypes.c_void_p(-1),0,0,0,0,0x53)
        user.SetForegroundWindow(root)
        deadline=time.monotonic()+3
        while True:
            enabled=True; ancestor=panel
            while ancestor:
                enabled=enabled and bool(user.IsWindowEnabled(ancestor))
                if ancestor==root: break
                ancestor=user.GetParent(ancestor)
            if enabled and user.WindowFromPoint(point)==panel: break
            if time.monotonic()>=deadline: raise RuntimeError("The owned native pane did not become enabled and exposed.")
            time.sleep(.01)
        user.SetCursorPos(point.x,point.y); positioned=True
        if any(user.GetAsyncKeyState(key)&0x8000 for key in (1,16,17,18,91,92)):
            raise RuntimeError("Human input changed before the owned drag began.")
        button(2); pressed=True
        thread=user.GetWindowThreadProcessId(panel,None); deadline=time.monotonic()+1
        while True:
            state=Gui(); state.size=ctypes.sizeof(state)
            if user.GetGUIThreadInfo(thread,ctypes.byref(state)) and state.capture==panel: break
            if time.monotonic()>=deadline: raise RuntimeError("The owned pane did not capture the held mouse input.")
            time.sleep(.01)
        user.SetThreadDpiAwarenessContext(user.GetWindowDpiAwarenessContext(panel))
        for step in range(1,6): send(0x200,1,round(x0+(x1-x0)*step/5),round(y0+(y1-y0)*step/5))
        if cancel:
            for kind in (0x100,0x101): send(kind,0x1b,0,0)
        send(0x202,0,x1,y1)
        button(4); pressed=False
    finally:
        if pressed:
            send(0x100,0x1b,0,0); button(4)
        user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        current=Point(); user.GetCursorPos(ctypes.byref(current))
        if positioned and (current.x,current.y)==(point.x,point.y): user.SetCursorPos(original.x,original.y)
        if not pinned: user.SetWindowPos(root,ctypes.c_void_p(-2),0,0,0,0,0x13)
        for window in hidden: user.ShowWindow(window,4)
        if user.GetForegroundWindow()==root and foreground: user.SetForegroundWindow(foreground)
        if previous_dpi: user.SetThreadDpiAwarenessContext(previous_dpi)

    return panel


def capture_owned_project(panel, path):
    """Capture this test's project window to an artifact, without reading other windows."""
    from PIL import Image
    from ctypes import wintypes
    user, gdi = ctypes.WinDLL("user32"), ctypes.WinDLL("gdi32")
    user.GetAncestor.argtypes = [wintypes.HWND, ctypes.c_uint]; user.GetAncestor.restype = wintypes.HWND
    window = user.GetAncestor(panel, 2)
    owner = wintypes.DWORD()
    user.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if owner.value != int(os.environ["AUTOPSY_OWNED_APP_PID"]): raise RuntimeError("Screenshot target has another owner.")
    rectangle = wintypes.RECT(); user.GetWindowRect(window, ctypes.byref(rectangle))
    width, height = rectangle.right - rectangle.left, rectangle.bottom - rectangle.top
    if not (0 < width < 6000 and 0 < height < 4000): raise RuntimeError("Unexpected project window size.")
    user.GetDC.argtypes = [wintypes.HWND]; user.GetDC.restype = wintypes.HDC
    user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]; gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    gdi.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]; gdi.SelectObject.restype = wintypes.HANDLE
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]; gdi.DeleteDC.argtypes = [wintypes.HDC]
    user.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, ctypes.c_uint]
    screen = user.GetDC(window); memory = gdi.CreateCompatibleDC(screen)
    bitmap = gdi.CreateCompatibleBitmap(screen, width, height); previous = gdi.SelectObject(memory, bitmap)
    try:
        if not user.PrintWindow(window, memory, 2): raise RuntimeError("The owned project could not be captured.")
        gdi.SelectObject(memory, previous)
        class Header(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("width", wintypes.LONG), ("height", wintypes.LONG),
                        ("planes", wintypes.WORD), ("bits", wintypes.WORD), ("compression", wintypes.DWORD),
                        ("image_size", wintypes.DWORD), ("x", wintypes.LONG), ("y", wintypes.LONG),
                        ("used", wintypes.DWORD), ("important", wintypes.DWORD)]
        header = Header(ctypes.sizeof(Header), width, -height, 1, 32)
        pixels = ctypes.create_string_buffer(width * height * 4)
        gdi.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                                 ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
        if gdi.GetDIBits(screen, bitmap, 0, height, pixels, ctypes.byref(header), 0) != height:
            raise RuntimeError("The owned project pixels could not be read.")
        Image.frombuffer("RGB", (width, height), pixels, "raw", "BGRX", 0, 1).save(path)
    finally:
        gdi.SelectObject(memory, previous); gdi.DeleteObject(bitmap); gdi.DeleteDC(memory)
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
        (x["name"], x["start"], x["end"]) for x in original]
    save_reopen(client, root / "tracks.aup3")
    reopened = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [(x["name"], x["channels"]) for x in reopened] == [
        (x["name"], x["channels"]) for x in tracks]


def decoded_frame_hashes(ffmpeg, path):
    data = run_process([ffmpeg, "-v", "error", "-i", str(path), "-map", "0:v:0",
                        "-f", "framemd5", "-"], text=True, timeout=60)
    return [line.rsplit(",", 1)[-1].strip() for line in data.splitlines()
            if line and not line.startswith("#")]


def check_video(client, root, ffmpeg, ffprobe):
    source = root / "source.mp4"
    source_bytes = hashlib.sha256(source.read_bytes()).hexdigest()
    import_children = client.data("GetVideoPreviewInfo")["importMenu"]
    assert import_children[0]["id"] == "ImportVideo", import_children
    video_import = next(row for row in import_children if row["id"] == "ImportVideo")
    assert video_import["label"].rstrip(".") == "Video", import_children
    client.file("ImportVideo", source)
    native_tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [track["kind"] for track in native_tracks] == ["video","wave"], native_tracks
    strip = painted_video_tracks(client)
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    assert strip["videoTrackLeftOffset"] > 0 and strip["viewEnd"] > strip["viewStart"], strip
    client.send("ZoomIn")
    assert client.data("GetVideoPreviewInfo")["viewEnd"] < strip["viewEnd"]
    client.send("ZoomOut")
    original = client.data("GetVideoClips")
    assert len(original["clips"]) == 1 and abs(original["end"] - 4) < .04
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
    assert abs(delta - 1) <= (view["viewEnd"]-view["viewStart"]) / view["videoTrackDataWidth"], (together, moved_tracks)
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
    info = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-show_format",
                                   "-of", "json", str(edited)], text=True, timeout=30))
    assert abs(float(info["format"]["duration"]) - 3) < .08, info
    assert [s["codec_name"] for s in info["streams"]] == ["mpeg4", "aac"], info
    for when, blue in ((.5, False), (1.5, True)):
        pixels = run_process([ffmpeg, "-v", "error", "-ss", str(when), "-i", str(edited),
                              "-frames:v", "1", "-vf", "scale=1:1", "-pix_fmt", "rgb24",
                              "-f", "rawvideo", "-"], timeout=30)
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
    samples = run_process([ffmpeg, "-v", "error", "-i", str(silent), "-map", "0:a:0",
                           "-f", "s16le", "-"], timeout=30)
    assert len(samples) > 48000 and not any(samples)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_bytes
    assert not list(root.glob("_autopsy-video-export-*"))
    client.send("Save")
    check_video_tracks(client, root, ffmpeg, ffprobe)


def check_video_tracks(client, root, ffmpeg, ffprobe):
    """Exercise native track ownership, mixed sources and shared editing actions."""
    other = root / "other-source.mp4"
    client.file("ImportVideo", other)
    assert [track["kind"] for track in client.data("GetInfo: Type=Tracks Format=JSON")] == ["video", "video"]
    layers = root / "layered.mp4"
    client.file("ExportVideoMix", layers)
    streams = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(layers)],
                                     text=True, timeout=30))["streams"]
    assert (streams[0]["width"], streams[0]["height"]) == (90, 160), streams
    for when, blue in ((.5, False), (1.5, True)):
        pixel = run_process([ffmpeg, "-v", "error", "-ss", str(when), "-i", str(layers),
                             "-frames:v", "1", "-vf", "crop=2:2:44:78,scale=1:1", "-pix_fmt", "rgb24",
                             "-f", "rawvideo", "-"], timeout=30)
        assert len(pixel) == 3
        assert (pixel[2] > 200 and pixel[0] < 20) if blue else (pixel[0] > 200 and pixel[1] > 200), pixel
    # Seeking switches the same preview between sources and uses source time,
    # including an edited clip's offset. Await the decoder's asynchronous seek.
    for when, path, expected in ((.5, other, .5), (1.5, root / "source.mp4", 2.5)):
        client.send(f"SelectTime: Start={when} End={when}")
        deadline = time.monotonic() + 5
        while True:
            info = client.data("GetVideoPreviewInfo")
            if info["ready"] and Path(info["playbackPath"]).name == path.name and abs(info["position"]-expected) < .08:
                break
            if time.monotonic() >= deadline: raise AssertionError(info)
            time.sleep(.1)
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
    try: time.sleep(5.5)
    finally: client.send("Stop")
    report = client.data("GetVideoPreviewInfo")
    (root / "_playback.json").write_text(json.dumps(report["samples"], indent=2))
    playing = [sample for sample in report["samples"] if not sample["paused"]]
    assert {Path(sample["source"]).name for sample in playing} == {"source.mp4", "other-source.mp4"}, report
    stable = [sample for sample in playing if 1.3 < sample["audioPosition"] < 2.8 or sample["audioPosition"] > 4.3]
    assert len(stable) >= 10 and max(abs(sample["position"]-sample["expectedPosition"]) for sample in stable) < .2, report
    client.send("SelectTime: Start=0 End=0")
    painted_video_tracks(client)
    drag_owned_video_clip(client, 1.5, 1.5, track=0, to_track=1)
    combined = client.data("GetVideoClips")
    assert len(combined["tracks"]) == 2 and {clip["track"] for clip in combined["clips"]} == {1}, combined
    assert [(clip["start"], Path(clip["source"]).name) for clip in combined["clips"]] == [
        (1, "source.mp4"), (4, "other-source.mp4")], combined
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
    copied = [clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0]
    assert len(copied) == 1 and copied[0]["start"] == 0 and copied[0]["sourceStart"] == 2, copied
    client.send("SelectTime: Start=0 End=2")
    client.send("Cut")
    assert not [clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0]
    client.send("Paste")
    pasted = [clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0]
    assert [{key: value for key, value in clip.items() if key != "id"} for clip in pasted] == [
        {key: value for key, value in clip.items() if key != "id"} for clip in copied], pasted
    client.send("SelectTime: Start=.5 End=1.5")
    client.send("SplitNew")
    split = client.data("GetVideoClips")
    assert len(split["tracks"]) == 3 and len([clip for clip in split["clips"] if clip["track"] == 2]) == 1, split
    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Set")
    client.send("Duplicate")
    assert len(client.data("GetVideoClips")["tracks"]) == 4
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=.1 End=1.9")
    client.send("Trim")
    trimmed = [clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0]
    assert abs(trimmed[0]["start"]-.1) < .001 and abs(trimmed[-1]["end"]-1.9) < .001, trimmed
    unchanged = client.data("GetVideoClips")
    client.send("AddVideoTrackBefore: Track=999", success=False)
    assert client.data("GetVideoClips") == unchanged
    client.send("AddVideoTrackBefore: Track=1")
    client.send("AddVideoTrackAfter: Track=1")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 6 and not [clip for clip in state["clips"] if clip["track"] in (1, 2)], state
    for _ in range(3): client.send("NewVideoTrack")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 9
    # New native rows scroll into view; earlier rows scroll out with audio rows.
    time.sleep(.2)
    rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
    assert rows[0]["y"] < 0 and rows[-1]["y"] >= 0, rows
    save_reopen(client, root / "video.aup3", save_existing=True)
    restored = client.data("GetVideoClips")
    assert restored["clips"] == state["clips"] and len(restored["tracks"]) == 9, restored
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
        time.sleep(.2)
        rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
        assert rows[3]["y"] >= 0 and rows[3]["dataWidth"] > 0, rows
        unchanged = layers.read_bytes()
        for _ in range(2): client.file("ExportVideoMix", layers, "Overwrite=1", success=False)
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
    user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.GetMenu.argtypes = [ctypes.c_void_p]; user.GetMenu.restype = ctypes.c_void_p
    user.GetSubMenu.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetSubMenu.restype = ctypes.c_void_p
    user.GetMenuItemCount.argtypes = [ctypes.c_void_p]
    user.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    user.GetMenuItemID.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetMenuStringW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p,
                                    ctypes.c_int, ctypes.c_uint]
    user.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                  ctypes.c_size_t, ctypes.c_ssize_t]
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
            if len(found) == 1: return found[0]
            if time.monotonic() >= deadline: raise AssertionError(windows())
            time.sleep(.05)

    welcome = wait_title("Welcome to AuTopsy!")
    assert user.PostMessageW(welcome, 0x10, 0, 0), ctypes.WinError(ctypes.get_last_error())
    def entries(menu):
        result = []
        for index in range(user.GetMenuItemCount(menu)):
            text = ctypes.create_unicode_buffer(256)
            user.GetMenuStringW(menu, index, text, len(text), 0x400)
            result.append((text.value.replace("&", "").split("\t")[0],
                           user.GetMenuItemID(menu, index), user.GetSubMenu(menu, index)))
        return result

    # wx also owns hidden menu-bearing windows. Select the editor through its
    # Help menu contract, never the enumeration order, and wait for startup UI.
    deadline = time.monotonic() + 5
    while True:
        menu_windows = {window: entries(user.GetMenu(window))
                        for window in windows() if user.GetMenu(window)}
        candidates = [(window, child) for window, menu in menu_windows.items()
                      for text, _, child in menu if text == "Help"
                      and user.IsWindowEnabled(window)]
        if len(candidates) == 1:
            main, help_menu = candidates[0]
            break
        if time.monotonic() >= deadline:
            raise AssertionError({"windows": windows(), "menus": menu_windows})
        time.sleep(.05)
    help_entries = entries(help_menu)
    labels = [text for text, _, _ in help_entries]
    assert {"AuTopsy Guide", "Audio Manual (Upstream)", "AuTopsy Support", "About AuTopsy"} <= set(labels), labels
    assert not any("Audacity" in text or "Check for Updates" in text for text in labels), labels
    about_id = next(command for text, command, _ in help_entries if text == "About AuTopsy")
    for _ in range(2):
        assert user.PostMessageW(main, 0x111, about_id, 0), ctypes.WinError(ctypes.get_last_error())
        about = wait_title("About AuTopsy")
        assert user.PostMessageW(about, 0x10, 0, 0), ctypes.WinError(ctypes.get_last_error())
        deadline = time.monotonic() + 5
        while any(text == "About AuTopsy" for text in windows().values()):
            if time.monotonic() >= deadline: raise AssertionError(windows())
            time.sleep(.05)
    (root / "_branding.json").write_text(json.dumps({"help": labels,
        "welcome": "AuTopsy", "about_open_close_count": 2}, indent=2), encoding="utf-8")


def worker(args, root):
    client = ScriptClient()
    if args.case == "groups": check_groups(client, root)
    elif args.case == "tracks": check_tracks(client, root)
    elif args.case == "video": check_video(client, root, args.ffmpeg, args.ffprobe)
    elif args.case == "branding": check_branding(client, root)
    else:
        source = args.video_source or root / "source.mp4"
        client.file("ImportVideo", source)
        info = ready_video_preview(client)
        assert info["lightweight"] and info["thumbnails"], info
        assert 0 < info["width"] <= 960 and 0 < info["height"] <= 540, info
        assert client.data("GetInfo: Type=Tracks Format=JSON"), "The original soundtrack was not imported."
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
        samples = [sample for sample in samples if sample["audioPosition"] > samples[0]["audioPosition"] + 1]
        (root / "_playback.json").write_text(json.dumps(samples, indent=2))
        assert all(not sample["paused"] for sample in samples), samples
        assert max(abs(sample["audioPosition"] - sample["position"]) for sample in samples) < .2, samples
        assert samples[-1]["position"] - samples[0]["position"] > 4, samples
        assert samples[-1]["droppedFrames"] - samples[0]["droppedFrames"] <= 2, samples
        assert samples[-1]["decoderDroppedFrames"] - samples[0]["decoderDroppedFrames"] <= 2, samples
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
            if metrics is not None: payload["observations"] = metrics
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
    counts = {}
    retained = 0
    for _, path, scope in sorted(rows, reverse=True):
        counts[scope] = counts.get(scope, 0) + 1
        if counts[scope] > 3 or retained >= 36:
            path.unlink()
        else:
            retained += 1
    for path in archive.glob("_*.tmp"): path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--case", choices=("groups", "tracks", "video", "preview", "branding"), required=True)
    parser.add_argument("--video-source", type=Path, default=os.environ.get("AUTOPSY_PREVIEW_SOURCE"))
    parser.add_argument("--interrupt-cache", choices=("before-publish", "after-publish"),
                        default=os.environ.get("AUTOPSY_INTERRUPT_PREVIEW"))
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--ffprobe", default="")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = args.work_root.resolve() / ("_" + args.case)
    if args.worker:
        worker(args, root)
        return
    if os.name != "nt": raise RuntimeError("The scripting integration cases require Windows.")
    executable = args.executable.resolve(strict=True)
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
    if shared_cache.exists() and (not cache_owner.is_file() or cache_owner.read_text() != cache_signature):
        raise RuntimeError("The preview test cache has another owner: " + str(shared_cache))
    if root.exists():
        if not marker.is_file() or marker.read_text() != signature:
            raise RuntimeError("The test output directory has another owner: " + str(root))
        previous_cache = root / "_preview-cache"
        if args.case == "preview" and previous_cache.is_dir() and not shared_cache.exists():
            shutil.copytree(previous_cache, shared_cache)
            cache_owner.write_text(cache_signature)
        for result_path in root.glob("_result-*.json"):
            observations = root / "_playback.json"
            retain_behavior_result(root.parent, json.loads(result_path.read_text()),
                                   json.loads(observations.read_text()) if observations.is_file() else None)
        shutil.rmtree(root)
    root.mkdir()
    marker.write_text(signature)
    for name, channels in (("tone-a.wav", 1), ("tone-b.wav", 1), ("tone-stereo.wav", 2)):
        write_tone(root / name, channels)
    if args.case == "video":
        run_process([args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                     "color=c=red:s=160x90:r=30:d=2", "-f", "lavfi", "-i",
                     "color=c=blue:s=160x90:r=30:d=2", "-f", "lavfi", "-i",
                     "sine=frequency=440:sample_rate=48000:duration=4", "-filter_complex",
                     "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-map", "2:a",
                     "-c:v", "mpeg4", "-q:v", "2", "-c:a", "aac", "-shortest",
                     str(root / "source.mp4")], timeout=60)
        run_process([args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                     "color=c=yellow:s=90x160:r=25:d=2", "-an", "-c:v", "mpeg4", "-q:v", "2",
                     str(root / "other-source.mp4")], timeout=30)
    if args.case == "preview" and not args.video_source:
        run_process([args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                     "testsrc2=s=1920x1080:r=30:d=16", "-f", "lavfi", "-i",
                     "sine=frequency=440:sample_rate=48000:duration=16", "-c:v", "mpeg4",
                     "-q:v", "5", "-c:a", "aac", "-shortest", str(root / "source.mp4")], timeout=60)
    version_numbers = executable_version(executable)
    profile = executable.parent / "Portable Settings"
    profile_owner = profile / "_autopsy_tests"
    if profile.exists():
        if not profile_owner.is_file() or profile_owner.read_text() != str(args.work_root.resolve()):
            raise RuntimeError("An unrelated Portable Settings profile exists: " + str(profile))
        shutil.rmtree(profile)
    profile.mkdir()
    profile_owner.write_text(str(args.work_root.resolve()))
    if args.case == "preview" and shared_cache.is_dir():
        shutil.copytree(shared_cache, profile / "_video-previews")
    audio = root / "_audio_temp"; audio.mkdir()
    gui = root / "_gui_temp"; gui.mkdir()
    version = ".".join(str(x) for x in version_numbers)
    module = executable.parent / "modules/mod-script-pipe.dll"
    stamp = datetime.fromtimestamp(module.stat().st_mtime).strftime("%Y-%m-%dT%H:%M:%S")
    (profile / "autopsy.cfg").write_text(
        f"[Version]\nMajor={version_numbers[0]}\nMinor={version_numbers[1]}\nMicro={version_numbers[2]}\n"
        f"[Directories]\nTempDir={audio.as_posix()}\n[GUI]\nShowSplashScreen={int(args.case == 'branding')}\n"
        "[FileFormats]\nMusicFileImportSettingChoice=No\n[Module]\nmod-script-pipe=1\n"
        f"[ModuleDateTime]\nmod-script-pipe={stamp}\n", encoding="utf-8")
    environment = dict(os.environ, TEMP=str(gui), TMP=str(gui), PYTHONDONTWRITEBYTECODE="1",
                       AUTOPSY_BEHAVIOR_ROOT=str(args.work_root.resolve()))
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    run_id = uuid.uuid4().hex
    with (root / "_app.log").open("w") as log:
        app = subprocess.Popen([str(executable)], cwd=executable.parent, env=environment,
                               stdout=log, stderr=subprocess.STDOUT)
        try:
            startup = user.WaitForInputIdle(int(app._handle), 30000)
            if startup != 0:
                raise RuntimeError(f"Owned app startup status {startup}; PID {app.pid}; "
                                   + startup_window_messages(app.pid))
            deadline = time.monotonic() + 30
            while not kernel.WaitNamedPipeW(pipe, 100):
                if app.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("The owned test app did not expose its scripting pipe.")
                time.sleep(.05)
            if app.poll() is not None: raise RuntimeError("The owned test app exited during startup.")
            environment["AUTOPSY_OWNED_APP_PID"] = str(app.pid)
            worker_command = [sys.executable, str(Path(__file__)), *sys.argv[1:], "--worker"]
            if args.interrupt_cache:
                cache = profile / "_video-previews"
                baseline = set(cache.rglob("_complete")) if cache.exists() else set()
                operation = subprocess.Popen(worker_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                             text=True, env=environment)
                try:
                    deadline = time.monotonic() + 45
                    observed = None
                    while time.monotonic() < deadline and app.poll() is None and operation.poll() is None:
                        candidates = list(cache.rglob("_building.mp4")) if args.interrupt_cache == "before-publish" else [
                            path for path in cache.rglob("_complete") if path not in baseline]
                        if candidates:
                            observed = candidates[0]; break
                        time.sleep(.005)
                    if observed is None:
                        raise RuntimeError("The preview did not reach the requested interruption boundary.")
                    app.terminate(); app.wait(timeout=20)
                    print("Observed cache interruption: " + args.interrupt_cache)
                    return
                finally:
                    if operation.poll() is None: operation.terminate()
                    output, errors = operation.communicate(timeout=10)
                    (root / "_commands.log").write_text(output, encoding="utf-8")
                    (root / "_errors.log").write_text(errors, encoding="utf-8")
            result = subprocess.run(worker_command,
                                    capture_output=True, text=True, timeout=240, env=environment)
            (root / "_commands.log").write_text(result.stdout, encoding="utf-8")
            (root / "_errors.log").write_text(result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"Command: {subprocess.list2cmdline(worker_command)}\n"
                                   f"Behavior worker exit {result.returncode}\n"
                                   f"stdout:\n{result.stdout[-14000:] or '<absent>'}\n"
                                   f"stderr:\n{result.stderr or '<absent>'}")
            try:
                status = app.wait(timeout=20)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError("Owned app did not exit; " + startup_window_messages(app.pid)) from error
            if status: raise RuntimeError(f"The owned app exited with status {status}.")
            source = args.video_source.resolve(strict=True) if args.video_source else root / "source.mp4"
            source_identity = None
            if source.is_file():
                info = source.stat()
                source_identity = {"source": str(source) if args.video_source else "generated:" + args.case,
                                   "bytes": info.st_size, "modified_ns": info.st_mtime_ns if args.video_source else None}
            record = {"run": run_id, "case": args.case, "result": "passed", "version": version.strip(),
                      "input": source_identity,
                      "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                      "check_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      "pipe_client_sha256": hashlib.sha256(Path(__file__).with_name("pipeclient.py").read_bytes()).hexdigest()}
            (root / ("_result-" + run_id + ".json")).write_text(json.dumps(record, indent=2), encoding="utf-8")
            observations = root / "_playback.json"
            retain_behavior_result(root.parent, record,
                                   json.loads(observations.read_text()) if observations.is_file() else None)
            print(args.case + ": PASS (native command, project and media checks)")
        finally:
            if app.poll() is None: app.terminate(); app.wait(timeout=20)
            if profile_owner.is_file() and profile_owner.read_text() == str(args.work_root.resolve()):
                cache = profile / "_video-previews"
                if args.case == "preview" and cache.is_dir():
                    if shared_cache.exists(): shutil.rmtree(shared_cache)
                    shutil.copytree(cache, shared_cache)
                    cache_owner.write_text(cache_signature)
                shutil.rmtree(profile)


if __name__ == "__main__":
    main()
