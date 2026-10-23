"""Temporary clip move sets exercised through owned native clicks and drags.

The case uses only generated tones and colour bars. The common harness owns its
process, display/input guard, finite result archive and cleanup. No fixture or
selection is stored outside that harness's case directory.
"""
import ctypes
import json

from run_fork_test_harness import operate_owned_video_track, painted_video_tracks, save_reopen


def audio_clips(client, track=None):
    clips = client.data("GetInfo: Type=Clips Format=JSON")
    return sorted((row for row in clips if track is None or row["track"] == track),
                  key=lambda row: (row["track"], row["start"]))


def audio_gesture(client, track, at, end=None, *, control=False, cancel=False, body=False):
    """Resolve actual audio header cells, never an assumed upstream row height."""
    info = client.data("GetVideoPreviewInfo")
    boxes = client.data("GetInfo: Type=Boxes Format=JSON")
    pane = next(row["box"] for row in boxes if row.get("label") == "Track Panel")
    headers = [row["box"] for row in boxes if row.get("label") == "ClipHeader" and row["track"] == track]
    assert len(headers) == 1, (track, boxes)
    header = headers[0]
    row = {"index": track, "x": 0, "y": header[1] - pane[1],
           "height": header[3] - header[1], "dataTop": header[1] - pane[1],
           "dataLeft": info["videoTrackLeftOffset"], "dataWidth": info["videoTrackDataWidth"]}
    info["videoTrackRows"] = [row]
    operate_owned_video_track(client, at, end, track=track, painted=info, control=control,
                             cancel=cancel, area="picture" if body else "header",
                             gesture="click" if end is None else "drag")
    assert not ctypes.WinDLL("user32").GetAsyncKeyState(0x11) & 0x8000, "Test left Ctrl pressed."


def same_starts(actual, expected, tolerance=0.0001):
    assert len(actual) == len(expected), (actual, expected)
    assert all(abs(a["start"] - b["start"]) <= tolerance for a, b in zip(actual, expected)), (actual, expected)


def moved_set(before, after, selected, desired=None):
    """All selected clips share one movement; every omitted clip stays put."""
    assert len(before) == len(after), (before, after)
    deltas = [new["start"] - old["start"] for old, new in zip(before, after)]
    delta = deltas[next(iter(selected))]
    assert all(abs(value - (delta if index in selected else 0)) < 0.0001
               for index, value in enumerate(deltas)), (before, after, selected, deltas)
    if desired is not None:
        assert abs(delta - desired) < 0.08, (delta, desired)
    return delta


def place_audio(client, track, at, start):
    client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
    client.send(f"SetClipStatus: At={at} Start={start}")


def check_audio_selection(client, root):
    client.file("Import2", root / "tone-a.wav")
    duration = audio_clips(client, 0)[0]["end"]
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={duration/3} End={duration/3}")
    client.send("Split")
    client.send(f"SelectTime: Start={2*duration/3} End={2*duration/3}")
    client.send("Split")
    # Three separate pieces, with one deliberately omitted between the chosen
    # pair. A time-span implementation would wrongly include that middle piece.
    for index, start in ((2, 10), (1, 6), (0, 2)):
        clip = audio_clips(client, 0)[index]
        place_audio(client, 0, (clip["start"] + clip["end"]) / 2, start)
    client.file("Import2", root / "tone-b.wav")
    place_audio(client, 1, 0.25, 3)
    client.send("FitInWindow")
    client.send("FitV")
    before = audio_clips(client)
    first_at = (before[0]["start"] + before[0]["end"]) / 2
    last_at = (before[2]["start"] + before[2]["end"]) / 2
    other_at = (before[3]["start"] + before[3]["end"]) / 2
    audio_gesture(client, 0, first_at)
    audio_gesture(client, 0, last_at, control=True)
    audio_gesture(client, 1, other_at, control=True, body=True)
    # A click alone is temporary state, not a project edit.
    same_starts(audio_clips(client), before)
    audio_gesture(client, 0, first_at, first_at + 1)
    after = audio_clips(client)
    delta = moved_set(before, after, {0, 2, 3}, 1)
    client.send("Undo")
    same_starts(audio_clips(client), before)
    client.send("Redo")
    same_starts(audio_clips(client), after)

    # Undo replaces native clip objects. A fresh click must use live identities,
    # without reviving deleted identities or silently expanding a time range.
    client.send("Undo")
    audio_gesture(client, 0, first_at)
    audio_gesture(client, 1, other_at, control=True)
    audio_gesture(client, 0, first_at, first_at + 1, cancel=True)
    same_starts(audio_clips(client), before)
    audio_gesture(client, 0, first_at)
    audio_gesture(client, 1, other_at, control=True)
    audio_gesture(client, 1, other_at, control=True)
    audio_gesture(client, 0, first_at, first_at + 1)
    moved_set(before, audio_clips(client), {0}, 1)
    client.send("Undo")
    same_starts(audio_clips(client), before)

    # The first track's obstacle limits the common movement of the whole set.
    audio_gesture(client, 0, first_at)
    audio_gesture(client, 1, other_at, control=True)
    audio_gesture(client, 0, first_at, first_at + 6)
    collided = audio_clips(client)
    allowed = moved_set(before, collided, {0, 3})
    assert 0 < allowed < 6, (allowed, collided)
    assert abs(collided[0]["end"] - collided[1]["start"]) < 0.001, collided
    client.send("Undo")
    same_starts(audio_clips(client), before)
    # The next ordinary move replaces the previous explicit set.
    middle_at = (before[1]["start"] + before[1]["end"]) / 2
    audio_gesture(client, 0, middle_at, middle_at + 0.5)
    moved_set(before, audio_clips(client), {1}, 0.5)
    client.send("Undo")
    same_starts(audio_clips(client), before)
    return {"first_move_seconds": delta, "collision_limit_seconds": allowed}


