"""Exercise AuTopsy through its public scripting and media interfaces.

CTest supplies an executable and a dedicated work root. Tests own only marked
temporary directories and their child process. The native Portable Settings
name is required by Audacity; a marker protects any unrelated existing profile.
No installed app, user project, source snapshot or fixed upstream layout is used.
Visible tests use DISPLAY2 when connected, otherwise another display except
DISPLAY1 if several are connected. The scripting module constrains windows
before showing them; the runner refuses modules without that contract.
"""

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

from run_fork_test_harness import (
    ScriptClient,
    assert_owned_test_display,
    capture_owned_project,
    check_behavior_worker_lifecycle,
    check_neutral_preview_fixture,
    check_test_display_selection,
    connected_test_displays,
    drag_owned_video_clip,
    executable_version,
    painted_video_tracks,
    ready_video_preview,
    require_test_display_guard,
    run_behavior_worker,
    run_process,
    save_reopen,
    select_test_display,
    startup_window_messages,
    write_neutral_preview_video,
    write_tone,
)


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
        (x["name"], x["start"], x["end"]) for x in original
    ]
    save_reopen(client, root / "tracks.aup3")
    reopened = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [(x["name"], x["channels"]) for x in reopened] == [
        (x["name"], x["channels"]) for x in tracks
    ]



