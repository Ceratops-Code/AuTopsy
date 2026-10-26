"""Direct capture through the public command, using only generated/owned media.

The harness owns the marked profile, secondary-display process and cleanup.
Camera/microphone hardware is never opened: invalid-device cases fail before
capture, and the screen case records only the private editor's own HWND.
"""
import ctypes
import hashlib
import json
import os
import time
from ctypes import wintypes as w

from run_fork_test_harness import run_process, save_reopen
from test_beginner_edits import model, video_rows
from test_clip_actions import ClipDialogWindows, with_native_dialog
from test_video_transport import wait_for_native_state

VIDEO_RECORDING_CASES = ("video-recording", "video-recording-stop", "video-recording-failures", "video-recording-controls")


def check_video_recording_controls(client):
    """Use the actual Transport menu; changing choices then Cancel has no effect."""
    windows = ClipDialogWindows()
    api = windows.api
    api.GetMenu.argtypes, api.GetMenu.restype = [w.HWND], w.HMENU
    api.GetSubMenu.argtypes, api.GetSubMenu.restype = [w.HMENU, ctypes.c_int], w.HMENU
    api.GetMenuItemCount.argtypes, api.GetMenuItemCount.restype = [w.HMENU], ctypes.c_int
    api.GetMenuItemID.argtypes, api.GetMenuItemID.restype = [w.HMENU, ctypes.c_int], w.UINT
    api.GetMenuStringW.argtypes = [w.HMENU, w.UINT, w.LPWSTR, ctypes.c_int, w.UINT]
    frame = api.GetAncestor(int(client.data("GetVideoPreviewInfo")["videoTrackPanelHandle"]), 2)
    before = model(client)
    for _ in range(2):
        menu = api.GetMenu(frame)
        for label in ("Transport", "Recording", "Record Video..."):
            entries = []
            for index in range(api.GetMenuItemCount(menu)):
                caption = ctypes.create_unicode_buffer(512)
                api.GetMenuStringW(menu, index, caption, len(caption), 0x400)
                if caption.value.replace("&", "").split("\t")[0] == label:
                    entries.append(index)
            assert len(entries) == 1, (label, entries)
            index = entries[0]
            if label != "Record Video...":
                menu = api.GetSubMenu(menu, index)
        command = api.GetMenuItemID(menu, index)
        assert command != 0xFFFFFFFF
        assert api.PostMessageW(frame, 0x111, command, 0)
        dialog = None
        deadline = time.monotonic()+15
        while not dialog:
            for handle in windows.windows():
                if windows.visible(handle) and windows.caption(handle) == "Record Video" and any(
                        windows.caption(child) == "Source" for child in windows.windows(handle)):
                    dialog = handle
                    break
            assert time.monotonic() < deadline, "Record Video options did not appear."
            if not dialog:
                time.sleep(0.025)
        try:
            source = windows.field(dialog, "Source", "ComboBox")
            camera = windows.field(dialog, "Camera", "ComboBox")
            display = windows.field(dialog, "Display", "ComboBox")
            quality = windows.field(dialog, "Maximum picture size", "ComboBox")
            assert windows.message(source, 0x146) == 3  # CB_GETCOUNT; 0x147 reads the selection
            assert windows.message(quality, 0x146) == 3
            windows.button(dialog, "Choose File and Record...")
            for selection in (1, 2, 0, 1):
                windows.message(source, 0x14E, selection)  # CB_SETCURSEL
                windows.message(dialog, 0x111, (api.GetDlgCtrlID(source) & 0xFFFF) | (9 << 16), source)
                assert windows.enabled(camera) == (selection == 0)
                assert windows.enabled(display) == (selection == 1)
            windows.click(windows.button(dialog, "Cancel"))
            wait_for_native_state(lambda: not windows.visible(dialog), "recording options cancelled")
        finally:
            if windows.visible(dialog):
                api.PostMessageW(dialog, 0x10, 0, 0)
        assert model(client) == before


