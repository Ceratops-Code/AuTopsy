"""Dedicated picture-generator journeys using owned colours, tones and text.

The harness owns the private process, display confinement and all output paths.
These cases use public editing commands; decoded frames, native history and
ordinary project reopen supply the observable outcomes.
"""
import json
import shutil
from pathlib import Path

from run_audacity_consumer import read_audio_project_state, select_audio_project_tracks
from run_fork_test_harness import (
    capture_owned_project, operate_owned_video_track, painted_video_tracks,
    ready_video_preview, run_process, save_reopen,
)
from test_clip_actions import with_native_dialog
from test_video_transport import expect_preview_crop_pixels

GENERATOR_CASES = ("video-reverse", "video-freeze", "video-repeat", "video-pictures", "video-text")


def create_colour_video(root, ffmpeg):
    path = root / "_colours.mp4"
    arguments = [ffmpeg, "-v", "error", "-nostdin", "-n"]
    for colour, seconds in (("red", 1), ("blue", 2), ("lime", 2)):
        arguments += ["-f", "lavfi", "-i", f"color=c={colour}:s=320x180:r=30:d={seconds}"]
    arguments += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=5",
        "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]", "-map", "[v]", "-map", "3:a",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "0", "-g", "1", "-threads", "1",
        "-c:a", "aac", str(path)]
    run_process(arguments, timeout=45)
    return path


def decoded_picture(ffmpeg, path, at):
    return run_process([ffmpeg, "-v", "error", "-ss", str(at), "-i", str(path), "-frames:v", "1",
        "-vf", "scale=160:90", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], text=False, timeout=30)


def colour_at(ffmpeg, path, at):
    picture = decoded_picture(ffmpeg, path, at)
    assert len(picture) == 160 * 90 * 3, len(picture)
    centre = (45 * 160 + 80) * 3
    red, green, blue = picture[centre:centre + 3]
    return "red" if red > 180 and green < 60 and blue < 60 else (
        "green" if green > 180 and red < 60 and blue < 60 else (
        "blue" if blue > 180 and red < 60 and green < 60 else (red, green, blue)))


