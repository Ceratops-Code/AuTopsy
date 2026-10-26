"""Focused native video editing, crop and visibility cases with neutral media."""
import array
import ctypes
import json
import os
import shutil
import time
from pathlib import Path

from run_audacity_consumer import read_audio_project_state, select_audio_project_tracks
from run_fork_test_harness import (
    assert_owned_test_display,
    operate_owned_video_track,
    run_process,
    save_reopen,
)
from test_video_transport import (
    PreviewNativeWindows,
    crop_pixel_counts,
    expect_preview_crop_pixels,
    owned_preview_controls,
    press_preview_button,
    preview_project_seconds,
    wait_for_native_state,
)


def check_video_audio_edit_parity(client, root):
    """Compare native range edits on linked and unlinked picture/sound tracks."""
    def model():
        return clip_rows(client), client.data("GetInfo: Type=Clips Format=JSON")

    def matching_occupied_ranges():
        picture, audio = model()

        def occupied(rows):
            tracks: dict[int, list[tuple[float, float]]] = {}
            for row in rows:
                tracks.setdefault(row["track"], []).append((row["start"], row["end"]))
            result = []
            for intervals in tracks.values():
                merged: list[tuple[float, float]] = []
                for start, end in sorted(intervals):
                    if merged and start <= merged[-1][1] + 0.0001:
                        merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
                    else:
                        merged.append((start, end))
                result.append(merged)
            return sorted(result)

        video_ranges, audio_ranges = occupied(picture), occupied(audio)
        assert len(video_ranges) == len(audio_ranges), (picture, audio)
        for video, sound in zip(video_ranges, audio_ranges, strict=True):
            assert len(video) == len(sound), (picture, audio)
            assert all(abs(v-s) < 0.05 for a, b in zip(video, sound, strict=True)
                       for v, s in zip(a, b, strict=True)), (picture, audio)

    for linked in (False, True):
        client.send("New")
        client.file("ImportVideo", root / "source.mp4")
        if not linked:
            client.send('UnlinkMediaClips: Clips="0:0"')
        original = model()
        for _repeat in range(2):
            for operation in ("Copy", "Cut", "SplitCut", "Trim", "Duplicate"):
                client.send("SelectAll")
                client.send("SelectTime: Start=1 End=3")
                client.send(operation)
                steps = 1
                if operation in ("Copy", "Cut", "SplitCut"):
                    client.send("SelectTime: Start=6 End=6")
                    client.send("Paste")
                    steps = 1 if operation == "Copy" else 2
                    copied = [row for row in clip_rows(client) if row["start"] >= 6]
                    assert len(copied) == 1 and abs(copied[0]["sourceStart"]-1) < 0.0001, copied
                matching_occupied_ranges()
                after = model()
                client.send("Undo")
                client.send("Redo")
                assert model() == after
                for _ in range(steps):
                    client.send("Undo")
                assert model() == original, (operation, model(), original)
        client.send("SelectAll")
        client.send("SelectTime: Start=2 End=2")
        client.send("Split")
        split = model()
        client.send("SelectAll")
        client.send("SelectTime: Start=0 End=4.05")
        client.send("Join")
        joined = model()
        assert len(joined[0]) == len(joined[1]) == 1, joined
        matching_occupied_ranges()
        if linked:
            picture = joined[0][0]
            client.send(f'MoveVideoClip: Track={picture["track"]} Id="{picture["id"]}" Start=5')
            matching_occupied_ranges()
            assert abs(model()[1][0]["start"]-5) < 0.002, model()
            client.send("Undo")
        # Repeating an already completed mixed-media Join is a no-op, and a
        # single Undo restores both selected media types to their split state.
        client.send("SelectAll")
        client.send("SelectTime: Start=0 End=4.05")
        client.send("Join")
        client.send("Undo")
        assert model() == split
        client.send("Redo")
        assert model() == joined
        save_reopen(client, root / f"_parity-{linked}.aup3")
        assert model() == joined
        client.send("Close")

    # Two different soundtrack tracks can be linked to different pieces of one
    # picture. Their native joins must both follow the resulting picture parent.
    client.send("New")
    client.file("ImportVideo", root / "source.mp4")
    client.send('UnlinkMediaClips: Clips="0:0"')
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=0 End=4.05")
    client.send("Duplicate")
    client.send("SelectAll")
    client.send("SelectTime: Start=2 End=2")
    client.send("Split")
    client.send('LinkMediaClips: Clips="0:0,1:0"')
    client.send('LinkMediaClips: Clips="0:1,2:1"')
    client.send("SelectAll")
    client.send("SelectTime: Start=0 End=4.05")
    split = model()
    client.send("Join")
    joined = model()
    assert len(joined[0]) == 1 and len(joined[1]) == 2, joined
    client.send('MoveVideoClip: Track=0 Id="1" Start=5')
    assert all(abs(row["start"]-5) < 0.002 for row in model()[1]), model()
    client.send("Undo")
    client.send("Undo")
    assert model() == split
    client.send("Redo")
    assert model() == joined
    save_reopen(client, root / "_parity-two-soundtracks.aup3")
    assert model() == joined