def probe_recording(ffprobe, path, *, audio, minimum=1):
    """Read actual packets/frames, rather than treating file existence as capture."""
    data = json.loads(run_process([ffprobe, "-v", "error", "-count_frames", "-show_streams",
                                   "-show_format", "-of", "json", path], text=True, timeout=30))
    picture = next(s for s in data["streams"] if s["codec_type"] == "video")
    assert int(picture["nb_read_frames"]) >= minimum, picture
    assert picture["width"] > 0 and picture["height"] > 0
    assert any(s["codec_type"] == "audio" for s in data["streams"]) == audio, data
    return data


def record(client, path, *, duration=2, audio=True, source="GeneratedTest", success=True, extra=""):
    return client.file("RecordVideo", path, f'Source="{source}" Duration={duration} '
                       f'GeneratedAudio={int(audio)} {extra}', success=success)


def check_imported_recording(client, path, start, *, audio):
    rows = video_rows(client)
    picture = next(row for row in rows if row["source"].replace("\\", "/") == path.as_posix())
    assert abs(picture["start"]-start) < 0.002, picture
    assert picture["duration"] > 0.5 and abs(picture["sourceStart"]) < 0.002, picture
    sound = client.data("GetInfo: Type=Clips Format=JSON")
    if audio:
        members = [row for row in sound if abs(row["start"]-start) < 0.002]
        assert len(members) == 1 and abs(members[0]["end"]-(start+picture["duration"])) < 0.12, (picture, sound)
        linked = client.data("GetAudioClipGroups")
        assert len(linked) == 1 and linked[0]["group"], linked
        client.send(f'MoveVideoClip: Id="{picture["id"]}" Start={start+1}')
        moved = next(row for row in video_rows(client) if row["id"] == picture["id"])
        assert abs(client.data("GetInfo: Type=Clips Format=JSON")[0]["start"]-moved["start"]) < 0.002
        client.send("Undo")
    else:
        assert not sound, sound
    after = model(client)
    client.send("Undo")
    assert not video_rows(client) and not client.data("GetInfo: Type=Clips Format=JSON"), "Capture/import was not one Undo action."
    client.send("Redo")
    assert model(client) == after
    return after


def check_video_recording(client, root, ffmpeg, ffprobe):
    """Repeated MKV/MP4 capture, linked cursor placement and ordinary reopen."""
    for index, (extension, audio) in enumerate((("mkv", True), ("mp4", True), ("mkv", False))):
        client.send("New")
        start = 2.75 + index
        client.send(f"SelectTime: Start={start} End={start}")
        path = root / f"_recorded-{index}.{extension}"
        record(client, path, audio=audio, extra='Quality="Original" FrameRate=25')
        media = probe_recording(ffprobe, path, audio=audio, minimum=45)
        picture = next(s for s in media["streams"] if s["codec_type"] == "video")
        assert (picture["width"], picture["height"], picture["r_frame_rate"]) == (320, 180, "25/1"), picture
        assert picture["avg_frame_rate"] == "25/1", "AAC priming stretched the first picture interval."
        after = check_imported_recording(client, path, start, audio=audio)
        assert video_rows(client)[0]["fps"] == 25, "The imported frame clock differs from the requested capture rate."
        before_bytes = hashlib.sha256(path.read_bytes()).hexdigest()
        record(client, path, success=False)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == before_bytes and model(client) == after
        save_reopen(client, root / f"_recorded-{index}.aup3")
        assert model(client) == after
        # Every stream must decode, including audio in the fragmented MP4 file.
        run_process([ffmpeg, "-v", "error", "-i", path, "-f", "null", "-"], timeout=30)
        # OpenProject2 reports changes on its current project only. Keep the
        # harness's empty anchor, and close each completed fixture before New,
        # so a later reopen reuses that anchor instead of opening another window.
        client.send("Close")
    # Exercise real Windows screen capture without recording either monitor's
    # unrelated content: gdigrab's HWND input targets this test process only.
    client.send("New")
    client.send("SelectTime: Start=1 End=1")
    screen = root / "_owned-window.mkv"
    record(client, screen, audio=False, source="AuTopsyWindow", duration=2)
    probe_recording(ffprobe, screen, audio=False, minimum=40)
    check_imported_recording(client, screen, 1, audio=False)
    client.file("SaveProject2", root / "_owned-window.aup3")


