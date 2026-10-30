"""Picture slip and clip-flag journeys through native commands and dialogs.

The common harness owns the private process, screen selection, generated media
and bounded case directory. No personal footage or installed editor is used.
Each journey covers no-op repetition, cancellation, Undo/Redo and saved state.
"""
import array

from run_fork_test_harness import operate_owned_video_track, painted_video_tracks, run_process, save_reopen
from test_beginner_edits import model, select, video_rows
from test_clip_actions import selected_audio, with_native_dialog
from test_clip_workflows import pixel
from test_video_transport import wait_for_native_state

NAVIGATION_CASES = ("video-slip", "clip-flag-navigation")


def check_picture_slip(client, root, ffmpeg):
    """Picture changes, exact PCM stays put, linked trims still use sound time."""
    client.file("ImportVideo", root / "source.mp4")
    row = video_rows(client)[0]
    track, identifier = row["track"], row["id"]
    command = f'SetVideoClipSlip: Track={track} Id="{identifier}" SourceStart=2'
    client.send(f'SetVideoClipBounds: Track={track} Id="{identifier}" Start=1 End=3')
    for item_track, marker, text in ((track, "a" * 32, "Picture"), (track + 1, "b" * 32, "Sound")):
        client.send(f'AddClipMarker: Track={item_track} At=1.5 Text="{text}" Id="{marker}"')
    before = model(client)
    flags_before = client.data("GetClipMarkers")
    sound_before = selected_audio(client, root, "_slip-before.wav", track + 1, 0, 4, ffmpeg)
    client.send(command)
    slipped = model(client)
    assert slipped["audio"] == before["audio"]
    actual = next(row for row in slipped["video"] if row["track"] == track)
    assert actual["sourceStart"] == 2 and actual["audioSourceOffset"] == -1, actual
    assert actual["start"] == 1 and actual["end"] == 3, actual
    flags = client.data("GetClipMarkers")
    assert next(flag for flag in flags if flag["kind"] == "audio")["time"] == 1.5
    assert next(flag for flag in flags if flag["kind"] == "video")["time"] == .5
    client.send(command)
    client.send("Undo")
    assert model(client) == before and client.data("GetClipMarkers") == flags_before
    client.send("Redo")
    assert model(client) == slipped
    for source in (-1, 3):
        client.send(f'SetVideoClipSlip: Track={track} Id="{identifier}" SourceStart={source}', success=False)
        assert model(client) == slipped
    client.send(f'SetVideoClipSlip: Track={track} Id="999999" SourceStart=1', success=False)
    assert model(client) == slipped
    sound_after = selected_audio(client, root, "_slip-after.wav", track + 1, 0, 4, ffmpeg)
    assert sound_before == sound_after
    select(client, track, 1, 3)
    exported = root / "_slipped-picture.mp4"
    client.file("ExportVideoMix", exported, "SelectedOnly=1 Preset=h264 Width=160 Height=90 FrameRate=30 Quality=12")
    colour = pixel(ffmpeg, exported, .25)
    assert colour[2] > 180 and max(colour[:2]) < 35, colour
    encoded_sound = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(exported),
        "-map", "0:a:0", "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
    assert len(encoded_sound) >= 15900 and max(abs(x) for x in encoded_sound) > .01
    for repeat in range(2):
        client.send(f'SetVideoClipBounds: Track={track} Id="{identifier}" Start=1.25 End=2.75')
        linked = client.data("GetLinkedMediaClips")
        assert all(abs(part["start"] - 1.25) < .0001 and abs(part["end"] - 2.75) < .0001 for part in linked), linked
        client.send("Undo")
        assert model(client) == slipped
    client.send("FitV")
    client.send("FitInWindow")
    for repeat in range(2):
        def cancel(windows, dialog):
            fields = [h for h in windows.windows(dialog) if windows.kind(h) == "Edit"]
            assert len(fields) == 1
            windows.text(fields[0], "1.25")
            windows.click(windows.button(dialog, "Cancel"))
        with_native_dialog(lambda: operate_owned_video_track(client, 1.5, track=track,
            gesture="context", menu_path=("Slip Picture...",)), "Slip Picture", cancel, menu_gesture=True)
        assert model(client) == slipped
    def apply(windows, dialog):
        fields = [h for h in windows.windows(dialog) if windows.kind(h) == "Edit"]
        assert len(fields) == 1
        windows.text(fields[0], "1.5")
        windows.click(windows.button(dialog, "OK"))
    with_native_dialog(lambda: operate_owned_video_track(client, 1.5, track=track,
        gesture="context", menu_path=("Slip Picture...",)), "Slip Picture", apply, menu_gesture=True)
    changed = model(client)
    assert changed["video"][0]["sourceStart"] == 1.5 and changed["audio"] == before["audio"]
    client.send("Undo")
    assert model(client) == slipped
    client.send("Redo")
    assert model(client) == changed
    save_reopen(client, root / "_slip.aup3")
    assert model(client) == changed
    assert client.data("GetClipMarkers") == [
        dict(flag, time=1.0, visible=True) if flag["kind"] == "video" else flag for flag in flags]
    client.send("Save")


