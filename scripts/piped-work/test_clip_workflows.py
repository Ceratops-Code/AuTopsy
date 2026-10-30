"""Public-command and menu checks for export scope, clip labels and ripple edits.

Generated fixtures and private application sessions belong to the common native
harness. Outputs stay inside its bounded case directory and are removed with
that directory. No personal media, microphone or installed editor is used.
"""
import array
import json
import math

from run_fork_test_harness import operate_owned_video_track, run_process, save_reopen
from test_beginner_edits import assert_bounds, model, select, video_rows
from test_clip_actions import with_native_dialog

CLIP_WORKFLOW_CASES = ("video-range-export", "video-clip-appearance", "linked-ripple-delete")


def pixel(ffmpeg, path, at, size="1:1"):
    return run_process([ffmpeg, "-v", "error", "-ss", str(at), "-i", str(path),
                        "-frames:v", "1", "-vf", f"scale={size}", "-pix_fmt", "rgb24",
                        "-f", "rawvideo", "pipe:1"], timeout=30)


def assert_unchanged(client, expected, selection):
    assert model(client) == expected
    assert client.data("GetInfo: Type=Selection Format=JSON") == selection


def check_video_range_export(client, root, ffmpeg, ffprobe):
    """Nonzero selection, all audible tracks, gaps, PIP/fades and atomic output."""
    client.file("ImportVideo", root / "source.mp4")
    extra = root / "_range-late-tone.wav"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-n", "-f", "lavfi", "-i",
                 r"aevalsrc=if(between(t\,2\,4)\,0.08*sin(2*PI*880*t)\,0):s=48000:d=4",
                 str(extra)], timeout=30)
    client.file("Import2", extra)
    select(client, 0, 1, 3)
    original = model(client)
    selection = client.data("GetInfo: Type=Selection Format=JSON")
    settings = "SelectedOnly=1 Preset=h264 Width=160 Height=90 FrameRate=30 Quality=12"
    for repeat in range(2):
        output = root / f"_selected-range-{repeat}.mp4"
        client.file("ExportVideoMix", output, settings)
        assert_unchanged(client, original, selection)
        metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-of", "json", str(output)], timeout=30))
        assert abs(float(metadata["format"]["duration"]) - 2) < .08, metadata
        red, blue = pixel(ffmpeg, output, .25), pixel(ffmpeg, output, 1.5)
        assert red[0] > 180 and max(red[1:]) < 35, red
        assert blue[2] > 180 and max(blue[:2]) < 35, blue
        pcm = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(output), "-map", "0:a:0",
            "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
        for start, frequency, minimum in ((.2, 440, .01), (1.2, 440, .01), (1.2, 880, .01)):
            samples = pcm[round(start * 8000):round((start + .5) * 8000)]
            magnitude = abs(sum(value * complex(math.cos(2 * math.pi * frequency * i / 8000),
                math.sin(2 * math.pi * frequency * i / 8000)) for i, value in enumerate(samples))) / len(samples)
            assert magnitude > minimum, (start, frequency, magnitude)
        accepted = output.read_bytes()
        client.file("ExportVideoMix", output, settings, success=False)
        assert output.read_bytes() == accepted
        client.file("ExportVideoMix", output, settings + " Overwrite=1")
        assert_unchanged(client, original, selection)
    # Invalid scope fails before creating a destination or changing history.
    for repeat in range(2):
        select(client, 0, 2, 2)
        destination = root / f"_empty-range-{repeat}.mp4"
        client.file("ExportVideoMix", destination, settings, success=False)
        assert not destination.exists()
        assert model(client) == original
    select(client, 0, 1.5, 2.5)
    client.send("Delete")
    select(client, 0, 1, 3)
    gap = root / "_selected-gap.mp4"
    client.file("ExportVideoMix", gap, settings)
    assert max(pixel(ffmpeg, gap, .8)) < 20
    assert pixel(ffmpeg, gap, .2)[0] > 180 and pixel(ffmpeg, gap, 1.8)[2] > 180
    client.send("Undo")
    assert model(client) == original
    # Composition's parent IDs and fade clocks must survive time rebasing.
    client.file("ImportVideo", root / "portrait.mp4")
    rows = video_rows(client)
    child = next(row for row in rows if row["source"].endswith("portrait.mp4"))
    parent = next(row for row in rows if row["source"].endswith("source.mp4"))
    client.send(f'SetVideoEmbedding: Track={child["track"]} Parent={parent["track"]} X=0.6 Y=0.1 Width=0.3 Height=0.7')
    client.send(f'SetVideoClipFades: Track={parent["track"]} Id="{parent["id"]}" In=2')
    whole = root / "_composed-whole.mp4"
    client.file("ExportVideoMix", whole, "Preset=h264 Width=160 Height=90 FrameRate=30 Quality=12")
    select(client, parent["track"], 1, 3)
    chosen = root / "_composed-selection.mp4"
    client.file("ExportVideoMix", chosen, settings)
    for at in (.25, .75, 1.5):
        before, after = pixel(ffmpeg, whole, at + 1, "32:18"), pixel(ffmpeg, chosen, at, "32:18")
        assert len(before) == len(after) == 32 * 18 * 3
        assert sum(abs(a - b) for a, b in zip(before, after, strict=True)) / len(before) < 8
    # A time range beyond picture still exports black and silence, not time zero.
    select(client, parent["track"], 6, 7)
    tail = root / "_selected-tail.mp4"
    client.file("ExportVideoMix", tail, settings)
    assert max(pixel(ffmpeg, tail, .25)) < 20


