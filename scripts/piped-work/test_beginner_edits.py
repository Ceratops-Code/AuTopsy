"""Focused beginner-edit cases using the public commands and owned dialogs.

The shared harness owns generated media, private processes, display selection,
bounded receipts and cleanup. No user project, microphone or personal video is
used. Every edit is checked before effects, after success, after Undo/Redo and
after a normal save/reopen; modal cancellation must leave the model untouched.
"""
import math
import hashlib
import json
import shutil
import struct
from pathlib import Path

from run_fork_test_harness import operate_owned_video_track, save_reopen
from test_clip_actions import selected_audio, with_native_dialog
from run_audacity_consumer import read_audio_project_state, select_audio_project_tracks

# One declared scope feeds command choices, neutral-video production and input
# identities. Registering a beginner case cannot omit its generated fixture.
BEGINNER_VIDEO_CASES = ("delete-gap", "clip-properties", "visual-crop", "collect-media", "video-fades", "clip-snapping")


def audio_rows(client):
    return client.data("GetInfo: Type=Clips Format=JSON")


def check_video_fades(client, root, ffmpeg, ffprobe):
    """Decode actual blends, preview pixels and stationary sound; retain edits."""
    from run_fork_test_harness import run_process, ready_video_preview
    from test_video_clip_edits import expect_preview_crop_pixels, wait_for_native_state

    for colour, frequency in (("red", 440), ("blue", 660)):
        run_process([ffmpeg, "-v", "error", "-nostdin", "-n", "-f", "lavfi", "-i",
                     f"color=c={colour}:s=160x100:r=30:d=4", "-f", "lavfi", "-i",
                     f"sine=frequency={frequency}:sample_rate=48000:duration=4", "-c:v", "libx264",
                     "-preset", "ultrafast", "-crf", "0", "-c:a", "aac", str(root / f"_{colour}.mp4")], timeout=30)
    client.file("ImportVideo", root / "_red.mp4")
    client.file("ImportVideo", root / "_blue.mp4")
    # The first imported track is the upper picture; resolve actual rows rather
    # than depending on how new tracks are inserted in a future upstream.
    clips = video_rows(client)
    upper = min(clips, key=lambda row: row["track"])
    track, identifier = upper["track"], upper["id"]
    original = model(client)
    audio_before = selected_audio(client, root, "_fades-before.wav", 1, 0, 4, ffmpeg)
    request = f'SetVideoClipFades: Track={track} Id="{identifier}" In=1 Out=1'
    client.send(request)
    faded = model(client)
    assert faded["audio"] == original["audio"]
    assert faded["video"][0]["start"] == original["video"][0]["start"]
    assert faded["video"][0]["fades"] == {"inStart": 0, "inDuration": 1, "outEnd": 4, "outDuration": 1}
    client.send(request)
    client.send("Undo")
    assert model(client) == original, "Repeated fades added an Undo step."
    client.send("Redo")
    assert model(client) == faded
    for arguments in ("In=5", "Out=5", "In=-0.5"):
        client.send(f'SetVideoClipFades: Track={track} Id="{identifier}" {arguments}', success=False)
        assert model(client) == faded
    client.send(f'SetVideoClipFades: Track={track} Id="999999" In=1', success=False)
    assert model(client) == faded
    client.send("FitV")
    client.send("FitInWindow")
    for cancelled in (True, False):
        operate_owned_video_track(client, .25, 1.25, track=track, cancel=cancelled)
        if cancelled:
            assert model(client) == faded
        else:
            moved = next(row for row in video_rows(client) if row["track"] == track)
            assert abs(moved["start"] - 1) < .04, moved
            client.send("Undo")
            assert model(client) == faded
    for title, item in (("Fade Picture", "Fade Picture..."),
                        ("Crossfade from Video Below", "Crossfade from Video Below...")):
        for _ in range(2):
            def cancel(windows, dialog):
                windows.text(windows.field(dialog, "Fade in (seconds)"), "0.25")
                windows.click(windows.button(dialog, "Cancel"))
            with_native_dialog(lambda menu=item: operate_owned_video_track(client, .5, track=track,
                gesture="context", menu_path=(menu,)), title, cancel, menu_gesture=True)
            assert model(client) == faded
    output = root / "_fades.mp4"
    def pixels(path):
        return run_process([ffmpeg, "-v", "error", "-i", str(path), "-an", "-vf", "scale=1:1",
                            "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
    first = None
    for _ in range(2):
        client.file("ExportVideoMix", output, 'Overwrite=1 Preset="h264" FrameRate=30 Quality=18')
        metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(output)]))
        picture = next(row for row in metadata["streams"] if row["codec_type"] == "video")
        assert (picture["width"], picture["height"], int(picture["nb_frames"])) == (160, 100, 120), picture
        frames = pixels(output)
        assert len(frames) == 120 * 3
        for index in (0, 15, 30, 60, 90, 105, 119):
            alpha = min(1, index / 30, (120 - index) / 30)
            red, green, blue = frames[index * 3:index * 3 + 3]
            expected_red = 255 * (alpha if Path(upper["source"]).stem == "_red" else 1 - alpha)
            assert abs(red - expected_red) < 18 and abs(blue - (255 - expected_red)) < 18 and green < 15, (index, (red, green, blue), expected_red)
        if first is not None:
            assert frames == first
        first = frames
    # The same composite is shown by the actual decoder, rather than a still
    # thumbnail or an export-only effect.
    select(client, track, .5, .5)
    wait_for_native_state(lambda: abs(ready_video_preview(client)["position"] - .5) < .05,
                          "Faded preview did not seek to the selected project time.")
    expect_preview_crop_pixels(client, root, ffmpeg, {"purple"})
    audio_after = selected_audio(client, root, "_fades-after.wav", 1, 0, 4, ffmpeg)
    assert max(abs(a - b) for a, b in zip(audio_before, audio_after, strict=True)) < .0001
    # Split inside a fade, trim its right-hand fragment, then untrim it. The
    # envelope stays anchored to retained content and the exported pixels match.
    select(client, track, .5, .5)
    client.send("Split")
    divided = model(client)
    parts = [row for row in divided["video"] if row["track"] == track]
    right = parts[1]
    for _ in range(2):
        client.send(f'SetVideoClipBounds: Track={track} Id="{right["id"]}" Start=0.75 End=4')
        client.send(f'SetVideoClipBounds: Track={track} Id="{right["id"]}" Start=0.5 End=4')
        assert model(client) == divided
    client.file("ExportVideoMix", output, 'Overwrite=1 Preset="h264" FrameRate=30 Quality=18')
    after_split = pixels(output)
    assert first is not None
    assert max(abs(a - b) for a, b in zip(first, after_split, strict=True)) < 10
    before_nested = model(client)
    select(client, track, 0, 4)
    client.send("JoinVideoClips")
    joined = next(row for row in video_rows(client) if row["track"] == track)
    client.send(f'SetVideoClipFades: Track={track} Id="{joined["id"]}" In=2 Out=0')
    nested_output = root / "_nested-fades.mp4"
    client.file("ExportVideoMix", nested_output, 'Preset="h264" FrameRate=30 Quality=18')
    nested = pixels(nested_output)
    red, green, blue = nested[45:48]  # Child .5 times parent .25 at half a second.
    assert abs(red - 255 * .125) < 18 and green < 15 and abs(blue - 255 * .875) < 18, (red, green, blue)
    select(client, track, 0, 4)
    client.send("SeparateVideoClips")
    separated = model(client)
    separated_output = root / "_separated-fades.mp4"
    client.file("ExportVideoMix", separated_output, 'Preset="h264" FrameRate=30 Quality=18')
    assert max(abs(a - b) for a, b in zip(nested, pixels(separated_output), strict=True)) < 12
    for _ in range(3):
        client.send("Undo")
    assert model(client) == before_nested
    for _ in range(3):
        client.send("Redo")
    assert model(client) == separated
    saved = model(client)
    save_reopen(client, root / "_video-fades.aup3")
    assert model(client) == saved
    client.file("ExportVideoMix", root / "_reopened-fades.mp4", 'Preset="h264" FrameRate=30 Quality=18')
    assert max(abs(a - b) for a, b in zip(nested, pixels(root / "_reopened-fades.mp4"), strict=True)) < 12
    # A late, slowed clip with non-zero container timestamps must retain the
    # same source frame underneath its fade. This catches seek/PTS phase errors
    # that a constant-colour fixture cannot expose.
    late = root / "_late-clock.mp4"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-n", "-f", "lavfi", "-i",
                 "testsrc2=s=160x100:r=24000/1001:d=4", "-c:v", "libx264", "-preset", "ultrafast",
                 "-crf", "0", "-output_ts_offset", "2", str(late)], timeout=30)
    client.file("ImportVideo", late)
    late_row = next(row for row in video_rows(client) if Path(row["source"]) == late)
    late_track, late_id = late_row["track"], late_row["id"]
    client.send(f"SetClipProperties: Track={late_track} At=1 Start=17 Speed=0.5")
    client.send(f'SetVideoClipFades: Track={late_track} Id="{late_id}" In=1 Out=1')
    timed = model(client)
    client.file("ExportVideoMix", root / "_late-export.mp4", 'Overwrite=1 Preset="h264" FrameRate=30 Quality=18')
    def pixel_at(path, at):
        return run_process([ffmpeg, "-v", "error", "-ss", str(at), "-i", str(path), "-frames:v", "1",
                            "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
    expected = pixel_at(late, .25)
    actual = pixel_at(root / "_late-export.mp4", 17.5)
    assert len(actual) == len(expected) == 3
    assert max(abs(a - .5 * b) for a, b in zip(actual, expected, strict=True)) < 18, (actual, expected)
    # Missing required footage rejects a new effect before publishing history.
    late.rename(root / "_offline-late.mp4")
    try:
        client.send(f'SetVideoClipFades: Track={late_track} Id="{late_id}" In=0.75', success=False)
        assert model(client) == timed
    finally:
        (root / "_offline-late.mp4").rename(late)
    save_reopen(client, root / "_video-fades.aup3", save_existing=True)
    assert model(client) == timed
    state = read_audio_project_state(client.data)
    select_audio_project_tracks(client.send, client.data)
    client.send("SelectTime: Start=0 End=5")
    client.file("Export2", root / "_fades-mix.wav", "NumChannels=2")
    client.send("Save")
    shutil.copyfile(root / "_video-fades.aup3", root / "_fades-consumer.aup3")
    (root / "_compatibility-expected.json").write_text(json.dumps([
        {"project": "_fades-consumer.aup3", "state": state, "audio": "_fades-mix.wav"}]), encoding="utf-8")
    client.send("Save")


def video_rows(client):
    return client.data("GetVideoClips")["clips"]


def select(client, track, start, end):
    client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={start} End={end}")


def model(client):
    return {"audio": audio_rows(client), "video": video_rows(client)}


def assert_bounds(rows, expected):
    assert len(rows) == len(expected), (rows, expected)
    for row, (start, end) in zip(rows, expected, strict=True):
        assert abs(row["start"] - start) < 0.0001, (row, start)
        assert abs(row["end"] - end) < 0.0001, (row, end)


def check_delete_gap(client, root, ffmpeg):
    """Delete removes content in place for sound and picture; ripple is explicit."""
    client.file("ImportVideo", root / "source.mp4")
    original = model(client)
    end = original["video"][0]["end"]
    audio_end = original["audio"][0]["end"]
    before = selected_audio(client, root, "_delete-before.wav", 1, 0, end, ffmpeg)
    for repeat in range(2):
        client.send("SelectAll")
        client.send("SelectTime: Start=2 End=3")
        client.send("Delete")
        after = model(client)
        assert_bounds(after["video"], [(0, 2), (3, end)])
        assert_bounds(after["audio"], [(0, 2), (3, audio_end)])
        assert abs(after["video"][1]["sourceStart"] - 3) < 0.0001
        actual = selected_audio(client, root, f"_delete-after-{repeat}.wav", 1, 0, end, ffmpeg)
        assert max(abs(sample) for sample in actual[16400:23600]) < 0.00001
        assert max(abs(a - b) for a, b in zip(actual[25000:40000], before[25000:40000], strict=True)) < 0.0001
        client.send("Undo")
        assert model(client) == original
        client.send("Redo")
        assert model(client) == after
        client.send("Undo")
    # Deletion covering the leading portion keeps the remaining clip at its
    # absolute timestamp. Repeating the empty-range effect cannot move it.
    client.send("SelectAll")
    client.send("SelectTime: Start=0 End=3")
    client.send("Delete")
    assert_bounds(video_rows(client), [(3, end)])
    assert_bounds(audio_rows(client), [(3, audio_end)])
    for _ in range(2):
        client.send("SelectAll")
        client.send("SelectTime: Start=0 End=3")
        client.send("Delete")
        assert_bounds(video_rows(client), [(3, end)])
    saved = model(client)
    save_reopen(client, root / "_delete-gap.aup3")
    assert model(client) == saved
    # The separately named operation preserves Audacity's original ripple
    # behavior for users intentionally closing time on the selected tracks.
    client.send("SelectAll")
    client.send("SelectTime: Start=3.25 End=3.5")
    client.send("RippleDelete")
    assert abs(video_rows(client)[-1]["end"] - (end - 0.25)) < 0.0001
    assert abs(audio_rows(client)[-1]["end"] - (audio_end - 0.25)) < 0.0001
    client.send("Undo")
    assert model(client) == saved
    client.send("Save")


def check_clip_properties(client, root):
    """Precise linked timing, collisions, no-ops, locks and cancelled dialogs."""
    client.file("ImportVideo", root / "source.mp4")
    before = model(client)
    original = client.data("GetClipProperties: Track=0 At=1")
    duration = original["end"] - original["start"]
    for _ in range(2):
        client.send("SetClipProperties: Track=0 At=1 Start=3 Speed=2")
        value = client.data("GetClipProperties: Track=0 At=4")
        assert value == {"start": 3.0, "end": 3 + duration / 2, "speed": 2.0, "locked": False}, value
        soundtrack = client.data("GetClipProperties: Track=1 At=4")
        assert soundtrack["start"] == 3 and soundtrack["speed"] == 2, soundtrack
        changed = model(client)
        client.send("SetClipProperties: Track=0 At=4 Start=3 Speed=2")
        assert model(client) == changed
        client.send("Undo")
        assert model(client) == before, "An unchanged properties request created an Undo step."
        client.send("Redo")
        assert model(client) == changed
        client.send("Undo")
    # End and Duration mean retime; both operate on retained source span and
    # must agree when supplied together. Invalid requests have zero effects.
    for arguments in ("End=0", "Duration=0", "Speed=0", "Duration=4 End=5", "Duration=4 Speed=3"):
        client.send(f"SetClipProperties: Track=0 At=1 {arguments}", success=False)
        assert model(client) == before
    client.send(f"SetClipProperties: Track=1 At=1 Start=2 Duration={duration / 2}")
    assert abs(video_rows(client)[0]["start"] - 2) < 0.0001
    assert abs(video_rows(client)[0]["playbackRate"] - 2) < 0.01
    client.send("Undo")
    assert model(client) == before
    # A linked partner's lock blocks the entire edit before either medium is
    # changed. Explicit unlock-and-edit on the locked anchor remains convenient.
    client.send("SetClipProperties: Track=1 At=1 Locked=1")
    locked = model(client)
    client.send("SetClipProperties: Track=0 At=1 Start=2", success=False)
    assert model(client) == locked
    saved = client.data("GetClipProperties: Track=1 At=1")
    save_reopen(client, root / "_clip-properties.aup3")
    assert client.data("GetClipProperties: Track=1 At=1") == saved
    client.send("SetClipProperties: Track=1 At=1 Locked=0 Start=3 Speed=2")
    assert client.data("GetClipProperties: Track=1 At=4")["start"] == 3
    client.send("Undo")
    client.send("SetClipProperties: Track=1 At=1 Locked=0")
    # The normal dialog shares the command's behavior. Cancel is checked twice
    # before any visible timing effect, including a partially typed value.
    select(client, 1, 0.5, 0.5)
    stable = model(client)
    for text in ("7", "-"):
        def cancel(windows, dialog, value=text):
            windows.text(windows.field(dialog, "Start (seconds)"), value)
            windows.click(windows.button(dialog, "Cancel"))
        with_native_dialog(lambda: client.send("AudioClipProperties"), "Clip Properties", cancel, needs_focus=False)
        assert model(client) == stable
    def apply(windows, dialog):
        windows.text(windows.field(dialog, "Start (seconds)"), "5")
        windows.text(windows.field(dialog, "Speed (%)"), "200")
        windows.click(windows.button(dialog, "OK"))
    with_native_dialog(lambda: client.send("AudioClipProperties"), "Clip Properties", apply, needs_focus=False)
    assert math.isclose(video_rows(client)[0]["start"], 5, abs_tol=0.0001)
    assert math.isclose(video_rows(client)[0]["playbackRate"], 2, abs_tol=0.0001)
    client.send("Undo")
    assert model(client) == stable
    # Closing a leading gap moves linked picture and sound together, while a
    # repeated request at zero is an exact no-op.
    client.send("SetClipProperties: Track=0 At=1 Start=4")
    placed = model(client)
    client.send("CloseClipGap: Track=0 At=5")
    assert video_rows(client)[0]["start"] == 0 and audio_rows(client)[0]["start"] == 0
    client.send("CloseClipGap: Track=0 At=1")
    client.send("Undo")
    assert model(client) == placed
    client.send("Redo")
    assert video_rows(client)[0]["start"] == 0
    save_reopen(client, root / "_clip-properties.aup3", save_existing=True)
    client.send("Save")


def check_clip_snapping(client, root):
    """Linked picture/sound snap to the cursor; Alt and Escape stay predictable."""
    from run_fork_test_harness import painted_video_tracks
    client.file("ImportVideo", root / "source.mp4")
    client.send("SetClipProperties: Track=0 At=1 Start=4 Speed=2")
    client.send("FitInWindow")
    client.send("FitV")
    select(client, 0, 2.5, 2.5)
    before = model(client)
    painted = painted_video_tracks(client)
    row = next(row for row in painted["videoTrackRows"] if row["index"] == 0)
    pixel_seconds = (painted["viewEnd"] - painted["viewStart"]) / row["dataWidth"]
    destination = 3.5 + 2 * pixel_seconds
    for _ in range(2):
        select(client, 0, 2.5, 2.5)
        operate_owned_video_track(client, 5, destination, track=0)
        snapped = model(client)
        assert abs(snapped["video"][0]["start"] - 2.5) < .0001, snapped
        assert abs(snapped["audio"][0]["start"] - 2.5) < .0001, snapped
        client.send("Undo")
        assert model(client) == before
        select(client, 0, 2.5, 2.5)
        operate_owned_video_track(client, 5, destination, track=0, alt=True)
        free = model(client)
        assert abs(free["video"][0]["start"] - 2.5) > .005, free
        assert abs(free["video"][0]["start"] - free["audio"][0]["start"]) < .0001
        client.send("Undo")
        assert model(client) == before
        select(client, 0, 2.5, 2.5)
        operate_owned_video_track(client, 5, destination, track=0, alt=True, cancel=True)
        assert model(client) == before
    # A lock on either member blocks the whole drag before a timestamp changes.
    client.send("SetClipProperties: Track=1 At=5 Locked=1")
    locked = model(client)
    assert client.data("GetClipProperties: Track=1 At=5")["locked"]
    try:
        operate_owned_video_track(client, 5, 3.5, track=0)
    except RuntimeError as error:
        # This negative gesture is expected to be refused before capture. Any
        # other input failure still fails the case; a captured drag is a bug.
        assert "The owned pane did not capture held input" in str(error), error
    else:
        raise AssertionError("A linked timing lock unexpectedly allowed drag capture.")
    assert model(client) == locked
    client.send("SelectAll")
    client.send("SelectTime: Start=4.25 End=4.75")
    client.send("Copy")
    # Paste's normal single-selected-track rule maps the second clipboard row
    # to an unselected audio destination. Its future locked clip must still
    # prevent a partial paste into the preceding unlocked video track.
    select(client, 0, 0, 0)
    with_native_dialog(lambda: client.send("Paste"), "Locked Clip",
        lambda windows, dialog: windows.click(windows.button(dialog, "OK")), needs_focus=False)
    assert model(client) == locked
    select(client, 1, 4.5, 5.5)
    selection = client.data("GetInfo: Type=Selection Format=JSON")
    for action in ("Delete", "Delete", "Cut", "Paste"):
        with_native_dialog(lambda command=action: client.send(command), "Locked Clip",
            lambda windows, dialog: windows.click(windows.button(dialog, "OK")), needs_focus=False)
        assert model(client) == locked
        assert client.data("GetInfo: Type=Selection Format=JSON") == selection
    save_reopen(client, root / "_clip-snapping.aup3")
    assert model(client) == locked
    client.send("Save")


def check_visual_crop(client, root):
    """Drag the displayed picture, cancel/commit, then close without a sleep."""
    client.file("ImportVideo", root / "source.mp4")
    client.send("FitInWindow")
    client.send("FitV")
    original = model(client)
    def open_menu():
        operate_owned_video_track(client, 1, track=0, gesture="context", menu_path=("Crop Picture...",))
    for commit in (False, True):
        def drag(windows, dialog, accept=commit):
            canvases = [handle for handle in windows.windows(dialog) if windows.caption(handle) == "Picture crop handles"]
            assert len(canvases) == 1, [(windows.kind(h), windows.caption(h)) for h in windows.windows(dialog)]
            canvas = canvases[0]
            left, top, right, bottom = windows.bounds(canvas)
            scale = min((right - left - 24) / 16, (bottom - top - 24) / 9)
            width = 16 * scale
            x = round((right - left - width) / 2)
            y = round((bottom - top) / 2)
            def pointer(kind, px, py, held=0):
                windows.message(canvas, kind, held, (int(py) << 16) | (int(px) & 0xFFFF))
            pointer(0x201, x, y, 1)
            pointer(0x200, x + round(width / 4), y, 1)
            pointer(0x202, x + round(width / 4), y)
            percent = float(windows.control_text(windows.field(dialog, "Left")))
            assert abs(percent - 25) < 1, percent
            windows.click(windows.button(dialog, "OK" if accept else "Cancel"))
        with_native_dialog(open_menu, "Crop Video Picture", drag, menu_gesture=True)
        if not commit:
            assert model(client) == original
        else:
            changed = model(client)
            assert abs(changed["video"][0]["crop"]["left"] - .25) < .01
            assert changed["audio"] == original["audio"]
            client.send("Undo")
            assert model(client) == original
            client.send("Redo")
            assert model(client) == changed
    # A fresh Save/Close sent immediately after native Apply must wait until
    # the dialog's stack and parent relationship have been released. This is
    # the boundary that previously attempted to delete a stack-owned window.
    save_reopen(client, root / "_visual-crop.aup3")
    assert model(client) == changed
    client.send("Save")


def check_collect_media(client, root):
    """Collect a live edit, repeat, move it offline and retain native audio data."""
    other = root / "_other"
    other.mkdir()
    second = other / "source.mp4"
    shutil.copyfile(root / "source.mp4", second)
    # A legal MP4 free box gives equal basenames distinct content identities.
    with second.open("ab") as stream:
        stream.write(struct.pack(">I4s", 16, b"free") + b"neutral!")
    client.file("ImportVideo", root / "source.mp4")
    client.file("ImportVideo", second)
    client.send("SelectAll")
    client.send("SelectTime: Start=2 End=3")
    client.send("Delete")
    client.send("SetClipProperties: Track=0 At=1 Locked=1")
    save_reopen(client, root / "_source-project.aup3")
    before = model(client)
    collection = root / "_collected"
    foreign_lock = root / ("_" + collection.name + ".collection.lock")
    foreign_lock.write_bytes(b"unrelated file")
    client.file("CollectProjectMedia", collection, success=False)
    assert foreign_lock.read_bytes() == b"unrelated file" and not collection.exists()
    foreign_lock.unlink()
    client.file("CollectProjectMedia", collection)
    assert model(client) == before
    assert (collection / "Project.aup3").is_file()
    media = list((collection / "Media").iterdir())
    assert len(media) == 2, media
    def identities(directory):
        return {path.relative_to(directory).as_posix(): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
                for path in directory.rglob("*") if path.is_file()}
    accepted = identities(collection)
    client.file("CollectProjectMedia", collection)
    assert identities(collection) == accepted, "A repeated collection rewrote accepted output."
    client.send("SetClipProperties: Track=0 At=1 Locked=0")
    client.file("CollectProjectMedia", collection, success=False)
    assert identities(collection) == accepted
    client.send("Undo")
    assert model(client) == before
    client.send("Save")
    client.send("Close")
    moved = root / "_moved-collection"
    collection.rename(moved)
    (root / "source.mp4").rename(root / "_offline-source.mp4")
    second.rename(other / "_offline-source.mp4")
    client.file("OpenProject2", moved / "Project.aup3")
    restored = model(client)
    assert restored["audio"] == before["audio"]
    assert [(row["start"], row["end"], row["sourceStart"]) for row in restored["video"]] == [
        (row["start"], row["end"], row["sourceStart"]) for row in before["video"]]
    assert all(Path(row["source"]).parent == moved / "Media" for row in restored["video"]), restored
    assert client.data("GetClipProperties: Track=0 At=1")["locked"]
    client.send("Save")
    client.send("Close")
    client.file("OpenProject2", moved / "Project.aup3")
    assert model(client) == restored, "Saving a collected copy lost its portable media references."
    # A normal backup elsewhere still uses a usable absolute fallback when no
    # matching Media directory accompanies it.
    backup = root / "_ordinary-backup.aup3"
    client.file("SaveCopy", backup)
    client.send("Close")
    client.file("OpenProject2", backup)
    assert model(client) == restored
    client.send("Save")
    # The stock consumer checks audio/labels using the collected AUP3 itself;
    # only AuTopsy's optional video extension is skipped.
    state = read_audio_project_state(client.data)
    select_audio_project_tracks(client.send, client.data)
    client.send("SelectTime: Start=0 End=5")
    client.file("Export2", root / "_collected-mix.wav", "NumChannels=2")
    # Consumer fixtures deliberately remain direct children of the case root.
    # This is a byte-for-byte copy of the closed collected database, not another
    # save through the producer whose compatibility we are checking.
    shutil.copyfile(moved / "Project.aup3", root / "_collected-consumer.aup3")
    (root / "_compatibility-expected.json").write_text(json.dumps([
        {"project": "_collected-consumer.aup3", "state": state,
         "audio": "_collected-mix.wav"}]), encoding="utf-8")