def crop_dialog_roundtrip(client, commit, reset=False):
    """Open the real clip menu and complete/cancel only the owned crop dialog."""
    import threading

    native = PreviewNativeWindows()
    user = native.api
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
    user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
    user.EnumChildWindows.argtypes = [ctypes.c_void_p, callback_type, ctypes.c_ssize_t]
    user.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user.GetForegroundWindow.restype = ctypes.c_void_p
    errors = []
    reset_fields: list[str] = []

    def open_menu():
        try:
            operate_owned_video_track(client, 1, track=0, gesture="context",
                                      menu_path=("Crop Picture...",))
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=open_menu, daemon=True)
    thread.start()
    dialog = None

    def find_dialog():
        matches = []

        @callback_type
        def visit(handle, _):
            if (native.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
                    and native.visible(handle) and native.caption(handle) == "Crop Video Picture"):
                matches.append(handle)
            return 1

        user.EnumWindows(visit, 0)
        assert len(matches) <= 1, matches
        return matches[0] if matches else None

    def button(caption):
        matches = []

        @callback_type
        def visit(handle, _):
            if native.caption(handle).replace("&", "") == caption:
                matches.append(handle)
            return 1

        user.EnumChildWindows(dialog, visit, 0)
        assert len(matches) == 1, (caption, matches)
        return matches[0]

    def reset_fields_are_zero():
        reset_fields.clear()

        @callback_type
        def visit(handle, _):
            kind = ctypes.create_unicode_buffer(64)
            user.GetClassNameW(handle, kind, len(kind))
            if kind.value == "Edit":
                reset_fields.append(native.control_text(handle))
            return 1

        user.EnumChildWindows(dialog, visit, 0)
        return len(reset_fields) == 4 and all(float(value.replace(",", ".")) == 0 for value in reset_fields)

    try:
        deadline = time.monotonic() + 10
        while not (dialog := find_dialog()):
            if errors:
                raise errors[0]
            assert time.monotonic() < deadline, "The owned crop dialog did not appear."
            time.sleep(0.025)
        assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]), dialog)
        # Opening the menu also restores its prior focus. Finish that gesture
        # before button input; BM_CLICK can be ignored on an inactive dialog.
        thread.join(timeout=10)
        assert not thread.is_alive(), "The owned menu gesture did not finish."
        if errors:
            raise errors[0]
        user.SetForegroundWindow(dialog)
        wait_for_native_state(lambda: user.GetForegroundWindow() == dialog, "owned crop dialog active")
        if reset:
            native.click(button("Reset Crop"))
            wait_for_native_state(reset_fields_are_zero,
                                  lambda: f"crop reset shown in all four fields: {reset_fields}")
        native.click(button("OK" if commit else "Cancel"))
        wait_for_native_state(lambda: not native.visible(dialog), "crop dialog closed")
    finally:
        if dialog and native.visible(dialog):
            user.PostMessageW(dialog, 0x10, 0, 0)
        thread.join(timeout=10)
    assert not thread.is_alive(), "The owned menu operation did not finish."
    if errors:
        raise errors[0]


