"""Focused native video editing, transport and visibility cases with neutral media."""
import ctypes
import json
import os
import re
import shutil
import time
from pathlib import Path

from run_fork_test_harness import (
    assert_owned_test_display,
    operate_owned_video_track,
    painted_video_tracks,
    ready_video_preview,
    run_process,
    save_reopen,
)


def wait_for_native_state(condition, description, timeout=8):
    """Wait for an asynchronous GUI/audio state; never replay its effect."""
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() >= deadline:
            raise AssertionError("Native state did not arrive: " + (description() if callable(description) else description))
        time.sleep(0.025)


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

    def click(self, handle):
        assert self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        wait_for_native_state(lambda: self.enabled(handle), f"preview button HWND={handle} enabled")
        reply = ctypes.c_size_t()
        if not self.api.SendMessageTimeoutW(handle, 0xF5, 0, 0, 2, 10000, ctypes.byref(reply)):
            raise RuntimeError(f"Native BM_CLICK failed for HWND={handle}: {ctypes.WinError()}")


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
    return info, handles


def press_preview_button(handle):
    """A native button event does not borrow shared mouse or keyboard input."""
    PreviewNativeWindows().click(handle)


def preview_project_seconds(status):
    caption = PreviewNativeWindows().caption(status)
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
            wait_for_native_state(lambda buttons=buttons: windows.enabled(buttons["play"]) and
                                  not windows.enabled(buttons["pause"]), "project paused")
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