def check_video_clip_appearance(client, root, ffmpeg):
    """Repeated/cancelled edits, UTF-8, copies, nested pieces and saved metadata."""
    client.file("ImportVideo", root / "source.mp4")
    clip = video_rows(client)[0]
    track, identifier = clip["track"], clip["id"]
    original = model(client)
    name = "\u05e9\u05dc\u05d5\u05dd / \u0422\u0435\u0441\u0442"
    request = f'SetVideoClipAppearance: Track={track} Id="{identifier}" Name="{name}" Colour=16777130'
    for repeat in range(2):
        client.send(request)
        changed = model(client)
        assert changed["video"][0]["name"] == name and changed["video"][0]["colour"] == 16777130
        assert changed["audio"] == original["audio"]
        client.send(request)
        client.send("Undo")
        assert model(client) == original, "Repeated appearance edit added history."
        client.send("Redo")
        assert model(client) == changed
        for invalid in ('Id="999999" Colour=0', f'Id="{identifier}" Colour=-2',
                        f'Id="{identifier}" Colour=16777216'):
            client.send(f"SetVideoClipAppearance: Track={track} {invalid}", success=False)
            assert model(client) == changed
        client.send("Undo")
    client.send(request)
    changed = model(client)
    client.send("FitV")
    client.send("FitInWindow")

    def edit_menu():
        operate_owned_video_track(client, 1, track=track, gesture="context", menu_path=("Clip Name and Colour...",))

    for commit in (False, True):
        def handle(windows, dialog):
            fields = [h for h in windows.windows(dialog) if windows.kind(h) == "Edit"]
            assert len(fields) == 1, fields
            windows.text(fields[0], "Named from clip menu")
            windows.click(windows.button(dialog, "OK" if commit else "Cancel"))
        with_native_dialog(edit_menu, "Video Clip Name and Colour", handle, menu_gesture=True)
        if not commit:
            assert model(client) == changed
        else:
            assert video_rows(client)[0]["name"] == "Named from clip menu"
            client.send("Undo")
            assert model(client) == changed
    select(client, track, 2, 2)
    client.send("Split")
    assert all(row["name"] == name for row in video_rows(client))
    right = video_rows(client)[1]
    client.send(f'SetVideoClipAppearance: Track={track} Id="{right["id"]}" Name="Right part" Colour=255')
    select(client, track, 0, 4)
    client.send("JoinVideoClips")
    assert video_rows(client)[0]["name"] == name
    joined = model(client)
    save_reopen(client, root / "_named-joined.aup3")
    assert model(client) == joined
    client.send("SeparateVideoClips")
    assert [row["name"] for row in video_rows(client)] == [name, "Right part"]
    assert [row["colour"] for row in video_rows(client)] == [16777130, 255]
    select(client, track, .5, 1.5)
    client.send("Copy")
    select(client, track, 6, 6)
    client.send("Paste")
    pasted = video_rows(client)[-1]
    assert pasted["name"] == name and pasted["colour"] == 16777130
    client.send("Undo")
    for repeat in range(2):
        select(client, track, 0, 4)
        output = root / f"_appearance-picture-{repeat}.mp4"
        client.file("ExportVideoMix", output, "SelectedOnly=1 Preset=h264 Width=160 Height=90")
        assert pixel(ffmpeg, output, .5)[0] > 180 and pixel(ffmpeg, output, 3)[2] > 180