def check_video_crop(client, root, ffmpeg, ffprobe, controls_only=False):
    """Real crop pixels, native history/dialogs, retained pieces and ordinary AUP3."""
    source = root / "source.mp4"
    recipe = ("color=c=black:s=160x90:r=30:d=4,"
              "drawbox=x=0:y=0:w=80:h=45:color=red:t=fill,"
              "drawbox=x=80:y=0:w=80:h=45:color=green:t=fill,"
              "drawbox=x=0:y=45:w=80:h=45:color=blue:t=fill,"
              "drawbox=x=80:y=45:w=80:h=45:color=white:t=fill")
    run_process([ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i", recipe,
                 "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=4",
                 "-c:v", "mpeg4", "-q:v", "2", "-c:a", "aac", "-shortest", str(source)], timeout=30)
    print("crop: importing generated neutral media", flush=True)
    client.file("ImportVideo", source)
    clip_id = client.data("GetVideoClips")["clips"][0]["id"]
    original_audio = client.data("GetInfo: Type=Clips Format=JSON")

    def margins():
        return client.data("GetVideoClips")["clips"][0]["crop"]

    def set_crop(left=0, top=0, right=0, bottom=0):
        client.send(f'SetVideoClipCrop: Id="{clip_id}" Track=0 Left={left} Top={top} Right={right} Bottom={bottom}')

    set_crop(left=0.123456789)
    if controls_only:
        print("crop: native dialog, reset and history", flush=True)
        for commit, reset in ((False, True), (True, False)):
            before = margins()
            crop_dialog_roundtrip(client, commit, reset)
            assert margins() == before  # Cancel/reset staging and untouched precision are no-ops.
            client.send("Undo")
            assert margins()["left"] == 0
            client.send("Redo")
            assert abs(margins()["left"] - 0.123456789) < 1e-12
        crop_dialog_roundtrip(client, True, True)
        assert margins()["left"] == 0
        client.send("Undo")
        assert abs(margins()["left"] - 0.123456789) < 1e-12
        client.file("SaveProject2", root / "_crop-state.aup3")
        return
    set_crop(left=0.5)
    set_crop(left=0.5)
    client.send("Undo")
    assert abs(margins()["left"] - 0.123456789) < 1e-12
    client.send("Redo")
    assert margins()["left"] == 0.5
    before = margins()
    client.send(f'SetVideoClipCrop: Id="{clip_id}" Track=0 Left=0.5 Right=0.5', success=False)
    assert margins() == before
    client.send("ShowVideoPreview")
    print("crop: rendered preview and export pixels", flush=True)
    expect_preview_crop_pixels(client, root, ffmpeg, {"green", "white"})

    def export_pixels(name, expected, expected_size):
        output = root / name
        client.file("ExportVideoMix", output, "Overwrite=1")
        metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-show_streams",
                                          "-of", "json", str(output)]))
        assert abs(float(metadata["format"]["duration"]) - 4) < 0.1, metadata
        video = next(stream for stream in metadata["streams"] if stream["codec_type"] == "video")
        assert (video["width"], video["height"]) == expected_size, video
        rgb = run_process([ffmpeg, "-v", "error", "-ss", "1", "-i", str(output), "-frames:v", "1",
                           "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
        counts = crop_pixel_counts(rgb)
        assert all(counts[colour] > 50 for colour in expected), counts
        assert sum(value for colour, value in counts.items() if colour not in expected) < 30, counts
        assert not any(max(rgb[index:index + 3]) < 20 for index in range(0, len(rgb), 3)), "Crop export added black bars."

    for _ in range(2):
        export_pixels("cropped.mp4", {"green", "white"}, (80, 90))
    assert client.data("GetInfo: Type=Clips Format=JSON") == original_audio
    set_crop(top=0.5)
    print("crop: saved projects and compound clips", flush=True)
    saved = root / "cropped.aup3"
    save_reopen(client, saved)
    assert margins()["top"] == 0.5
    client.send("ShowVideoPreview")
    expect_preview_crop_pixels(client, root, ffmpeg, {"blue", "white"})
    select_clip_range(client, 2, 2)
    client.send("Split")
    select_clip_range(client, 0, 4)
    client.send("JoinVideoClips")
    clip_id = client.data("GetVideoClips")["clips"][0]["id"]
    set_crop(left=0.5)
    export_pixels("joined-crop.mp4", {"white"}, (80, 44))
    save_reopen(client, root / "joined-crop.aup3")
    client.send("ShowVideoPreview")
    expect_preview_crop_pixels(client, root, ffmpeg, {"white"})
    select_clip_range(client, 0, 4)
    client.send("SeparateVideoClips")
    rows = client.data("GetVideoClips")["clips"]
    assert len(rows) == 2 and all(row["crop"]["left"] == 0.5 and row["crop"]["top"] == 0.5 for row in rows), rows

    rotated = root / "rotated.mp4"
    print("crop: rotated source preview and export", flush=True)
    run_process([ffmpeg, "-v", "error", "-display_rotation:v:0", "90", "-i", str(source),
                 "-map", "0", "-c", "copy", str(rotated)], timeout=30)
    client.send("SelectAll")
    client.send("RemoveTracks")
    client.file("ImportVideo", rotated)
    clip_id = client.data("GetVideoClips")["clips"][0]["id"]
    set_crop(bottom=0.5)
    client.send("ShowVideoPreview")
    expect_preview_crop_pixels(client, root, ffmpeg, {"green", "white"})
    export_pixels("rotated-crop.mp4", {"green", "white"}, (90, 80))
    # The worker's normal Close must never prompt for these owned test edits.
    client.file("SaveProject2", root / "_crop-state.aup3")


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


def check_video_clip_mute(client, root, ffmpeg, ffprobe):
    """One retained picture occurrence hides independently of its linked sound.

    The marked case owner owns generated media and the retained compatibility
    descriptor. Its normal lifecycle removes scratch exports and private apps.
    """
    for colour, frequency in (("red", 440), ("blue", 880)):
        source = root / f"_{colour}.mp4"
        run_process([ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                     f"color=c={colour}:s=240x120:r=30:d=4", "-f", "lavfi", "-i",
                     f"sine=frequency={frequency}:sample_rate=48000:duration=4",
                     "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", str(source)], timeout=30)
        client.file("ImportVideo", source)
    rows = clip_rows(client)
    front = next(row["track"] for row in rows if Path(row["source"]).stem == "_red")
    select_clip_range(client, 0, 4, front)
    client.send("SetTrackStatus: Focused=1")
    client.send("TrackMoveTop")
    rows = clip_rows(client)
    front = next(row["track"] for row in rows if Path(row["source"]).stem == "_red")
    select_clip_range(client, 2, 2, front)
    client.send("Split")
    client.send("FitInWindow")
    client.send("FitV")
    clips = [row for row in clip_rows(client) if row["track"] == front]
    clip_id = clips[0]["id"]
    linked = client.data("GetLinkedMediaClips")
    audio_clips = client.data("GetInfo: Type=Clips Format=JSON")

    def sound(name):
        # Include the exact native audio extent, including codec padding after
        # the final picture frame; stock Audacity exports that full arrangement.
        client.send("SelectAll")
        select_audio_project_tracks(client.send, client.data)
        output = root / name
        client.file("Export2", output, "NumChannels=2")
        return array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(output),
                                            "-f", "f32le", "pipe:1"], timeout=30))

    baseline = sound("_before-mute.wav")
    for _ in range(2):
        for _ in range(2):
            client.send(f'SetVideoClipMute: Track={front} Id="{clip_id}" Muted=1')
        assert [row["muted"] for row in clip_rows(client) if row["track"] == front] == [True, False]
        client.send("Undo")
        assert not any(row["muted"] for row in clip_rows(client))
        client.send("Redo")
        assert next(row for row in clip_rows(client) if row["track"] == front and row["id"] == clip_id)["muted"]
        client.send(f'SetVideoClipMute: Track={front} Id="999999" Muted=0', success=False)
        operate_owned_video_track(client, 1, track=front, gesture="context")
        assert next(row for row in clip_rows(client) if row["track"] == front and row["id"] == clip_id)["muted"]
        for expected in (False, True):
            operate_owned_video_track(client, 1, track=front, gesture="context", menu_path=("Mute Clip",))
            current = [row for row in clip_rows(client) if row["track"] == front]
            assert [row["muted"] for row in current] == [expected, False], current
        assert client.data("GetLinkedMediaClips") == linked
        assert client.data("GetInfo: Type=Clips Format=JSON") == audio_clips
        client.send(f'SetVideoClipMute: Track={front} Id="{clip_id}" Muted=0')
    client.send(f'SetVideoClipMute: Track={front} Id="{clip_id}" Muted=1')

    def picture(at, colour):
        client.send(f"SelectTime: Start={at} End={at}")
        client.send("ShowVideoPreview")
        expect_preview_crop_pixels(client, root, ffmpeg, {colour})

    def exported(name, colours):
        output = root / name
        client.file("ExportVideoMix", output, "Overwrite=1")
        metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-of", "json", str(output)]))
        assert abs(float(metadata["format"]["duration"]) - 4) < 0.08, metadata
        for at, colour in colours:
            pixel = run_process([ffmpeg, "-v", "error", "-ss", str(at), "-i", str(output), "-frames:v", "1",
                                 "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
            assert max(range(3), key=lambda i: pixel[i]) == (2 if colour == "blue" else 0), (at, colour, pixel)

    picture(1, "blue")
    picture(3, "red")
    for _ in range(2):
        exported("_clip-mute.mp4", [(1, "blue"), (3, "red")])
    saved = clip_rows(client)
    project = root / "_clip-mute.aup3"
    save_reopen(client, project)
    assert clip_rows(client) == saved
    select_clip_range(client, 0, 4, front)
    client.send("JoinVideoClips")
    saved = clip_rows(client)
    save_reopen(client, project, save_existing=True)
    assert clip_rows(client) == saved
    exported("_joined-mute.mp4", [(1, "blue"), (3, "red")])
    client.send("SeparateVideoClips")
    assert [row["muted"] for row in clip_rows(client) if row["track"] == front] == [True, False]
    client.send("JoinVideoClips")
    joined = next(row for row in clip_rows(client) if row["track"] == front)
    client.send(f'SetVideoClipMute: Track={front} Id="{joined["id"]}" Muted=1')
    exported("_parent-mute.mp4", [(1, "blue"), (3, "blue")])
    after = sound("_clip-mute.wav")
    assert len(after) == len(baseline) and max(abs(a - b) for a, b in zip(after, baseline, strict=True)) < 0.0001
    client.send("Save")
    expected_projects = [{"project": project.name, "state": read_audio_project_state(client.data), "audio": "_clip-mute.wav"}]
    (root / "_compatibility-expected.json").write_text(json.dumps(expected_projects), encoding="utf-8")
    for scratch in ("_before-mute.wav", "_clip-mute.mp4", "_joined-mute.mp4", "_parent-mute.mp4"):
        (root / scratch).unlink(missing_ok=True)


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


def check_video_clip_structure(client, root, linked, storage_only=False, parent_links_only=False):
    """Exercise real commands, repeat boundaries, gestures, Undo and native XML.

    Candidates have one visible history effect. Cancelled gestures exercise the
    pre-effect boundary; fresh commands after save/reopen exercise the completed
    boundary without restoring caller progress. Linked audio stays as samples
    in its original clips, with both source handles and flags intact.
    """
    if parent_links_only:
        client.file("ImportVideo", root / "source.mp4")
        client.send('UnlinkMediaClips: Clips="0:0"')
        select_clip_range(client, 2, 2)
        client.send("Split")
        select_clip_range(client, 0, 4)
        client.send("JoinVideoClips")
        client.send("MoveVideoClip: Track=0 Id=1 Start=2")
        client.send('LinkMediaClips: Clips="0:0,1:0"')
        for speed in (1, 2):
            client.send(f'SetVideoClipSpeed: Track=0 Id="1" Speed={speed}')
            before = client.data("GetLinkedMediaClips")
            client.send('LinkMediaClips: Clips="0:0,1:0"')
            assert client.data("GetLinkedMediaClips") == before
            video = next(row for row in before if row["kind"] == "video")
            restore = f'SetVideoClipBounds: Track=0 Id="1" Start={video["start"]} End={video["end"]}'
            for edge in ("start", "end"):
                for repeat in range(2):
                    start, end = video["start"], video["end"]
                    delta = 1 / speed * (1 if edge == "start" else -1)
                    if edge == "start":
                        start += delta
                    else:
                        end += delta
                    command = f'SetVideoClipBounds: Track=0 Id="1" Start={start} End={end}'
                    client.send(command)
                    trimmed = client.data("GetLinkedMediaClips")
                    assert all(abs(after[edge] - original[edge] - delta) < 1e-7
                               for after, original in zip(trimmed, before, strict=True)), trimmed
                    client.send(command)
                    assert client.data("GetLinkedMediaClips") == trimmed
                    client.send("Undo")
                    assert client.data("GetLinkedMediaClips") == before
                    client.send("Redo")
                    assert client.data("GetLinkedMediaClips") == trimmed
                    save_reopen(client, root / f"_parent-{speed}-{edge}-{repeat}.aup3")
                    assert client.data("GetLinkedMediaClips") == trimmed
                    client.send(restore)
                    assert client.data("GetLinkedMediaClips") == before
                    client.send(restore)
                    client.send("Undo")
                    assert client.data("GetLinkedMediaClips") == trimmed
                    client.send("Redo")
                    assert client.data("GetLinkedMediaClips") == before
                    client.send("Save")
        return
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
    for area in ("header", "picture"):
        # Keep a preceding body click/Escape and the edge drag in one input
        # session. Restoring the pointer outside the pane used to mask this bug.
        preceding = at + 0.25 if area == "picture" else None
        operate_owned_video_track(client, at, at + 0.5, track=0, cancel=True,
                                  area=area, prior_click_time=preceding)
        assert client.data("GetLinkedMediaClips") == original
        operate_owned_video_track(client, at, at + 0.5, track=0,
                                  area=area, prior_click_time=preceding)
        trimmed = client.data("GetLinkedMediaClips")
        assert len(trimmed) == len(original), trimmed
        for before, after in zip(original, trimmed, strict=True):
            start = before["start"] + (0.5 if before["clip"] == 1 and (linked or before["kind"] == "video") else 0)
            assert abs(after["start"] - start) < 0.04 and after["end"] == before["end"], trimmed
        operate_owned_video_track(client, at + 0.5, at, track=0, cancel=True, area=area)
        assert client.data("GetLinkedMediaClips") == trimmed
        operate_owned_video_track(client, at + 0.5, at, track=0, area=area)
        assert client.data("GetLinkedMediaClips") == original
        assert client.data("GetVideoClips") == video
        client.send("Undo")
        assert client.data("GetLinkedMediaClips") == trimmed
        client.send("Redo")
        assert client.data("GetLinkedMediaClips") == original
    client.file("SaveProject2", root / "_split-video-edges.aup3")