def check_flag_navigation(client, root):
    """Mixed flags, filtered list, duplicate source IDs, rename/delete and zoom."""
    def empty(windows, dialog):
        listing = next(h for h in windows.windows(dialog) if windows.kind(h) == "SysListView32")
        assert windows.message(listing, 0x1004) == 0
        for label in ("Jump", "Rename...", "Delete Flag", "Previous", "Next"):
            assert not windows.enabled(windows.button(dialog, label)), label
        windows.click(windows.button(dialog, "Close"))
    with_native_dialog(lambda: client.send("ShowClipFlags"), "Clip Flags", empty)
    client.file("ImportVideo", root / "source.mp4")
    video = video_rows(client)[0]
    track, identifier = video["track"], video["id"]
    for index, at, marker, text in ((track, .5, "a" * 32, "Picture"),
                                   (track + 1, 1.5, "a" * 32, "Sound"),
                                   (track, 3.5, "c" * 32, "Hidden picture")):
        client.send(f'AddClipMarker: Track={index} At={at} Text="{text}" Id="{marker}"')
    client.send(f'SetVideoClipBounds: Track={track} Id="{identifier}" Start=0 End=3')
    client.send("FitInWindow")
    painted = painted_video_tracks(client)
    width = painted["viewEnd"] - painted["viewStart"]
    client.send("SelectTime: Start=0 End=0")
    for repeat in range(2):
        client.send("NextClipFlag")
        assert selection_time(client) == .5
        client.send("NextClipFlag")
        assert selection_time(client) == 1.5
        client.send("NextClipFlag")
        assert selection_time(client) == 1.5, "Navigation wrapped or reached a hidden flag."
        client.send("PreviousClipFlag")
        assert selection_time(client) == .5
        client.send("PreviousClipFlag")
        assert selection_time(client) == .5
        client.send("SelectTime: Start=0 End=0")
    before = model(client)
    flags_before = client.data("GetClipMarkers")
    def controls(windows, dialog):
        children = windows.windows(dialog)
        lists = [h for h in children if windows.kind(h) == "SysListView32"]
        edits = [h for h in children if windows.kind(h) == "Edit"]
        assert len(lists) == len(edits) == 1
        return lists[0], edits[0]
    for repeat in range(2):
        def cancel(windows, dialog):
            listing, search = controls(windows, dialog)
            assert windows.message(listing, 0x1004) == 2
            windows.text(search, "no such flag")
            wait_for_native_state(lambda: windows.message(listing, 0x1004) == 0, "empty flag search")
            assert not windows.enabled(windows.button(dialog, "Jump"))
            windows.text(search, "sound")
            wait_for_native_state(lambda: windows.message(listing, 0x1004) == 1, "sound-only flag search")
            windows.text(search, "")
            wait_for_native_state(lambda: windows.message(listing, 0x1004) == 2, "unfiltered flags")
            windows.click(windows.button(dialog, "Include flags outside trimmed clip edges"))
            wait_for_native_state(lambda: windows.message(listing, 0x1004) == 3, "hidden flags included")
            windows.click(windows.button(dialog, "Close"))
        with_native_dialog(lambda: client.send("ShowClipFlags"), "Clip Flags", cancel)
        assert model(client) == before and client.data("GetClipMarkers") == flags_before
    def rename(windows, dialog):
        listing, search = controls(windows, dialog)
        windows.text(search, "sound")
        wait_for_native_state(lambda: windows.message(listing, 0x1004) == 1, "rename filtered flag")
        def label(edit_windows, edit_dialog):
            fields = [h for h in edit_windows.windows(edit_dialog) if edit_windows.kind(h) == "Edit"]
            assert len(fields) == 1
            edit_windows.text(fields[0], "Renamed sound")
            edit_windows.click(edit_windows.button(edit_dialog, "OK"))
        button = windows.button(dialog, "Rename...")
        with_native_dialog(lambda: windows.api.PostMessageW(button, 0xF5, 0, 0), "Rename Clip Flag", label)
        windows.text(search, "renamed")
        wait_for_native_state(lambda: windows.message(listing, 0x1004) == 1, "renamed flag remains searchable")
        windows.click(windows.button(dialog, "Jump"))
        windows.click(windows.button(dialog, "Close"))
    with_native_dialog(lambda: client.send("ShowClipFlags"), "Clip Flags", rename)
    assert selection_time(client) == 1.5
    after_jump = painted_video_tracks(client)
    assert abs(after_jump["viewEnd"] - after_jump["viewStart"] - width) < .0001
    renamed = client.data("GetClipMarkers")
    assert next(flag for flag in renamed if flag["kind"] == "audio")["text"] == "Renamed sound"
    client.send("Undo")
    assert client.data("GetClipMarkers") == flags_before
    client.send("Redo")
    assert client.data("GetClipMarkers") == renamed
    def remove(windows, dialog):
        listing, search = controls(windows, dialog)
        windows.text(search, "renamed")
        wait_for_native_state(lambda: windows.message(listing, 0x1004) == 1, "delete filtered flag")
        windows.click(windows.button(dialog, "Delete Flag"))
        wait_for_native_state(lambda: windows.message(listing, 0x1004) == 0, "flag removed")
        assert not windows.enabled(windows.button(dialog, "Delete Flag"))
        windows.click(windows.button(dialog, "Close"))
    with_native_dialog(lambda: client.send("ShowClipFlags"), "Clip Flags", remove)
    removed = client.data("GetClipMarkers")
    assert len(removed) == len(renamed) - 1 and model(client) == before
    client.send("Undo")
    assert client.data("GetClipMarkers") == renamed
    client.send("Redo")
    assert client.data("GetClipMarkers") == removed
    save_reopen(client, root / "_flag-navigation.aup3")
    assert client.data("GetClipMarkers") == removed
    client.send("Save")


def selection_time(client):
    selection = client.data("GetInfo: Type=Selection Format=JSON")
    assert abs(selection["Start"] - selection["End"]) < .0001, selection
    return selection["Start"]


def check_video_navigation(client, root, ffmpeg, case):
    if case == "video-slip":
        check_picture_slip(client, root, ffmpeg)
    elif case == "clip-flag-navigation":
        check_flag_navigation(client, root)
    else:
        raise ValueError(f"Unknown video navigation case: {case}")