def check_linked_ripple_delete(client, root):
    """Audio/video anchors, unselected partners, locks and project-wide scope."""
    client.file("ImportVideo", root / "source.mp4")
    client.file("Import2", root / "tone-b.wav")
    original = model(client)
    video = original["video"][0]
    for repeat in range(2):
        for anchor in (video["track"], 1):
            select(client, anchor, 1, 2)
            client.send("RippleDelete")
            changed = model(client)
            assert_bounds(changed["video"], [(0, 1), (1, 3)])
            partner = [row for row in changed["audio"] if row["track"] == 1]
            assert abs(max(row["end"] for row in partner) - (original["audio"][0]["end"] - 1)) < .0001
            assert [row for row in changed["audio"] if row["track"] == 2] == [
                row for row in original["audio"] if row["track"] == 2]
            client.send("Undo")
            assert model(client) == original
            client.send("Redo")
            assert model(client) == changed
            client.send("Undo")
            assert model(client) == original
            # A later locked partner rejects before either medium is changed.
            client.send("SetClipProperties: Track=1 At=3 Locked=1")
            locked = model(client)
            select(client, anchor, 1, 2)
            selection = client.data("GetInfo: Type=Selection Format=JSON")
            # Native menu dispatch reports that it handled the command; its
            # error response and unchanged model prove the edit was rejected.
            assert "Unlock" in client.send("RippleDelete")
            assert_unchanged(client, locked, selection)
            client.send("Undo")
            assert model(client) == original
    select(client, video["track"], 1, 2)
    client.send("RippleDeleteProject")
    global_result = model(client)
    for track in (1, 2):
        old_end = max(row["end"] for row in original["audio"] if row["track"] == track)
        assert abs(max(row["end"] for row in global_result["audio"] if row["track"] == track) - (old_end - 1)) < .0001
    client.send("Undo")
    assert model(client) == original
    # Ordinary Delete remains a gap edit and never pulls the right piece left.
    select(client, video["track"], 1, 2)
    client.send("Delete")
    assert_bounds(video_rows(client), [(0, 1), (2, 4)])
    client.send("Undo")
    assert model(client) == original
    select(client, 1, 1, 2)
    client.send("RippleDelete")
    saved = model(client)
    save_reopen(client, root / "_linked-ripple.aup3")
    assert model(client) == saved


def check_clip_workflow(client, root, ffmpeg, ffprobe, case):
    if case == "video-range-export":
        check_video_range_export(client, root, ffmpeg, ffprobe)
    elif case == "video-clip-appearance":
        check_video_clip_appearance(client, root, ffmpeg)
    elif case == "linked-ripple-delete":
        check_linked_ripple_delete(client, root)
    else:
        raise ValueError(f"Unknown clip workflow: {case}")
