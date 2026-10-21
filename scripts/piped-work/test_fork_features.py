"""Exercise AuTopsy through its public scripting and media interfaces.

CTest supplies an executable and a dedicated work root. Tests own only marked
temporary directories and their child process. The native Portable Settings
name is required by Audacity; a marker protects any unrelated existing profile.
No installed app, user project, source snapshot or upstream UI geometry is used.
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


def save_reopen(client, project):
    client.file("SaveProject2", project)
    client.send("Close")
    client.file("OpenProject2", project)


def drag_owned_video_clip(client, start_time, end_time):
    """Send drag events only to this runner's video panel; never move the mouse."""
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
    user.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    panels = []
    @callback_type
    def child(window, _):
        text = ctypes.create_unicode_buffer(256)
        user.GetWindowTextW(window, text, len(text))
        if text.value == "Video Track": panels.append(window)
        return 1
    @callback_type
    def parent(window, _):
        owner = ctypes.c_uint32()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid: user.EnumChildWindows(window, child, 0)
        return 1
    user.EnumWindows(parent, 0)
    if len(panels) != 1: raise RuntimeError("The owned main-window video panel is missing or ambiguous.")
    info = client.data("GetVideoPreviewInfo")
    def point(time):
        x = info["videoTrackLeftOffset"] + round((time - info["viewStart"]) /
             (info["viewEnd"] - info["viewStart"]) * info["videoTrackDataWidth"])
        return (50 << 16) | x
    user.SendMessageTimeoutW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t,
                                        ctypes.c_ssize_t, ctypes.c_uint, ctypes.c_uint,
                                        ctypes.POINTER(ctypes.c_size_t)]
    user.SendMessageTimeoutW.restype = ctypes.c_size_t
    for message, buttons, position in ((0x201, 1, point(start_time)),
                                      (0x200, 1, point(end_time)), (0x202, 0, point(end_time))):
        reply = ctypes.c_size_t()
        if not user.SendMessageTimeoutW(panels[0], message, buttons, position, 2, 2000, ctypes.byref(reply)):
            raise RuntimeError("The owned video track did not process its drag event.")
    return panels[0]


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
    client.file("ImportVideoReference", source)
    strip = client.data("GetVideoPreviewInfo")
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    assert strip["videoTrackLeftOffset"] > 0 and strip["viewEnd"] > strip["viewStart"], strip
    client.send("ZoomIn")
    assert client.data("GetVideoPreviewInfo")["viewEnd"] < strip["viewEnd"]
    client.send("ZoomOut")
    original = client.data("GetVideoClips")
    assert len(original["clips"]) == 1 and abs(original["end"] - 4) < .04
    client.send("SplitVideo: At=2")
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
    client.send("DeleteVideoClip: Id=1")
    client.send("DeleteVideoClip: Id=1")
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
    save_reopen(client, root / "video.aup3")
    assert client.data("GetVideoClips") == moved
    strip = client.data("GetVideoPreviewInfo")
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    client.send("SelectAll")
    client.send("RemoveTracks")
    assert client.data("GetInfo: Type=Tracks Format=JSON") == []
    assert client.data("GetVideoClips") == moved
    silent = root / "silent.mkv"
    client.file("ExportVideoMix", silent)
    samples = run_process([ffmpeg, "-v", "error", "-i", str(silent), "-map", "0:a:0",
                           "-f", "s16le", "-"], timeout=30)
    assert len(samples) > 48000 and not any(samples)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_bytes
    assert not list(root.glob("_autopsy-video-export-*"))
    client.send("Save")


def worker(args, root):
    client = ScriptClient()
    if args.case == "groups": check_groups(client, root)
    elif args.case == "tracks": check_tracks(client, root)
    elif args.case == "video": check_video(client, root, args.ffmpeg, args.ffprobe)
    else:
        source = args.video_source or root / "source.mp4"
        client.file("ImportVideoReference", source)
        deadline = time.monotonic() + 15
        while True:
            info = client.data("GetVideoPreviewInfo")
            if info["ready"]: break
            if time.monotonic() > deadline: raise RuntimeError("Prepared preview did not become ready.")
            time.sleep(.1)
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
        assert client.data("GetVideoPreviewInfo")["lightweight"]
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
    parser.add_argument("--case", choices=("groups", "tracks", "video", "preview"), required=True)
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
        f"[Directories]\nTempDir={audio.as_posix()}\n[GUI]\nShowSplashScreen=0\n"
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