def check_mixed_selection(client, root):
    client.send("SelectAll")
    client.send("RemoveTracks")
    client.file("ImportVideo", root / "source.mp4")
    client.send("SelectAll")
    client.send("SelectTime: Start=2 End=2")
    client.send("Split")
    videos = client.data("GetVideoClips")["clips"]
    assert len(videos) == 2, videos
    video_track = videos[0]["track"]
    client.file("Import2", root / "tone-a.wav")
    clips = audio_clips(client)
    assert len(clips) == 3, clips
    independent = clips[-1]["track"]
    place_audio(client, independent, 0.25, 1)
    client.send("FitInWindow")
    client.send("FitV")
    before = audio_clips(client)
    painted_video_tracks(client)
    operate_owned_video_track(client, 1, track=video_track, gesture="click")
    operate_owned_video_track(client, 3, track=video_track, gesture="click", control=True)
    audio_gesture(client, independent, 1.5, control=True)
    operate_owned_video_track(client, 1, 2, track=video_track)
    after = audio_clips(client)
    delta = moved_set(before, after, {0, 1, 2}, 1)
    pictures = client.data("GetVideoClips")["clips"]
    assert len(pictures) == len(videos)
    assert all(abs(picture["start"] - video["start"] - delta) < 0.0001
               for picture, video in zip(pictures, videos)), (pictures, delta)
    # Import's existing picture/sound link remains effective; the temporary
    # selection adds independent sound without creating a persistent group for it.
    groups = client.data("GetAudioClipGroups")
    assert groups[-1]["group"] == "", groups
    client.send("Undo")
    same_starts(audio_clips(client), before)
    same_starts(client.data("GetVideoClips")["clips"], videos)
    client.send("Redo")
    same_starts(audio_clips(client), after)
    client.send("Undo")
    operate_owned_video_track(client, 1, track=video_track, gesture="click")
    operate_owned_video_track(client, 3, track=video_track, gesture="click", control=True)
    audio_gesture(client, independent, 1.5, control=True)
    audio_gesture(client, independent, 1.5, 2.5, cancel=True)
    same_starts(audio_clips(client), before)
    # Move from an audio anchor as well as from the picture anchor, repeated
    # after cancellation. Both routes must keep the linked soundtrack in step.
    audio_gesture(client, independent, 1.5)
    operate_owned_video_track(client, 1, track=video_track, gesture="click", control=True)
    operate_owned_video_track(client, 3, track=video_track, gesture="click", control=True)
    audio_gesture(client, independent, 1.5, 2.5)
    moved_set(before, audio_clips(client), {0, 1, 2}, 1)
    save_reopen(client, root / "_temporary-selection.aup3")
    assert client.data("GetAudioClipGroups")[-1]["group"] == ""
    return {"linked_picture_sound_delta": delta}


def check_clip_selection(client, root):
    observations = {"audio": check_audio_selection(client, root),
                    "mixed": check_mixed_selection(client, root)}
    (root / "_selection-observations.json").write_text(json.dumps(observations, indent=2), encoding="utf-8")