def check_media(client, root, ffmpeg, ffprobe):
    """Exercise saved links, clip-local flags, relinking, frames and encoders."""
    source = root / "source.mp4"
    client.file("ImportVideo", source)
    linked = client.data("GetLinkedMediaClips")
    assert len(linked) == 2 and linked[0]["group"], linked
    assert linked[0]["group"] == linked[1]["group"], linked
    client.send("Undo")
    assert client.data("GetLinkedMediaClips") == []
    client.send("Redo")
    assert client.data("GetLinkedMediaClips") == linked
    client.send("SelectAll")
    client.send("SelectTime: Start=0 End=4.02")
    client.send("Copy")
    pasted_groups = {linked[0]["group"]}
    for pasted_start in (6, 12):
        client.send(f"SelectTime: Start={pasted_start} End={pasted_start}")
        client.send("Paste")
        rows = client.data("GetLinkedMediaClips")
        copies = [row for row in rows if abs(row["start"] - pasted_start) < 1e-7]
        assert len(copies) == 2, rows
        group = {row["group"] for row in copies}
        assert len(group) == 1 and not group.intersection(pasted_groups), rows
        pasted_groups.update(group)
    client.send("MoveVideoClip: Track=0 Id=2 Start=7")
    rows = client.data("GetLinkedMediaClips")
    assert sorted(round(row["start"], 5) for row in rows) == [0, 0, 7, 7, 12, 12], rows
    for _ in range(3):
        client.send("Undo")
    assert client.data("GetLinkedMediaClips") == linked
    client.send("SelectAll")
    client.send("SelectTime: Start=0 End=4.02")
    client.send("Duplicate")
    duplicated = client.data("GetLinkedMediaClips")
    copies = [row for row in duplicated if row["track"] >= 2]
    assert len(copies) == 2 and len({row["group"] for row in copies}) == 1, duplicated
    assert copies[0]["group"] != linked[0]["group"], duplicated
    client.send("MoveVideoClip: Track=2 Id=1 Start=6")
    rows = client.data("GetLinkedMediaClips")
    assert all(abs(row["start"] - (6 if row["track"] >= 2 else 0)) < 1e-7 for row in rows), rows
    client.send("Undo")
    client.send("Undo")
    assert client.data("GetLinkedMediaClips") == linked
    for track, text, marker in ((0, "Picture flag", "a" * 32), (1, "Sound flag", "b" * 32)):
        command = f'AddClipMarker: Track={track} At=1 Text="{text}" Id="{marker}"'
        client.send(command)
        flags = client.data("GetClipMarkers")
        client.send(command)
        assert client.data("GetClipMarkers") == flags
    client.send('AddClipMarker: Track=0 At=1 Text=""', success=False)
    assert client.data("GetClipMarkers") == flags
    client.send("MoveVideoClip: Track=0 Id=1 Start=2")
    moved = client.data("GetLinkedMediaClips")
    assert all(abs(row["start"] - 2) < 1e-7 for row in moved), moved
    assert all(abs(flag["time"] - 3) < 1e-7 for flag in client.data("GetClipMarkers"))
    client.send("MoveVideoClip: Track=0 Id=1 Start=2")
    assert client.data("GetLinkedMediaClips") == moved
    client.send("MoveAudioClipGroup: Track=1 At=3 Start=0")
    assert client.data("GetLinkedMediaClips") == linked
    drag_owned_video_clip(client, 4, 3.5, track=0)
    trimmed = client.data("GetLinkedMediaClips")
    assert all(abs(row["end"] - 3.5) < 0.04 for row in trimmed), trimmed
    drag_owned_video_clip(client, 3.5, 3, track=0, cancel=True)
    assert client.data("GetLinkedMediaClips") == trimmed
    drag_owned_video_clip(client, 3.5, 3.5, track=0)
    assert client.data("GetLinkedMediaClips") == trimmed
    client.send("Undo")
    assert client.data("GetLinkedMediaClips") == linked
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=2 End=2")
    client.send("Split")
    split = client.data("GetLinkedMediaClips")
    assert len(split) == 4, split
    for index in (0, 1):
        members = [row for row in split if row["clip"] == index]
        assert {row["kind"] for row in members} == {"audio", "video"}, split
        assert len({row["group"] for row in members}) == 1, split
    assert split[0]["group"] != split[1]["group"], split
    client.send("Split")
    assert client.data("GetLinkedMediaClips") == split
    client.send("MoveVideoClip: Track=0 Id=2 Start=3")
    right = client.data("GetLinkedMediaClips")
    assert all(abs(row["start"] - (3 if row["clip"] else 0)) < 1e-7 for row in right), right
    client.send("MoveVideoClip: Track=0 Id=2 Start=0", success=False)
    assert client.data("GetLinkedMediaClips") == right
    visible = [flag for flag in client.data("GetClipMarkers") if flag["visible"]]
    assert len(visible) == 2 and all(flag["time"] == 1 for flag in visible), visible
    client.send("Undo")
    assert client.data("GetLinkedMediaClips") == split
    for track, start, end in ((0, 1, 3), (1, 0.5, 3.5)):
        client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
        client.send(f"SelectTime: Start={start} End={end}")
        client.send("Trim")
        bounds = client.data("GetLinkedMediaClips")
        assert all(abs(row["start"] - (2 if row["clip"] else start)) < 0.04 for row in bounds), bounds
        assert all(abs(row["end"] - (end if row["clip"] else 2)) < 0.04 for row in bounds), bounds
        client.send("Undo")
        assert client.data("GetLinkedMediaClips") == split
    save_reopen(client, root / "linked-flags.aup3")
    assert client.data("GetLinkedMediaClips") == split
    assert [flag for flag in client.data("GetClipMarkers") if flag["visible"]] == visible
    client.send('RemoveClipMarker: Track=0 Clip=0 Id="' + "a" * 32 + '"')
    removed = client.data("GetClipMarkers")
    client.send('RemoveClipMarker: Track=0 Clip=0 Id="' + "a" * 32 + '"')
    assert client.data("GetClipMarkers") == removed
    client.send("Undo")
    assert [flag for flag in client.data("GetClipMarkers") if flag["visible"]] == visible
    client.send('UnlinkMediaClips: Clips="0:0"')
    unlinked = client.data("GetLinkedMediaClips")
    assert all(not row["group"] for row in unlinked if row["clip"] == 0), unlinked
    client.send('UnlinkMediaClips: Clips="0:0"')
    assert client.data("GetLinkedMediaClips") == unlinked
    client.send("Undo")
    assert client.data("GetLinkedMediaClips") == split
    clips = client.data("GetVideoClips")
    client.file("RelinkVideoMedia", root / "other-source.mp4",
                'OldPath=' + json.dumps(source.as_posix()), success=False)
    assert client.data("GetVideoClips") == clips
    replacement = root / "relocated.mp4"
    shutil.copyfile(source, replacement)
    relink = 'OldPath=' + json.dumps(source.as_posix())
    client.file("RelinkVideoMedia", replacement, relink)
    relinked = client.data("GetVideoClips")
    assert all(Path(clip["source"]) == replacement for clip in relinked["clips"]), relinked
    client.file("RelinkVideoMedia", replacement, relink)
    assert client.data("GetVideoClips") == relinked
    assert client.data("GetLinkedMediaClips") == split
    client.send("Undo")
    assert client.data("GetVideoClips") == clips
    client.send("Redo")
    save_reopen(client, root / "relinked.aup3")
    assert client.data("GetVideoClips") == relinked
    client.send("SelectTime: Start=0 End=0")
    for step in (1, 2):
        client.send("StepVideoFrame: Direction=1")
        deadline = time.monotonic() + 5
        while True:
            info = client.data("GetVideoPreviewInfo")
            if info["ready"] and info["originalFrames"] and abs(info["position"] - step / 30) < 0.02:
                break
            if time.monotonic() >= deadline:
                raise AssertionError(info)
            time.sleep(0.05)
        assert abs(info["audioPosition"] - step / 30) < 1e-7, info
    client.send("StepVideoFrame: Direction=-1")
    assert abs(client.data("GetVideoPreviewInfo")["audioPosition"] - 1 / 30) < 1e-7
    client.send("StepVideoFrame: Direction=0", success=False)
    assert abs(client.data("GetVideoPreviewInfo")["audioPosition"] - 1 / 30) < 1e-7
    for preset, codec in (("h264", "h264"), ("hevc", "hevc"), ("av1", "av1")):
        output = root / (preset + ".mp4")
        options = f'Preset="{preset}" Width=128 Height=72 FrameRate=15 Quality=28'
        client.file("ExportVideoMix", output, options)
        streams = json.loads(run_process(
            [ffprobe, "-v", "error", "-show_streams", "-of", "json", str(output)],
            text=True, timeout=30))["streams"]
        assert streams[0]["codec_name"] == codec, streams
        assert (streams[0]["width"], streams[0]["height"]) == (128, 72), streams
        assert streams[0]["avg_frame_rate"] == "15/1", streams
        previous = output.read_bytes()
        client.file("ExportVideoMix", output, 'Overwrite=1 Width=127 Height=72', success=False)
        assert output.read_bytes() == previous
        client.file("ExportVideoMix", output, options + " Overwrite=1")
        assert output.is_file() and output.stat().st_size > 1000
    assert not list(root.glob("_autopsy-video-export-*"))