def stop_recording(windows, dialog, path):
    wait_for_native_state(lambda: path.exists() and path.stat().st_size > 24000, "completed recording packets")
    time.sleep(0.4)
    windows.click(windows.button(dialog, "Stop and Add"))


def check_video_recording_stop(client, root, ffprobe):
    """Normal Stop closes the stream and imports once; repeating uses new media."""
    for index in range(2):
        client.send("New")
        client.send("SelectTime: Start=3.125 End=3.125")
        path = root / f"_stopped-{index}.{'mp4' if index else 'mkv'}"
        with_native_dialog(lambda: record(client, path, duration=0), "Record Video",
                           lambda windows, dialog, path=path: stop_recording(windows, dialog, path))
        probe_recording(ffprobe, path, audio=True, minimum=5)
        check_imported_recording(client, path, 3.125, audio=True)
        client.file("SaveProject2", root / f"_stopped-{index}.aup3")
        client.send("Close")


def terminate_owned_encoder():
    """Kill only the encoder whose current parent is this private app process."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD),
                    ("heap", ctypes.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                    ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD), ("name", w.WCHAR*260)]

    kernel.CreateToolhelp32Snapshot.argtypes, kernel.CreateToolhelp32Snapshot.restype = [w.DWORD, w.DWORD], w.HANDLE
    kernel.Process32FirstW.argtypes, kernel.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(ProcessEntry)], [w.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
    kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    assert snapshot and snapshot != ctypes.c_void_p(-1).value, ctypes.WinError()
    matches = []
    try:
        row = ProcessEntry(); row.size = ctypes.sizeof(row)
        found = kernel.Process32FirstW(snapshot, ctypes.byref(row))
        while found:
            if row.parent == int(os.environ["AUTOPSY_OWNED_APP_PID"]) and row.name.casefold() == "ffmpeg.exe":
                matches.append(row.pid)
            found = kernel.Process32NextW(snapshot, ctypes.byref(row))
    finally:
        kernel.CloseHandle(snapshot)
    assert len(matches) == 1, matches
    process = kernel.OpenProcess(1, False, matches[0])
    assert process, ctypes.WinError()
    try:
        assert kernel.TerminateProcess(process, 73), ctypes.WinError()
    finally:
        kernel.CloseHandle(process)


def check_video_recording_failures(client, root, ffprobe):
    """Reject before effects, preserve partial media, then succeed via a fresh call."""
    before = model(client)
    for index, extra in enumerate(('FrameRate=0', 'Duration=-1', 'Source="Unknown"', 'Quality="bad"')):
        path = root / f"_rejected-{index}.mkv"
        client.file("RecordVideo", path, f'Source="GeneratedTest" {extra}', success=False)
        assert not path.exists() and model(client) == before
    missing = root / "_missing-parent" / "_capture.mkv"
    record(client, missing, success=False)
    assert not missing.exists() and model(client) == before
    for index in range(2):
        empty = root / f"_missing-camera-{index}.mkv"
        record(client, empty, source="Webcam", extra='Camera="AuTopsy nonexistent test camera"', success=False)
        assert not empty.exists(), "A failed device left an empty output."
        partial = root / f"_interrupted-{index}.{'mp4' if index else 'mkv'}"

        def interrupt(windows, dialog, path=partial):
            wait_for_native_state(lambda: path.exists() and path.stat().st_size > 80000, "packets before encoder interruption")
            terminate_owned_encoder()

        with_native_dialog(lambda: record(client, partial, duration=0, success=False), "Record Video", interrupt, needs_focus=False)
        assert model(client) == before, "Failed capture changed the project."
        probe_recording(ffprobe, partial, audio=True, minimum=5)
        content = partial.read_bytes()
        record(client, partial, success=False)
        assert partial.read_bytes() == content and model(client) == before
        # A fresh ordinary entry call needs no prior execution/checkpoint state.
        finished = root / f"_fresh-{index}.mkv"
        record(client, finished)
        probe_recording(ffprobe, finished, audio=True, minimum=40)
        client.send("Undo")
        assert model(client) == before
