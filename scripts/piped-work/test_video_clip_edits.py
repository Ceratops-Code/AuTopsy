"""Focused native clip-structure and edge cases, using generated test media."""
import json
import shutil
import time
from pathlib import Path

from run_fork_test_harness import operate_owned_video_track, run_process, save_reopen


def select_clip_range(client, start, end, track=0):
    client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={start} End={end}")


def clip_rows(client):
    return client.data("GetVideoClips")["clips"]


def check_video_clip_structure(client, root, linked):
    """Exercise real commands, repeat boundaries, gestures, Undo and native XML.

    Candidates have one visible history effect. Cancelled gestures exercise the
    pre-effect boundary; fresh commands after save/reopen exercise the completed
    boundary without restoring caller progress. Linked audio stays as samples
    in its original clips, with both source handles and flags intact.
    """
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