def check_video_speed(client, root, ffmpeg, ffprobe, storage_only=False):
    """Retiming is one native edit across linked media, storage and rendering."""
    client.file("ImportVideo", root / "source.mp4")
    video = clip_rows(client)[0]
    track, clip_id = video["track"], video["id"]
    client.send(f'AddClipMarker: Track={track} At=1.5 Text="Speed flag" Id="' + "a" * 32 + '"')

    def change(speed, success=True):
        zoom = client.data("GetInfo: Type=View Format=JSON")["pixelsPerSecond"]
        reply = client.send(f'SetVideoClipSpeed: Track={track} Id="{clip_id}" Speed={speed}', success=success)
        assert client.data("GetInfo: Type=View Format=JSON")["pixelsPerSecond"] == zoom
        return reply

    def media():
        return client.data("GetLinkedMediaClips")

    def check_speed(speed):
        row = next(row for row in clip_rows(client) if row["id"] == clip_id and row["track"] == track)
        assert abs(row["playbackRate"] - speed) < 1e-9, row
        assert abs(row["end"] - 4 / speed) < 0.001, row
        audio = [row for row in media() if row["kind"] == "audio"]
        assert audio and all(abs(row["start"]) < 0.001 and abs(row["end"] - 4 / speed) < 0.05 for row in audio), audio
        flag = next(row for row in client.data("GetClipMarkers") if row["id"] == "a" * 32)
        assert abs(flag["time"] - 1.5 / speed) < 1e-8 and flag["visible"], flag

    if storage_only:
        for speed in (0.5, 2):
            change(speed)
            check_speed(speed)
            saved = media(), clip_rows(client), client.data("GetClipMarkers")
            save_reopen(client, root / f"_speed-{speed}.aup3")
            assert (media(), clip_rows(client), client.data("GetClipMarkers")) == saved
        return

    for _ in range(2):
        change(2)
        check_speed(2)
        accepted = media(), clip_rows(client)
        change(2)
        assert (media(), clip_rows(client)) == accepted
        client.send("Undo")
        check_speed(1)
        client.send("Redo")
        check_speed(2)
        client.send("SelectTime: Start=0 End=0")
        info, buttons = owned_preview_controls(client)
        press_preview_button(buttons["play"])
        wait_for_native_state(lambda info=info: preview_project_seconds(int(info["previewStatusHandle"])) > 0.65,
                              "retimed preview advancing on the project clock")
        press_preview_button(buttons["stop"])
        samples = [row for row in client.data("GetVideoPreviewInfo")["samples"] if row["audioPosition"] > 0.2]
        assert len(samples) >= 3, samples
        assert all(abs(row["speed"] - 2) < 0.001 for row in samples), samples
        assert max(abs(row["position"] - row["expectedPosition"]) for row in samples) < 0.15, samples
        change(1)
        check_speed(1)

    # A separately copied soundtrack piece blocks expansion. Rejection must
    # leave both media types untouched and must not create an Undo entry.
    before = media(), clip_rows(client)
    audio_track = next(row["track"] for row in before[0] if row["kind"] == "audio")
    select_clip_range(client, 0, 4, audio_track)
    client.send("Copy")
    client.send("SelectTime: Start=5 End=5")
    client.send("Paste")
    blocked = media(), clip_rows(client)
    change(0.5, success=False)
    assert (media(), clip_rows(client)) == blocked
    client.send("Undo")
    assert (media(), clip_rows(client)) == before

    change(0.5)
    check_speed(0.5)
    output = root / "_half-speed.mp4"
    client.file("ExportVideoMix", output, 'Preset="h264" Width=160 Height=90 FrameRate=30 Quality=18')
    metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-of", "json", str(output)], timeout=30))
    assert abs(float(metadata["format"]["duration"]) - 8) < 0.1, metadata
    for when, color in ((1, "red"), (5, "blue")):
        pixel = run_process([ffmpeg, "-v", "error", "-ss", str(when), "-i", str(output), "-frames:v", "1",
                             "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
        assert len(pixel) == 3, pixel
        red, green, blue = pixel
        assert red > 180 and green < 30 and blue < 30 if color == "red" else blue > 180 and red < 30 and green < 30, (color, pixel)
    import array

    pcm_samples = array.array("h", run_process([ffmpeg, "-v", "error", "-ss", "1", "-i", str(output), "-t", "1",
                                          "-map", "0:a:0", "-f", "s16le", "-ac", "1", "-ar", "8000", "pipe:1"], timeout=30))
    assert pcm_samples and max(abs(value) for value in pcm_samples) > 100
    crossings = sum(left <= 0 < right for left, right in zip(pcm_samples, pcm_samples[1:], strict=False))
    assert abs(crossings * 8000 / len(pcm_samples) - 440) < 20, crossings

    change(2)
    client.send("FitInWindow")
    view = client.data("GetInfo: Type=View Format=JSON")
    seam = next((frame / 60 for frame in range(30, 90)
                 if 0.02 < ((frame / 60 - view["horizontalStart"]) * view["pixelsPerSecond"]) % 1 < 0.48), 1)
    select_clip_range(client, seam, seam, track)
    client.send("Split")
    split = media(), clip_rows(client)
    for _ in range(2):
        operate_owned_video_track(client, seam, seam + 0.25, track=track, cancel=True)
        assert (media(), clip_rows(client)) == split
        operate_owned_video_track(client, seam, seam + 0.25, track=track)
        right = next(row for row in clip_rows(client) if row["start"] > seam)
        assert abs(right["start"] - seam - 0.25) < 0.025 and right["playbackRate"] == 2, right
        operate_owned_video_track(client, seam + 0.25, seam, track=track)
        assert (media(), clip_rows(client)) == split
    select_clip_range(client, 0, 2, track)
    client.send("HealVideoSplit")
    client.send("SeparateVideoClips")
    assert all(row["playbackRate"] == 2 for row in clip_rows(client))
    client.file("SaveProject2", root / "_retimed-split.aup3")


def check_video_visibility(client, root, ffmpeg, ffprobe):
    """Track buttons, native history/storage and decoded export share visibility."""
    source, other = root / "source.mp4", root / "other-source.mp4"
    client.file("ImportVideo", source)
    client.file("ImportVideo", other)
    other_track = next(row["track"] for row in clip_rows(client) if Path(row["source"]) == other)
    select_clip_range(client, 0, 2, other_track)
    client.send("SetTrackStatus: Focused=1")
    client.send("TrackMoveTop")
    client.send("FitV")
    client.send("FitInWindow")
    clips = clip_rows(client)
    front = next(row["track"] for row in clips if Path(row["source"]) == other)
    back = next(row["track"] for row in clips if Path(row["source"]) == source)
    assert front < back, clips
    linked_before = client.data("GetLinkedMediaClips")

    def states():
        return client.data("GetVideoClips")["tracks"]

    def toggle(track, area, cancel=False):
        operate_owned_video_track(client, 0.5, track=track, gesture="drag" if cancel else "click",
                                  area=area, cancel=cancel)

    def picture(path):
        client.send("SelectTime: Start=0.5 End=0.5")
        observed = {}
        def matches():
            info = client.data("GetVideoPreviewInfo")
            observed.update({key: info[key] for key in ("pictureSource", "pictureVisible", "ready", "position")})
            actual = Path(info["pictureSource"]) if info["pictureSource"] else None
            return actual == path and info["pictureVisible"] == bool(path)
        wait_for_native_state(matches, lambda: f"picture expected={path}; observed={observed}")

    def exported(name, colors, options='Preset="h264" Width=160 Height=90 FrameRate=30 Quality=18'):
        output = root / name
        client.file("ExportVideoMix", output, options)
        metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json", str(output)], timeout=30))
        assert abs(float(metadata["format"]["duration"]) - 4) < 0.08, metadata
        for when, color in colors:
            pixel = run_process([ffmpeg, "-v", "error", "-ss", str(when), "-i", str(output), "-frames:v", "1",
                                 "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
            assert len(pixel) == 3, pixel
            red, green, blue = pixel
            assert (max(pixel) < 15 if color == "black" else red > 180 and green < 30 and blue < 30
                    if color == "red" else blue > 180 and red < 30 and green < 30 if color == "blue"
                    else red > 70 and green > 70 and blue < 30), (when, color, tuple(pixel))
        import array

        samples = array.array("h", run_process([ffmpeg, "-v", "error", "-i", str(output), "-map", "0:a:0",
                                              "-f", "s16le", "-ac", "1", "-ar", "8000", "pipe:1"], timeout=30))
        assert samples and max(abs(value) for value in samples) > 100, "Video mute changed the soundtrack mix"
        return metadata

    baseline = states()
    picture(other)
    for _ in range(2):
        toggle(front, "mute", cancel=True)
        assert states() == baseline
        toggle(front, "mute")
        assert next(row for row in states() if row["index"] == front)["muted"]
        picture(source)
        client.send("Undo")
        assert states() == baseline
        client.send("Redo")
        assert next(row for row in states() if row["index"] == front)["muted"]
        client.send("Undo")
    toggle(back, "solo")
    picture(source)
    exported("_solo-back.mp4", [(0.5, "red"), (2.5, "blue")])
    toggle(front, "solo")
    picture(other)
    toggle(back, "mute")
    picture(other)
    exported("_muted-tail.mp4", [(0.5, "yellow"), (2.5, "black")])
    toggle(front, "mute")
    picture(None)
    hidden = states()
    assert client.data("GetLinkedMediaClips") == linked_before
    project = root / "_muted-solo.aup3"
    save_reopen(client, project)
    assert states() == hidden
    picture(None)
    relocated = root / "_temporarily-offline.mp4"
    source.replace(relocated)
    try:
        exported("_all-muted.mp4", [(0.5, "black"), (3.5, "black")])
        canvas = exported("_all-muted-source-size.mp4", [(1, "black")], 'Preset="h264"')
        video = next(stream for stream in canvas["streams"] if stream["codec_type"] == "video")
        assert (video["width"], video["height"]) == (90, 160), video
    finally:
        relocated.replace(source)
    # Normal clipboard track creation must carry visibility state, without
    # baking it into clips or altering their retained media ranges.
    select_clip_range(client, 0, 4, back)
    client.send("Copy")
    client.send("SelectAll")
    client.send("RemoveTracks")
    client.send("SelectTime: Start=0 End=0")
    client.send("Paste")
    copied = states()
    assert copied and all(row["muted"] and row["solo"] for row in copied), copied
    save_reopen(client, root / "_copied-visibility.aup3")
    assert states() == copied
    # Saved source geometry includes quarter-turn metadata. Black export must
    # use those dimensions rather than preview/window dimensions.
    rotated = root / "_rotated.mp4"
    run_process([ffmpeg, "-v", "error", "-display_rotation:v:0", "90", "-i", str(source),
                 "-map", "0", "-c", "copy", str(rotated)], timeout=30)
    metadata = json.loads(run_process([ffprobe, "-v", "error", "-select_streams", "v:0",
                                       "-show_entries", "stream_side_data=rotation", "-of", "json",
                                       str(rotated)], timeout=30))
    assert any(abs(side.get("rotation", 0)) == 90 for stream in metadata["streams"]
               for side in stream.get("side_data_list", [])), metadata
    client.send("SelectAll")
    client.send("RemoveTracks")
    client.file("ImportVideo", rotated)
    track = states()[0]["index"]
    toggle(track, "mute")
    save_reopen(client, root / "_rotated-muted.aup3")
    canvas = exported("_rotated-black.mp4", [(1, "black")], 'Preset="h264"')
    video = next(stream for stream in canvas["streams"] if stream["codec_type"] == "video")
    assert (video["width"], video["height"]) == (90, 160), video


def select_clip_range(client, start, end, track=0):
    client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={start} End={end}")


def clip_rows(client):
    return client.data("GetVideoClips")["clips"]


def check_video_clip_structure(client, root, linked, storage_only=False):
    """Exercise real commands, repeat boundaries, gestures, Undo and native XML.

    Candidates have one visible history effect. Cancelled gestures exercise the
    pre-effect boundary; fresh commands after save/reopen exercise the completed
    boundary without restoring caller progress. Linked audio stays as samples
    in its original clips, with both source handles and flags intact.
    """
    if storage_only:
        client.file("ImportVideo", root / "source.mp4")
        client.send('UnlinkMediaClips: Clips="0:0"')
        client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
        client.send("RemoveTracks")
        count = 1
        for _ in range(13):
            select_clip_range(client, 0, 4 * count)
            client.send("Copy")
            client.send(f"SelectTime: Start={4 * count} End={4 * count}")
            client.send("Paste")
            count *= 2
        select_clip_range(client, 0, 4 * count)
        client.send("Copy")
        client.send(f"SelectTime: Start={4 * count} End={4 * count}")
        client.send("Paste", success=False)
        assert len(clip_rows(client)) == 8192
        select_clip_range(client, 0, 4 * count)
        client.send("JoinVideoClips")
        client.send("Copy")
        client.send(f"SelectTime: Start={4 * count} End={4 * count}")
        client.send("Paste")
        expected = clip_rows(client)
        assert len(expected) == 2 and all(row["pieces"] == 8192 for row in expected), expected
        for repeat in range(2):
            save_reopen(client, root / f"_large-track-{repeat}.aup3")
            assert clip_rows(client) == expected
        return
    # The inherited menu listing does not escape every shortcut as JSON. Only
    # menu captions are needed here; leave that unrelated serializer untouched.
    menus = client.send("GetInfo: Type=Menus Format=JSON")
    assert "Heal Video Split" in menus and "Separate Joined Video" in menus, menus
    client.file("ImportVideo", root / "source.mp4")
    if not linked:
        client.send('UnlinkMediaClips: Clips="0:0"')
    client.send('AddClipMarker: Track=0 At=0.5 Text="Left flag" Id="' + "a" * 32 + '"')
    client.send('AddClipMarker: Track=0 At=3 Text="Right flag" Id="' + "b" * 32 + '"')
    select_clip_range(client, 2, 2)
    client.send("Split")
    before = clip_rows(client)
    audio_before = [row for row in client.data("GetLinkedMediaClips") if row["kind"] == "audio"]
    select_clip_range(client, 0, 4)
    client.send("HealVideoSplit")
    assert len(clip_rows(client)) == 1, clip_rows(client)
    healed = clip_rows(client)
    client.send("HealVideoSplit")
    assert clip_rows(client) == healed
    client.send("Undo")
    assert clip_rows(client) == before
    client.send("Redo")
    assert clip_rows(client) == healed
    client.send("Undo")
    # Use the existing Edit > Clips > Join command, not a private testing hook.
    select_clip_range(client, 0, 4)
    client.send("Join")
    joined = clip_rows(client)
    assert len(joined) == 1 and joined[0]["joined"] and joined[0]["pieces"] == 2, joined
    client.send("JoinVideoClips")
    assert clip_rows(client) == joined
    if linked:
        assert [row for row in client.data("GetLinkedMediaClips") if row["kind"] == "audio"] == audio_before
    client.send("MoveVideoClip: Track=0 Id=1 Start=2")
    client.send("MoveVideoClip: Track=0 Id=1 Start=2")
    moved = client.data("GetLinkedMediaClips")
    assert clip_rows(client)[0]["start"] == 2
    if linked:
        audio = [row for row in moved if row["kind"] == "audio"]
        assert [round(row["start"], 6) for row in audio] == [2, 4], audio
    client.send("FitInWindow")
    for repeat in range(2):
        operate_owned_video_track(client, 2, 5, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == moved
        operate_owned_video_track(client, 2, 5, track=0)
        trimmed = client.data("GetLinkedMediaClips")
        assert abs(clip_rows(client)[0]["start"] - 5) < 0.04, clip_rows(client)
        if linked:
            audio = [row for row in trimmed if row["kind"] == "audio"]
            assert len(audio) == 2, audio
            assert abs(audio[0]["end"] - audio[0]["start"]) < 1e-6, audio
            assert abs(audio[1]["start"] - 5) < 0.04, audio
            track = client.data("GetInfo: Type=Tracks Format=JSON")[1]
            assert abs(track["start"] - 5) < 0.04, track
        save_reopen(client, root / f"_hidden-joined-{repeat}.aup3")
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, 5, 2, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, 5, 2, track=0)
        assert client.data("GetLinkedMediaClips") == moved
        client.send("Undo")
        assert client.data("GetLinkedMediaClips") == trimmed
        client.send("Redo")
        assert client.data("GetLinkedMediaClips") == moved
    for repeat in range(2):
        operate_owned_video_track(client, 6, 3, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == moved
        operate_owned_video_track(client, 6, 3, track=0)
        trimmed = client.data("GetLinkedMediaClips")
        assert abs(clip_rows(client)[0]["end"] - 3) < 0.04, trimmed
        if linked:
            track = client.data("GetInfo: Type=Tracks Format=JSON")[1]
            assert abs(track["end"] - 3) < 0.04, track
        save_reopen(client, root / f"_hidden-right-joined-{repeat}.aup3")
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, 3, 6, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, 3, 6, track=0)
        assert client.data("GetLinkedMediaClips") == moved
        client.send("Undo")
        assert client.data("GetLinkedMediaClips") == trimmed
        client.send("Redo")
        assert client.data("GetLinkedMediaClips") == moved
    select_clip_range(client, 2, 6)
    flags = client.data("GetClipMarkers")
    save_reopen(client, root / "_joined-media.aup3")
    assert clip_rows(client)[0]["joined"]
    assert client.data("GetLinkedMediaClips") == moved
    assert client.data("GetClipMarkers") == flags
    select_clip_range(client, 2, 6)
    # Cancel and then choose the actual popup action on the joined picture.
    operate_owned_video_track(client, 3, track=0, gesture="context", menu_path=())
    assert len(clip_rows(client)) == 1
    operate_owned_video_track(client, 3, track=0, gesture="context", area="picture",
                              menu_path=("Separate Joined Clip",))
    separated = clip_rows(client)
    assert len(separated) == 2 and all(not row["joined"] for row in separated), separated
    assert [(row["start"], row["end"]) for row in separated] == [(2, 4), (4, 6)]
    client.send("SeparateVideoClips")
    assert clip_rows(client) == separated
    client.send("Undo")
    assert len(clip_rows(client)) == 1
    client.send("Redo")
    assert clip_rows(client) == separated
    if linked:
        client.send("MoveVideoClip: Track=0 Id=1 Start=1")
        audio = [row for row in client.data("GetLinkedMediaClips") if row["kind"] == "audio"]
        assert [round(row["start"], 6) for row in audio] == [1, 4], audio
        client.send("Undo")
    client.send("Save")
    if not linked:
        # A manual link after Join belongs to the parent timeline, rather than
        # to the original unlinked leaves. Its audio must follow both edges.
        select_clip_range(client, 2, 6)
        client.send("JoinVideoClips")
        client.send('LinkMediaClips: Clips="0:0,1:0"')
        before = client.data("GetLinkedMediaClips")
        client.send('LinkMediaClips: Clips="0:0,1:0"')
        assert client.data("GetLinkedMediaClips") == before
        client.send("FitInWindow")
        previous = {row["kind"]: row for row in before}
        for start, end, edge in ((2, 3, "start"), (6, 5, "end")):
            for repeat in range(2):
                operate_owned_video_track(client, start, end, track=0, cancel=True)
                assert client.data("GetLinkedMediaClips") == before
                operate_owned_video_track(client, start, end, track=0)
                trimmed = client.data("GetLinkedMediaClips")
                assert all(abs(row[edge] - previous[row["kind"]][edge] - (end - start)) < 0.04 for row in trimmed), trimmed
                save_reopen(client, root / f"_relinked-joined-{edge}-{repeat}.aup3")
                assert client.data("GetLinkedMediaClips") == trimmed
                operate_owned_video_track(client, end, start, track=0)
                assert client.data("GetLinkedMediaClips") == before
                client.send("Undo")
                assert client.data("GetLinkedMediaClips") == trimmed
                client.send("Redo")
                assert client.data("GetLinkedMediaClips") == before
        client.send("Save")


def check_video_compound_render(client, root, ffmpeg):
    """Picture lookup, real export and relinking agree for joined sources/gaps."""
    source, other = root / "source.mp4", root / "other-source.mp4"
    client.file("ImportVideo", source)
    client.send('UnlinkMediaClips: Clips="0:0"')
    select_clip_range(client, 1, 3)
    client.send("SplitDelete")
    client.file("ImportVideo", other)
    rows = clip_rows(client)
    source_track = next(row["track"] for row in rows if Path(row["source"]) == source)
    other_track = next(row["track"] for row in rows if Path(row["source"]) == other)
    select_clip_range(client, 0, 2, other_track)
    client.send("Copy")
    select_clip_range(client, 5, 5, source_track)
    client.send("Paste")
    select_clip_range(client, 0, 2, other_track)
    client.send("RemoveTracks")
    source_track = clip_rows(client)[0]["track"]
    select_clip_range(client, 0, 7, source_track)
    client.send("JoinVideoClips")
    joined = clip_rows(client)
    assert len(joined) == 1 and joined[0]["pieces"] == 3, joined
    # The deleted interior must remain black, even though both pieces still
    # retain handles into the same original source file.
    for when, path, expected in ((0.5, source, 0.5), (3.5, source, 3.5), (5.5, other, 0.5)):
        client.send(f"SelectTime: Start={when} End={when}")
        deadline = time.monotonic() + 5
        while True:
            info = client.data("GetVideoPreviewInfo")
            if Path(info["playbackPath"]) == path and abs(info["position"] - expected) < 0.05:
                break
            if time.monotonic() > deadline:
                raise AssertionError(info)
            time.sleep(0.05)
    client.send("SelectAll")
    client.send("SetTrackAudio: Mute=1")
    output = root / "_compound.mp4"
    client.file("ExportVideoMix", output, 'Preset="h264" Width=160 Height=90 FrameRate=30 Quality=18')
    for when, color in ((0.5, "red"), (2, "black"), (3.5, "blue"), (4.5, "black"), (5.5, "yellow")):
        pixel = run_process([ffmpeg, "-v", "error", "-ss", str(when), "-i", str(output),
                             "-frames:v", "1", "-vf", "scale=1:1", "-pix_fmt", "rgb24",
                             "-f", "rawvideo", "pipe:1"], timeout=30)
        assert len(pixel) == 3, pixel
        red, green, blue = pixel
        assert (max(pixel) < 15 if color == "black" else
                red > 180 and green < 30 and blue < 30 if color == "red" else
                blue > 180 and red < 30 and green < 30 if color == "blue" else
                green > 70 and red > 70 and blue < 30), (when, color, tuple(pixel))
    replacement = root / "_relocated-yellow.mp4"
    shutil.copyfile(other, replacement)
    command = "OldPath=" + json.dumps(other.as_posix())
    client.file("RelinkVideoMedia", replacement, command)
    client.file("RelinkVideoMedia", replacement, command)
    select_clip_range(client, 0, 7, source_track)
    save_reopen(client, root / "_mixed-compound.aup3")
    select_clip_range(client, 0, 7, source_track)
    client.send("SeparateVideoClips")
    restored = clip_rows(client)
    assert [(row["start"], row["end"]) for row in restored] == [(0, 1), (3, 4), (5, 7)], restored
    assert Path(restored[-1]["source"]) == replacement, restored
    client.send("Save")

def check_split_video_edges(client, root, linked):
    """A split seam must trim the right clip, including its linked soundtrack."""
    if client.data("GetInfo: Type=Tracks Format=JSON"):
        client.send("SelectAll")
        client.send("RemoveTracks")
    client.file("ImportVideo", root / "source.mp4")
    if not linked:
        client.send('UnlinkMediaClips: Clips="0:0"')
    client.send("FitInWindow")
    view = client.data("GetInfo: Type=View Format=JSON")
    # Prefer a seam whose painted pixel falls before its exact source time.
    at = next((frame / 30 for frame in range(30, 90)
               if 0.02 < ((frame / 30 - view["horizontalStart"]) * view["pixelsPerSecond"]) % 1 < 0.48), 2)
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={at} End={at}")
    client.send("Split")
    original = client.data("GetLinkedMediaClips")
    video = client.data("GetVideoClips")
    for _ in range(2):
        operate_owned_video_track(client, at, at + 0.5, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == original
        operate_owned_video_track(client, at, at + 0.5, track=0)
        trimmed = client.data("GetLinkedMediaClips")
        assert len(trimmed) == len(original), trimmed
        for before, after in zip(original, trimmed, strict=True):
            start = before["start"] + (0.5 if before["clip"] == 1 and (linked or before["kind"] == "video") else 0)
            assert abs(after["start"] - start) < 0.04 and after["end"] == before["end"], trimmed
        operate_owned_video_track(client, at + 0.5, at, track=0, cancel=True)
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, at + 0.5, at, track=0)
        assert client.data("GetLinkedMediaClips") == original
        assert client.data("GetVideoClips") == video
        client.send("Undo")
        assert client.data("GetLinkedMediaClips") == trimmed
        client.send("Redo")
        assert client.data("GetLinkedMediaClips") == original
    client.file("SaveProject2", root / "_split-video-edges.aup3")