def expect_timeline_colours(client, root, ffmpeg, expected):
    """Read the owned pane's filmstrip pixels, independent of preview/export."""
    client.send("FitInWindow")
    info = painted_video_tracks(client)
    path = root / "_filmstrip.png"
    capture_owned_project(int(info["videoTrackPanelHandle"]), path)
    rgb = run_process([ffmpeg, "-v", "error", "-i", str(path), "-vf", "scale=800:600",
        "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], text=False, timeout=30)
    assert len(rgb) == 800 * 600 * 3
    best: list[str] = []
    for y in range(600):
        row: list[str] = []
        for x in range(800):
            r, g, b = rgb[(y * 800 + x) * 3:(y * 800 + x) * 3 + 3]
            colour = "red" if r > 180 and g < 60 and b < 60 else (
                "green" if g > 180 and r < 60 and b < 60 else (
                "blue" if b > 180 and r < 60 and g < 60 else None))
            if colour:
                row.append(colour)
        if len(row) > len(best):
            best = row
    observed = [colour for i, colour in enumerate(best) if i == 0 or colour != best[i-1]]
    assert len(best) > 100 and observed == expected, (observed, expected, path)
    path.unlink()


def export_picture(client, root, ffmpeg, ffprobe, name, seconds):
    path = root / f"_{name}.mp4"
    client.file("ExportVideoMix", path, 'Preset="h264" Width=320 Height=180 FrameRate=30 Quality=18')
    data = json.loads(run_process([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=width,height,r_frame_rate,nb_read_frames", "-of", "json", str(path)], timeout=30))
    stream = data["streams"][0]
    assert (stream["width"], stream["height"], stream["r_frame_rate"], int(stream["nb_read_frames"])) == (
        320, 180, "30/1", round(seconds * 30)), stream
    assert decoded_picture(ffmpeg, path, 0), path
    return path


def check_video_generator(client, root, ffmpeg, ffprobe, case):
    client.file("ImportVideo", create_colour_video(root, ffmpeg))
    initial = client.data("GetVideoClips")["clips"][0]
    track, clip = initial["track"], initial["id"]
    audio = read_audio_project_state(client.data)
    if case == "video-pictures":
        return check_video_pictures(client, root, ffmpeg, ffprobe, initial, audio)
    if case == "video-text":
        return check_video_text(client, root, ffmpeg, ffprobe, initial, audio)
    action = {
        "video-reverse": f"ReverseVideoPicture: Track={track} Id={clip}",
        "video-freeze": f"FreezeVideoFrame: Track={track} Id={clip} At=1.25 Seconds=2",
        "video-repeat": f"RepeatVideoPicture: Track={track} Id={clip} Copies=2",
    }[case]
    seconds = {"video-reverse": 5, "video-freeze": 2, "video-repeat": 15}[case]
    before = client.data("GetVideoClips")["clips"]
    client.send(action)
    after = client.data("GetVideoClips")["clips"]
    assert read_audio_project_state(client.data) == audio, "Picture editing changed the native soundtrack."
    for _ in range(2):
        client.send(action)
        assert client.data("GetVideoClips")["clips"] == after
        client.send("Undo")
        assert client.data("GetVideoClips")["clips"] == before, "Repeated request added another history entry."
        client.send("Redo")
        assert client.data("GetVideoClips")["clips"] == after
        for invalid in ("ReverseVideoPicture: Track=99999 Id=1",
                        f"FreezeVideoFrame: Track={track} Id={clip} At=99 Seconds=2",
                        f"RepeatVideoPicture: Track={track} Id={clip} Copies=101"):
            client.send(invalid, success=False)
            assert client.data("GetVideoClips")["clips"] == after
    if case != "video-repeat":
        expect_timeline_colours(client, root, ffmpeg,
            ["green", "blue", "red"] if case == "video-reverse" else ["blue"])
    outputs = []
    for repeat in range(2):
        path = export_picture(client, root, ffmpeg, ffprobe, f"{case}-{repeat}", seconds)
        if case == "video-reverse":
            assert [colour_at(ffmpeg, path, t) for t in (.25, 2.75, 4.75)] == ["green", "blue", "red"]
        elif case == "video-freeze":
            assert [colour_at(ffmpeg, path, t) for t in (.05, 1.9)] == ["blue", "blue"]
        else:
            assert len(after) == 3
            assert [colour_at(ffmpeg, path, t) for t in (.25, 5.25, 10.25)] == ["red"] * 3
            assert [colour_at(ffmpeg, path, t) for t in (4.75, 9.75, 14.75)] == ["green"] * 3
        outputs.append(decoded_picture(ffmpeg, path, .25))
    assert outputs[0] == outputs[1], "A repeated render changed the accepted picture."
    for _ in range(2):
        with_native_dialog(lambda: operate_owned_video_track(client, 1.25, track=track,
            gesture="context", menu_path=("Crop Picture...",)), "Crop Video Picture",
            lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")), menu_gesture=True)
        assert client.data("GetVideoClips")["clips"] == after
    if case != "video-repeat":
        sound = client.data("GetInfo: Type=Clips Format=JSON")
        client.send(f"MoveVideoClip: Track={track} Id={clip} Start=1")
        moved = client.data("GetInfo: Type=Clips Format=JSON")
        assert len(moved) == len(sound)
        for a, b in zip(sound, moved, strict=True):
            assert abs(b["start"] - a["start"] - 1) < 1e-6 and abs(b["end"] - a["end"] - 1) < 1e-6
        client.send("Undo")
        assert client.data("GetVideoClips")["clips"] == after
        assert read_audio_project_state(client.data) == audio
    client.send("SelectTime: Start=0.25 End=0.25")
    ready_video_preview(client)
    expect_preview_crop_pixels(client, root, ffmpeg, {"green" if case == "video-reverse" else "blue" if case == "video-freeze" else "red"})
    save_reopen(client, root / f"_{case}.aup3")
    assert client.data("GetVideoClips")["clips"] == after
    assert read_audio_project_state(client.data) == audio
    if case != "video-repeat":
        expect_timeline_colours(client, root, ffmpeg,
            ["green", "blue", "red"] if case == "video-reverse" else ["blue"])
    reopened = export_picture(client, root, ffmpeg, ffprobe, f"{case}-reopened", seconds)
    assert decoded_picture(ffmpeg, reopened, .25) == outputs[0]


def check_video_text(client, root, ffmpeg, ffprobe, original, audio):
    """Exercise caption publication, rejection, cancellation and retained alpha.

    Project state and decoded pictures supply separate observations. Every modal
    interaction belongs to the harness process on its guarded secondary display.
    The pictures journey separately sends a caption project to stock Audacity.
    """
    from test_video_clip_edits import select_clip_range

    def rows():
        return client.data("GetVideoClips")["clips"]

    def caption_pixels(path, at, present=True):
        picture = decoded_picture(ffmpeg, path, at)
        pixels = [picture[i:i+3] for i in range(0, len(picture), 3)]
        white = sum(min(pixel) > 190 for pixel in pixels)
        assert (white > 30) if present else (white == 0), (at, white, present)
        if present:
            blue = sum(b > 180 and r < 60 and g < 60 for r, g, b in pixels)
            assert blue > 11000, ("The caption obscured its transparent box.", blue)
        return picture

    track, clip = original["track"], original["id"]
    baseline = rows()
    # Rejected input is the pre-publication boundary: no row, samples or history
    # effect may escape, including a failure while preparing the requested font.
    for invalid in ('Text=""', 'Text="Too late" At=6', 'Text="Outside" X=90 Width=80',
                    'Text="Missing" Font="AuTopsy Missing Fixture Font"',
                    'Text="Too long for this tiny box" Size=90 Width=10 Height=10'):
        client.send(f'AddVideoText: Track={track} {invalid}', success=False)
        assert rows() == baseline
    parameters = 'Text="CAPTION" Font="Arial" At=1 Seconds=2 Size=55 X=10 Y=30 Width=80 Height=40'
    client.send(f"AddVideoText: Track={track} {parameters}")
    added = rows()
    caption = next(row for row in added if row["pictureEffect"] == "text")
    text_track, text_id = caption["track"], caption["id"]
    assert (caption["text"], caption["font"], caption["textSize"], caption["textColour"]) == (
        "CAPTION", "Arial", 55, 0xffffff), caption
    layers = client.data("GetVideoClips")["tracks"]
    child = next(layer for layer in layers if layer["index"] == text_track)
    parent = next(layer for layer in layers if layer["index"] == track)
    assert child["parentLayerId"] == parent["layerId"] and child["parentLayerId"]
    for _ in range(2):
        client.send("Undo"); assert rows() == baseline
        client.send("Redo"); assert rows() == added
        client.send(f"SetVideoText: Track={text_track} Id={text_id} {parameters}")
        client.send("Undo"); assert rows() == baseline, "An identical edit added a history entry."
        client.send("Redo"); assert rows() == added
    # Editing style and wording must be one history effect and must not change
    # the retained snapshot that Undo restores.
    styled = 'Text="EDITED" Font="Arial" At=1 Seconds=2 Size=50 Colour=16711935 Bold=1 Italic=1 Outline=0 X=10 Y=30 Width=80 Height=40'
    client.send(f"SetVideoText: Track={text_track} Id={text_id} {styled}")
    edited = rows()
    row = next(row for row in edited if row["track"] == text_track)
    assert (row["text"], row["textColour"], row["textBold"], row["textItalic"], row["textOutline"]) == (
        "EDITED", 0xff00ff, True, True, False), row
    client.send(f"SetVideoText: Track={text_track} Id={text_id} {styled}")
    client.send("Undo"); assert rows() == added
    client.send("Redo"); assert rows() == edited
    styled_output = export_picture(client, root, ffmpeg, ffprobe, "styled-text", 5)
    styled_picture = decoded_picture(ffmpeg, styled_output, 1.5)
    magenta = sum(r > 180 and g < 70 and b > 180 for r, g, b in zip(
        styled_picture[0::3], styled_picture[1::3], styled_picture[2::3], strict=True))
    assert magenta > 30, magenta
    client.send("Undo"); assert rows() == added
    for invalid in ('Text=""', 'Text="Outside" X=90 Width=80',
                    'Text="Missing" Font="AuTopsy Missing Fixture Font"'):
        client.send(f'SetVideoText: Track={text_track} Id={text_id} At=1 Seconds=2 {invalid}', success=False)
        assert rows() == added
    first = export_picture(client, root, ffmpeg, ffprobe, "text", 5)
    caption_pixels(first, .5, False); expected = caption_pixels(first, 1.5)
    caption_pixels(first, 3.5, False)
    for _ in range(2):
        with_native_dialog(lambda: operate_owned_video_track(client, .25, track=track,
            gesture="context", menu_path=("Add Text Overlay...",)), "Add Text Overlay",
            lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")), menu_gesture=True)
        with_native_dialog(lambda: operate_owned_video_track(client, 1.5, track=text_track,
            gesture="context", menu_path=("Edit Text Overlay...",)), "Edit Text Overlay",
            lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")), menu_gesture=True)
        assert rows() == added

    def enter_caption(windows, dialog):
        fields = [handle for handle in windows.windows(dialog)
                  if windows.kind(handle) == "Edit" and windows.control_text(handle) == "CAPTION"]
        assert len(fields) == 1, fields
        windows.text(fields[0], "GUI caption")
        windows.click(windows.button(dialog, "OK"))

    with_native_dialog(lambda: operate_owned_video_track(client, 1.5, track=text_track,
        gesture="context", menu_path=("Edit Text Overlay...",)), "Edit Text Overlay",
        enter_caption, menu_gesture=True)
    assert next(row for row in rows() if row["track"] == text_track)["text"] == "GUI caption"
    client.send("Undo"); assert rows() == added
    # Split/join and nested picture effects must retain alpha in their private
    # intermediates. Checking only the direct PNG would miss black-box regressions.
    select_clip_range(client, 2, 2, text_track); client.send("SplitVideo")
    split = rows()
    assert [(row["start"], row["end"]) for row in split if row["track"] == text_track] == [(1, 2), (2, 3)]
    select_clip_range(client, 1, 3, text_track); client.send("JoinVideoClips")
    joined = rows(); joined_caption = next(row for row in joined if row["track"] == text_track)
    nested_id = joined_caption["id"]
    for command in (f'ReverseVideoPicture: Track={text_track} Id={nested_id}',
                    f'FreezeVideoFrame: Track={text_track} Id={nested_id} At=1.5 Seconds=2'):
        before = rows(); client.send(command)
        nested = export_picture(client, root, ffmpeg, ffprobe, "nested-" + command.split(":")[0], 5)
        caption_pixels(nested, 1.5)
        client.send("Undo"); assert rows() == before
    client.send("SeparateVideoClips")
    assert [(row["start"], row["end"]) for row in rows() if row["track"] == text_track] == [(1, 2), (2, 3)]
    client.send("Undo"); assert rows() == joined
    client.send("Undo"); assert rows() == split
    client.send("Undo"); assert rows() == added
    client.send("SelectTime: Start=1.5 End=1.5")
    ready_video_preview(client)
    expect_preview_crop_pixels(client, root, ffmpeg, {"blue", "white"}, edge_colours=("purple",))
    assert read_audio_project_state(client.data) == audio
    saved = root / "_text-overlay.aup3"
    save_reopen(client, saved)
    assert rows() == added and read_audio_project_state(client.data) == audio
    again = export_picture(client, root, ffmpeg, ffprobe, "text-reopened", 5)
    assert caption_pixels(again, 1.5) == expected


def check_video_pictures(client, root, ffmpeg, ffprobe, original, audio):
    image = root / "_neutral.png"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-n", "-f", "lavfi", "-i",
        "color=blue:s=320x180", "-frames:v", "1", "-threads", "1", str(image)], timeout=30)
    client.file("ImportStillImage", image, "At=0 Seconds=5")
    expect_timeline_colours(client, root, ffmpeg, ["blue"])
    image_export = export_picture(client, root, ffmpeg, ffprobe, "still", 5)
    assert colour_at(ffmpeg, image_export, .25) == colour_at(ffmpeg, image_export, 4.75) == "blue"
    moved = root / "_moved.png"; image.rename(moved)
    before_relink = client.data("GetVideoClips")["clips"]
    request = f'RelinkVideoMedia: OldPath="{image.as_posix()}" Filename="{moved.as_posix()}"'
    client.send(request)
    relinked = client.data("GetVideoClips")["clips"]
    assert any(row["source"] == moved.as_posix() for row in relinked)
    for _ in range(2):
        client.send(request)
        client.send("Undo"); assert client.data("GetVideoClips")["clips"] == before_relink
        client.send("Redo"); assert client.data("GetVideoClips")["clips"] == relinked
    still_export = export_picture(client, root, ffmpeg, ffprobe, "still-relinked", 5)
    assert colour_at(ffmpeg, still_export, .25) == "blue"
    client.send('AddVideoTitle: Text="Neutral title" At=0 Seconds=5 Width=320 Height=180')
    after = client.data("GetVideoClips")["clips"]
    client.send("Undo")
    assert all(row["pictureEffect"] != "title" for row in client.data("GetVideoClips")["clips"])
    client.send("Redo")
    assert client.data("GetVideoClips")["clips"] == after
    for _ in range(2):
        client.send('AddVideoTitle: Text="" At=0 Seconds=5', success=False)
        assert client.data("GetVideoClips")["clips"] == after
    title_export = export_picture(client, root, ffmpeg, ffprobe, "title", 5)
    picture = decoded_picture(ffmpeg, title_export, .25)
    white = sum(all(channel > 190 for channel in picture[i:i+3]) for i in range(0, len(picture), 3))
    black = sum(all(channel < 30 for channel in picture[i:i+3]) for i in range(0, len(picture), 3))
    assert white > 20 and black > 8000, (white, black)
    # A generated source remains an ordinary editable clip, including nesting.
    rows = client.data("GetVideoClips")["clips"]
    movie = next(row for row in rows if Path(row["source"]).name == "_colours.mp4")
    still = next(row for row in rows if row["pictureEffect"] == "image")
    client.send(f'ReverseVideoPicture: Track={movie["track"]} Id={movie["id"]}')
    client.send(f'FreezeVideoFrame: Track={still["track"]} Id={still["id"]} At=1 Seconds=5')
    saved = client.data("GetVideoClips")["clips"]
    save_reopen(client, root / "_pictures.aup3")
    assert client.data("GetVideoClips")["clips"] == saved
    assert read_audio_project_state(client.data) == audio
    output = export_picture(client, root, ffmpeg, ffprobe, "title-reopened", 5)
    assert decoded_picture(ffmpeg, output, .25) == picture
    # Include the optional caption fields in the stock-consumer fixture. Audacity
    # must still open the ordinary project and retain every native audio clip.
    client.send(f'AddVideoText: Track={movie["track"]} Text="Compatible caption" At=1 Seconds=2 Size=35')
    saved = client.data("GetVideoClips")["clips"]
    save_reopen(client, root / "_pictures-with-text.aup3")
    assert client.data("GetVideoClips")["clips"] == saved
    assert read_audio_project_state(client.data) == audio
    # The consumer runner opens this in stock Audacity and checks its audio mix.
    state = read_audio_project_state(client.data)
    select_audio_project_tracks(client.send, client.data)
    client.send("SelectTime: Start=0 End=5")
    client.file("Export2", root / "_pictures-mix.wav", "NumChannels=2")
    shutil.copyfile(root / "_pictures-with-text.aup3", root / "_pictures-consumer.aup3")
    (root / "_compatibility-expected.json").write_text(json.dumps([
        {"project": "_pictures-consumer.aup3", "state": state, "audio": "_pictures-mix.wav"}
    ]))