def decoded_frame_hashes(ffmpeg, path):
    data = run_process(
        [
            ffmpeg,
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:v:0",
            "-f",
            "framemd5",
            "-",
        ],
        text=True,
        timeout=60,
    )
    return [
        line.rsplit(",", 1)[-1].strip()
        for line in data.splitlines()
        if line and not line.startswith("#")
    ]


def check_video(client, root, ffmpeg, ffprobe):
    source = root / "source.mp4"
    source_bytes = hashlib.sha256(source.read_bytes()).hexdigest()
    import_children = client.data("GetVideoPreviewInfo")["importMenu"]
    assert import_children[0]["id"] == "ImportVideo", import_children
    video_import = next(row for row in import_children if row["id"] == "ImportVideo")
    assert video_import["label"].rstrip(".") == "Video", import_children
    client.file("ImportVideo", source)
    native_tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    assert [track["kind"] for track in native_tracks] == ["video", "wave"], (
        native_tracks
    )
    strip = painted_video_tracks(client)
    assert strip["videoTrackVisible"] and strip["videoTrackThumbnails"], strip
    assert (
        strip["videoTrackLeftOffset"] > 0 and strip["viewEnd"] > strip["viewStart"]
    ), strip
    client.send("ZoomIn")
    assert client.data("GetVideoPreviewInfo")["viewEnd"] < strip["viewEnd"]
    client.send("ZoomOut")
    client.send('UnlinkMediaClips: Clips="0:0"')
    original = client.data("GetVideoClips")
    assert len(original["clips"]) == 1 and abs(original["end"] - 4) < 0.04
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
    assert (
        abs(delta - 1)
        <= (view["viewEnd"] - view["viewStart"]) / view["videoTrackDataWidth"]
    ), (together, moved_tracks)
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
    info = json.loads(
        run_process(
            [
                ffprobe,
                "-v",
                "error",
                "-show_streams",
                "-show_format",
                "-of",
                "json",
                str(edited),
            ],
            text=True,
            timeout=30,
        )
    )
    assert abs(float(info["format"]["duration"]) - 3) < 0.08, info
    assert [s["codec_name"] for s in info["streams"]] == ["h264", "aac"], info
    for when, blue in ((0.5, False), (1.5, True)):
        pixels = run_process(
            [
                ffmpeg,
                "-v",
                "error",
                "-ss",
                str(when),
                "-i",
                str(edited),
                "-frames:v",
                "1",
                "-vf",
                "scale=1:1",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "-",
            ],
            timeout=30,
        )
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
    samples = run_process(
        [ffmpeg, "-v", "error", "-i", str(silent), "-map", "0:a:0", "-f", "s16le", "-"],
        timeout=30,
    )
    assert len(samples) > 48000 and not any(samples)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_bytes
    assert not list(root.glob("_autopsy-video-export-*"))
    client.send("Save")
    check_video_tracks(client, root, ffmpeg, ffprobe)


def check_video_tracks(client, root, ffmpeg, ffprobe):
    """Exercise native track ownership, mixed sources and shared editing actions."""
    other = root / "other-source.mp4"
    client.file("ImportVideo", other)
    assert [
        track["kind"] for track in client.data("GetInfo: Type=Tracks Format=JSON")
    ] == ["video", "video"]
    layers = root / "layered.mp4"
    client.file("ExportVideoMix", layers)
    streams = json.loads(
        run_process(
            [ffprobe, "-v", "error", "-show_streams", "-of", "json", str(layers)],
            text=True,
            timeout=30,
        )
    )["streams"]
    assert (streams[0]["width"], streams[0]["height"]) == (90, 160), streams
    for when, blue in ((0.5, False), (1.5, True)):
        pixel = run_process(
            [
                ffmpeg,
                "-v",
                "error",
                "-ss",
                str(when),
                "-i",
                str(layers),
                "-frames:v",
                "1",
                "-vf",
                "crop=2:2:44:78,scale=1:1",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "-",
            ],
            timeout=30,
        )
        assert len(pixel) == 3
        assert (
            (pixel[2] > 200 and pixel[0] < 20)
            if blue
            else (pixel[0] > 200 and pixel[1] > 200)
        ), pixel
    # Seeking switches the same preview between sources and uses source time,
    # including an edited clip's offset. Await the decoder's asynchronous seek.
    for when, path, expected in ((0.5, other, 0.5), (1.5, root / "source.mp4", 2.5)):
        client.send(f"SelectTime: Start={when} End={when}")
        deadline = time.monotonic() + 5
        while True:
            info = client.data("GetVideoPreviewInfo")
            if (
                info["ready"]
                and Path(info["playbackPath"]).name == path.name
                and abs(info["position"] - expected) < 0.08
            ):
                break
            if time.monotonic() >= deadline:
                raise AssertionError(info)
            time.sleep(0.1)
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
    try:
        time.sleep(5.5)
    finally:
        client.send("Stop")
    report = client.data("GetVideoPreviewInfo")
    (root / "_playback.json").write_text(json.dumps(report["samples"], indent=2))
    playing = [sample for sample in report["samples"] if not sample["paused"]]
    assert {Path(sample["source"]).name for sample in playing} == {
        "source.mp4",
        "other-source.mp4",
    }, report
    stable = [
        sample
        for sample in playing
        if 1.3 < sample["audioPosition"] < 2.8 or sample["audioPosition"] > 4.3
    ]
    assert (
        len(stable) >= 10
        and max(
            abs(sample["position"] - sample["expectedPosition"]) for sample in stable
        )
        < 0.2
    ), report
    client.send("SelectTime: Start=0 End=0")
    painted_video_tracks(client)
    drag_owned_video_clip(client, 1.5, 1.5, track=0, to_track=1)
    combined = client.data("GetVideoClips")
    assert len(combined["tracks"]) == 2 and {
        clip["track"] for clip in combined["clips"]
    } == {1}, combined
    assert [
        (clip["start"], Path(clip["source"]).name) for clip in combined["clips"]
    ] == [(1, "source.mp4"), (4, "other-source.mp4")], combined
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
    copied = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert (
        len(copied) == 1 and copied[0]["start"] == 0 and copied[0]["sourceStart"] == 2
    ), copied
    client.send("SelectTime: Start=0 End=2")
    client.send("Cut")
    assert not [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    client.send("Paste")
    pasted = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert [
        {key: value for key, value in clip.items() if key != "id"} for clip in pasted
    ] == [
        {key: value for key, value in clip.items() if key != "id"} for clip in copied
    ], pasted
    client.send("SelectTime: Start=.5 End=1.5")
    client.send("SplitNew")
    split = client.data("GetVideoClips")
    assert (
        len(split["tracks"]) == 3
        and len([clip for clip in split["clips"] if clip["track"] == 2]) == 1
    ), split
    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Set")
    client.send("Duplicate")
    assert len(client.data("GetVideoClips")["tracks"]) == 4
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=.1 End=1.9")
    client.send("Trim")
    trimmed = [
        clip for clip in client.data("GetVideoClips")["clips"] if clip["track"] == 0
    ]
    assert (
        abs(trimmed[0]["start"] - 0.1) < 0.001 and abs(trimmed[-1]["end"] - 1.9) < 0.001
    ), trimmed
    unchanged = client.data("GetVideoClips")
    client.send("AddVideoTrackBefore: Track=999", success=False)
    assert client.data("GetVideoClips") == unchanged
    client.send("AddVideoTrackBefore: Track=1")
    client.send("AddVideoTrackAfter: Track=1")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 6 and not [
        clip for clip in state["clips"] if clip["track"] in (1, 2)
    ], state
    for _ in range(3):
        client.send("NewVideoTrack")
    state = client.data("GetVideoClips")
    assert len(state["tracks"]) == 9
    # New native rows scroll into view; earlier rows scroll out with audio rows.
    time.sleep(0.2)
    rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
    assert rows[0]["y"] < 0 and rows[-1]["y"] >= 0, rows
    save_reopen(client, root / "video.aup3", save_existing=True)
    restored = client.data("GetVideoClips")
    assert restored["clips"] == state["clips"] and len(restored["tracks"]) == 9, (
        restored
    )
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
        time.sleep(0.2)
        rows = client.data("GetVideoPreviewInfo")["videoTrackRows"]
        assert rows[3]["y"] >= 0 and rows[3]["dataWidth"] > 0, rows
        unchanged = layers.read_bytes()
        for _ in range(2):
            client.file("ExportVideoMix", layers, "Overwrite=1", success=False)
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
    user.GetWindowThreadProcessId.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user.GetMenu.argtypes = [ctypes.c_void_p]
    user.GetMenu.restype = ctypes.c_void_p
    user.GetSubMenu.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetSubMenu.restype = ctypes.c_void_p
    user.GetMenuItemCount.argtypes = [ctypes.c_void_p]
    user.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    user.GetMenuItemID.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user.GetMenuStringW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_wchar_p,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    user.PostMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
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
            if len(found) == 1:
                return found[0]
            if time.monotonic() >= deadline:
                raise AssertionError(windows())
            time.sleep(0.05)

    welcome = wait_title("Welcome to AuTopsy!")
    assert user.PostMessageW(welcome, 0x10, 0, 0), ctypes.WinError(
        ctypes.get_last_error()
    )

    def entries(menu):
        result = []
        for index in range(user.GetMenuItemCount(menu)):
            text = ctypes.create_unicode_buffer(256)
            user.GetMenuStringW(menu, index, text, len(text), 0x400)
            result.append(
                (
                    text.value.replace("&", "").split("\t")[0],
                    user.GetMenuItemID(menu, index),
                    user.GetSubMenu(menu, index),
                )
            )
        return result

    # wx also owns hidden menu-bearing windows. Select the editor through its
    # Help menu contract, never the enumeration order, and wait for startup UI.
    deadline = time.monotonic() + 5
    while True:
        menu_windows = {
            window: entries(user.GetMenu(window))
            for window in windows()
            if user.GetMenu(window)
        }
        candidates = [
            (window, child)
            for window, menu in menu_windows.items()
            for text, _, child in menu
            if text == "Help" and user.IsWindowEnabled(window)
        ]
        if len(candidates) == 1:
            main, help_menu = candidates[0]
            break
        if time.monotonic() >= deadline:
            raise AssertionError({"windows": windows(), "menus": menu_windows})
        time.sleep(0.05)

    # Deliberately request screen 1 while hidden and visible. The in-process
    # placement adapter must constrain the request before Windows applies it,
    # including repeated hide/show and an oversized window. No mouse is used.
    from ctypes import wintypes as w

    user.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    user.SetWindowPos.argtypes = [
        w.HWND,
        w.HWND,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    original = w.RECT()
    assert user.GetWindowRect(main, ctypes.byref(original))
    displays = connected_test_displays()
    target = select_test_display(displays)
    outside = next(
        (row for row in displays if row["device"] != target["device"]), target
    )
    for _ in range(2):
        user.ShowWindow(main, 0)
        assert user.SetWindowPos(
            main, None, outside["work"][0], outside["work"][1], 2000, 1500, 0x14
        ), ctypes.WinError(ctypes.get_last_error())
        assert_owned_test_display(pid, main)
        user.ShowWindow(main, 4)
        assert_owned_test_display(pid, main)
        assert user.SetWindowPos(
            main, None, outside["work"][0], outside["work"][1], 2000, 1500, 0x14
        ), ctypes.WinError(ctypes.get_last_error())
        assert_owned_test_display(pid, main)
        assert user.SetWindowPos(
            main,
            None,
            original.left,
            original.top,
            original.right - original.left,
            original.bottom - original.top,
            0x14,
        )
        assert_owned_test_display(pid, main)
    help_entries = entries(help_menu)
    labels = [text for text, _, _ in help_entries]
    assert {
        "AuTopsy Guide",
        "Audio Manual (Upstream)",
        "AuTopsy Support",
        "About AuTopsy",
    } <= set(labels), labels
    assert not any(
        "Audacity" in text or "Check for Updates" in text for text in labels
    ), labels
    about_id = next(
        command for text, command, _ in help_entries if text == "About AuTopsy"
    )
    for _ in range(2):
        assert_owned_test_display(pid)
        assert user.PostMessageW(main, 0x111, about_id, 0), ctypes.WinError(
            ctypes.get_last_error()
        )
        about = wait_title("About AuTopsy")
        assert_owned_test_display(pid, about)
        assert user.PostMessageW(about, 0x10, 0, 0), ctypes.WinError(
            ctypes.get_last_error()
        )
        deadline = time.monotonic() + 5
        while any(text == "About AuTopsy" for text in windows().values()):
            if time.monotonic() >= deadline:
                raise AssertionError(windows())
            time.sleep(0.05)
    (root / "_branding.json").write_text(
        json.dumps(
            {"help": labels, "welcome": "AuTopsy", "about_open_close_count": 2},
            indent=2,
        ),
        encoding="utf-8",
    )


def worker(args, root):
    client = ScriptClient()
    if args.case == "groups":
        check_groups(client, root)
    elif args.case == "tracks":
        check_tracks(client, root)
    elif args.case == "video":
        check_video(client, root, args.ffmpeg, args.ffprobe)
    elif args.case == "media":
        check_media(client, root, args.ffmpeg, args.ffprobe)
    elif args.case == "branding":
        check_branding(client, root)
    else:
        source = root / "source.mp4"
        client.file("ImportVideo", source)
        info = ready_video_preview(client)
        assert info["lightweight"] and info["thumbnails"], info
        assert 0 < info["width"] <= 960 and 0 < info["height"] <= 540, info
        assert client.data("GetInfo: Type=Tracks Format=JSON"), (
            "The original soundtrack was not imported."
        )
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
        samples = [
            sample
            for sample in samples
            if sample["audioPosition"] > samples[0]["audioPosition"] + 1
        ]
        (root / "_playback.json").write_text(json.dumps(samples, indent=2))
        assert all(not sample["paused"] for sample in samples), samples
        assert (
            max(abs(sample["audioPosition"] - sample["position"]) for sample in samples)
            < 0.2
        ), samples
        assert samples[-1]["position"] - samples[0]["position"] > 4, samples
        assert samples[-1]["droppedFrames"] - samples[0]["droppedFrames"] <= 2, samples
        assert (
            samples[-1]["decoderDroppedFrames"] - samples[0]["decoderDroppedFrames"]
            <= 2
        ), samples
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
            if metrics is not None:
                payload["observations"] = metrics
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
    counts: dict[tuple[str, str], int] = {}
    retained = 0
    for _, path, scope in sorted(rows, reverse=True):
        counts[scope] = counts.get(scope, 0) + 1
        if counts[scope] > 3 or retained >= 36:
            path.unlink()
        else:
            retained += 1
    for path in archive.glob("_*.tmp"):
        path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument(
        "--case",
        choices=(
            "groups",
            "tracks",
            "video",
            "media",
            "preview",
            "branding",
            "display-policy",
            "worker-lifecycle",
            "fixtures",
        ),
        required=True,
    )
    parser.add_argument(
        "--interrupt-cache",
        choices=("before-publish", "after-publish"),
        default=os.environ.get("AUTOPSY_INTERRUPT_PREVIEW"),
    )
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--ffprobe", default="")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--reject-display", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.case == "display-policy":
        check_test_display_selection()
        return
    if args.case == "worker-lifecycle":
        if not args.work_root:
            parser.error("Worker lifecycle checks require --work-root.")
        check_behavior_worker_lifecycle(args.work_root.resolve())
        return
    if args.case == "fixtures":
        if not args.work_root or not args.ffmpeg or not args.ffprobe:
            parser.error("Fixture checks require --work-root, --ffmpeg and --ffprobe.")
        check_neutral_preview_fixture(args.work_root.resolve(), args.ffmpeg, args.ffprobe)
        return
    if not args.executable or not args.work_root:
        parser.error("GUI cases require --executable and --work-root.")
    root = args.work_root.resolve() / ("_" + args.case)
    if args.reject_display:
        root = root.with_name(root.name + "_display_rejection")
    if args.worker:
        worker(args, root)
        return
    if os.name != "nt":
        raise RuntimeError("The scripting integration cases require Windows.")
    executable = args.executable.resolve(strict=True)
    module = executable.parent / "modules/mod-script-pipe.dll"
    require_test_display_guard(module)
    display = select_test_display(connected_test_displays())
    print("GUI test display: " + display["device"], flush=True)
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
    if shared_cache.exists() and (
        not cache_owner.is_file() or cache_owner.read_text() != cache_signature
    ):
        raise RuntimeError(
            "The preview test cache has another owner: " + str(shared_cache)
        )
    if root.exists():
        if not marker.is_file() or marker.read_text() != signature:
            raise RuntimeError(
                "The test output directory has another owner: " + str(root)
            )
        previous_cache = root / "_preview-cache"
        if (
            args.case == "preview"
            and previous_cache.is_dir()
            and not shared_cache.exists()
        ):
            shutil.copytree(previous_cache, shared_cache)
            cache_owner.write_text(cache_signature)
        for result_path in root.glob("_result-*.json"):
            observations = root / "_playback.json"
            retain_behavior_result(
                root.parent,
                json.loads(result_path.read_text()),
                json.loads(observations.read_text())
                if observations.is_file()
                else None,
            )
        shutil.rmtree(root)
    root.mkdir()
    marker.write_text(signature)
    for name, channels in (
        ("tone-a.wav", 1),
        ("tone-b.wav", 1),
        ("tone-stereo.wav", 2),
    ):
        write_tone(root / name, channels)
    if args.case in ("video", "media"):
        run_process(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "lavfi",
                "-i",
                "color=c=red:s=160x90:r=30:d=2",
                "-f",
                "lavfi",
                "-i",
                "color=c=blue:s=160x90:r=30:d=2",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=48000:duration=4",
                "-filter_complex",
                "[0:v][1:v]concat=n=2:v=1:a=0[v]",
                "-map",
                "[v]",
                "-map",
                "2:a",
                "-c:v",
                "mpeg4",
                "-q:v",
                "2",
                "-c:a",
                "aac",
                "-shortest",
                str(root / "source.mp4"),
            ],
            timeout=60,
        )
        run_process(
            [
                args.ffmpeg,
                "-v",
                "error",
                "-nostdin",
                "-f",
                "lavfi",
                "-i",
                "color=c=yellow:s=90x160:r=25:d=2",
                "-an",
                "-c:v",
                "mpeg4",
                "-q:v",
                "2",
                str(root / "other-source.mp4"),
            ],
            timeout=30,
        )
    if args.case == "preview":
        write_neutral_preview_video(args.ffmpeg, root / "source.mp4")
    version_numbers = executable_version(executable)
    profile = executable.parent / "Portable Settings"
    profile_owner = profile / "_autopsy_tests"
    if profile.exists():
        if not profile_owner.is_file() or profile_owner.read_text() != str(
            args.work_root.resolve()
        ):
            raise RuntimeError(
                "An unrelated Portable Settings profile exists: " + str(profile)
            )
        shutil.rmtree(profile)
    profile.mkdir()
    profile_owner.write_text(str(args.work_root.resolve()))
    if args.case == "preview" and shared_cache.is_dir():
        shutil.copytree(shared_cache, profile / "_video-previews")
    audio = root / "_audio_temp"
    audio.mkdir()
    gui = root / "_gui_temp"
    gui.mkdir()
    version = ".".join(str(x) for x in version_numbers)
    stamp = datetime.fromtimestamp(module.stat().st_mtime).strftime("%Y-%m-%dT%H:%M:%S")  # noqa: DTZ006 -- matches the upstream module preference in local time
    left, top, right, bottom = display["work"]
    width, height = min(1100, right - left - 32), min(650, bottom - top - 32)
    placement = (
        f"[Window]\nX={left + 16}\nY={top + 16}\nWidth={width}\nHeight={height}\n"
        f"Normal_X={left + 16}\nNormal_Y={top + 16}\nNormal_Width={width}\nNormal_Height={height}\n"
        "Maximized=0\nIconized=0\n"
    )
    (profile / "autopsy.cfg").write_text(
        f"[Version]\nMajor={version_numbers[0]}\nMinor={version_numbers[1]}\nMicro={version_numbers[2]}\n"
        f"[Directories]\nTempDir={audio.as_posix()}\n[GUI]\nShowSplashScreen={int(args.case == 'branding')}\n"
        "[FileFormats]\nMusicFileImportSettingChoice=No\n[Module]\nmod-script-pipe=1\n"
        f"[ModuleDateTime]\nmod-script-pipe={stamp}\n" + placement,
        encoding="utf-8",
    )
    environment = dict(
        os.environ,
        TEMP=str(gui),
        TMP=str(gui),
        PYTHONDONTWRITEBYTECODE="1",
        AUTOPSY_BEHAVIOR_ROOT=str(args.work_root.resolve()),
        AUTOPSY_TEST_DISPLAY=display["device"]
        if not args.reject_display
        else "missing-test-display",
    )
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.WaitForInputIdle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    run_id = uuid.uuid4().hex
    identities = {
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "script_module_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
        "check_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "harness_sha256": hashlib.sha256(
            Path(__file__).with_name("run_fork_test_harness.py").read_bytes()
        ).hexdigest(),
        "pipe_client_sha256": hashlib.sha256(
            Path(__file__).with_name("pipeclient.py").read_bytes()
        ).hexdigest(),
    }
    with (root / "_app.log").open("w") as log:
        app = subprocess.Popen(
            [str(executable)],
            cwd=executable.parent,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            if args.reject_display:
                status = app.wait(timeout=30)
                log.flush()
                report = (root / "_app.log").read_text(
                    encoding="utf-8", errors="replace"
                )
                if (
                    status != 197
                    or "AUTOPSY_TEST_WINDOW show" in report
                    or "test display guard:" not in report
                ):
                    raise RuntimeError(
                        f"Unsafe-display rejection: owned app exit {status}; {report[-4000:]}"
                    )
                print(
                    "display-rejection: PASS (missing screen stopped before the first window)"
                )
                return
            startup = user.WaitForInputIdle(int(app._handle), 30000)  # type: ignore[attr-defined]  # CPython's Windows process handle.
            if startup != 0:
                raise RuntimeError(
                    f"Owned app startup status {startup}; PID {app.pid}; "
                    + startup_window_messages(app.pid)
                )
            deadline = time.monotonic() + 30
            while not kernel.WaitNamedPipeW(pipe, 100):
                if app.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(
                        "The owned test app did not expose its scripting pipe."
                    )
                time.sleep(0.05)
            if app.poll() is not None:
                raise RuntimeError("The owned test app exited during startup.")
            environment["AUTOPSY_OWNED_APP_PID"] = str(app.pid)
            previous_display = os.environ.get("AUTOPSY_TEST_DISPLAY")
            os.environ["AUTOPSY_TEST_DISPLAY"] = display["device"]
            try:
                windows = assert_owned_test_display(app.pid)
            finally:
                if previous_display is None:
                    os.environ.pop("AUTOPSY_TEST_DISPLAY", None)
                else:
                    os.environ["AUTOPSY_TEST_DISPLAY"] = previous_display
            (root / "_display.json").write_text(
                json.dumps({"display": display, "startup_windows": windows}, indent=2)
            )
            worker_command = [
                sys.executable,
                str(Path(__file__)),
                *sys.argv[1:],
                "--worker",
            ]
            if args.interrupt_cache:
                cache = profile / "_video-previews"
                baseline = set(cache.rglob("_complete")) if cache.exists() else set()
                operation = subprocess.Popen(
                    worker_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=environment,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
                try:
                    deadline = time.monotonic() + 45
                    observed = None
                    while (
                        time.monotonic() < deadline
                        and app.poll() is None
                        and operation.poll() is None
                    ):
                        candidates = (
                            list(cache.rglob("_building.mp4"))
                            if args.interrupt_cache == "before-publish"
                            else [
                                path
                                for path in cache.rglob("_complete")
                                if path not in baseline
                            ]
                        )
                        if candidates:
                            observed = candidates[0]
                            break
                        time.sleep(0.005)
                    if observed is None:
                        raise RuntimeError(
                            "The preview did not reach the requested interruption boundary."
                        )
                    app.terminate()
                    app.wait(timeout=20)
                    print("Observed cache interruption: " + args.interrupt_cache)
                    return
                finally:
                    if operation.poll() is None:
                        operation.terminate()
                    output, errors = operation.communicate(timeout=10)
                    (root / "_commands.log").write_text(output, encoding="utf-8")
                    (root / "_errors.log").write_text(errors, encoding="utf-8")
            result = run_behavior_worker(worker_command, environment, app, root)
            if result.returncode:
                raise RuntimeError(
                    f"Command: {subprocess.list2cmdline(worker_command)}\n"
                    f"Behavior worker exit {result.returncode}\n"
                    f"stdout:\n{result.stdout[-14000:] or '<absent>'}\n"
                    f"stderr:\n{result.stderr or '<absent>'}"
                )
            try:
                status = app.wait(timeout=20)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    "Owned app did not exit; " + startup_window_messages(app.pid)
                ) from error
            if status:
                raise RuntimeError(f"The owned app exited with status {status}.")
            log.flush()
            shows = re.findall(
                r"^AUTOPSY_TEST_WINDOW show (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$",
                (root / "_app.log").read_text(encoding="utf-8", errors="replace"),
                re.MULTILINE,
            )
            assert shows, "No pre-show placement observations were recorded."
            for _, x, y, w, h in shows:
                x, y, w, h = map(int, (x, y, w, h))
                assert left <= x and top <= y and x + w <= right and y + h <= bottom, (
                    shows
                )
            source = root / "source.mp4"
            source_identity = None
            if source.is_file():
                info = source.stat()
                source_identity = {
                    "source": "generated:" + args.case,
                    "bytes": info.st_size,
                    "modified_ns": None,
                }
            record = {
                "run": run_id,
                "case": args.case,
                "result": "passed",
                "version": version.strip(),
                "test_display": display,
                "pre_show_count": len(shows),
                "input": source_identity,
                **identities,
            }
            (root / ("_result-" + run_id + ".json")).write_text(
                json.dumps(record, indent=2), encoding="utf-8"
            )
            observations = root / "_playback.json"
            retain_behavior_result(
                root.parent,
                record,
                json.loads(observations.read_text())
                if observations.is_file()
                else None,
            )
            print(args.case + ": PASS (native command, project and media checks)")
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=20)
            if profile_owner.is_file() and profile_owner.read_text() == str(
                args.work_root.resolve()
            ):
                cache = profile / "_video-previews"
                if args.case == "preview" and cache.is_dir():
                    if shared_cache.exists():
                        shutil.rmtree(shared_cache)
                    shutil.copytree(cache, shared_cache)
                    cache_owner.write_text(cache_signature)
                shutil.rmtree(profile)


if __name__ == "__main__":
    main()
