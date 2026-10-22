"""Native folder, phase-aware waveform and stock-Audacity project contracts.

The main harness owns generated media, process/window leases and finite result
retention. This module never touches an installed editor or personal project.
"""
import ctypes
import json
import os
import threading
import time

from run_fork_test_harness import (
    assert_owned_test_display,
    read_audio_project_state,
    save_reopen,
    select_audio_project_tracks,
)


def read_group(client, group_id):
    info = client.data("GetTrackGroups")
    group = next(item for item in info["groups"] if item["id"] == group_id)
    return info, group


def history_command(client, command):
    """Report owned modal text if history blocks, without dismissing anything."""
    finished = threading.Event()

    def observe():
        if finished.wait(5):
            return
        from ctypes import wintypes as w

        user = ctypes.WinDLL("user32", use_last_error=True)
        callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        user.IsWindowVisible.argtypes = [w.HWND]
        user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        user.EnumWindows.argtypes = [callback, w.LPARAM]
        user.EnumChildWindows.argtypes = [w.HWND, callback, w.LPARAM]
        user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                           w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
        user.SendMessageTimeoutW.restype = ctypes.c_size_t
        rows = []

        def owner(handle):
            pid = w.DWORD()
            user.GetWindowThreadProcessId(handle, ctypes.byref(pid))
            return pid.value == int(os.environ["AUTOPSY_OWNED_APP_PID"])

        @callback
        def window(handle, _):
            if len(rows) >= 8 or not owner(handle) or not user.IsWindowVisible(handle):
                return True
            text = ctypes.create_unicode_buffer(256)
            user.GetWindowTextW(handle, text, len(text))
            row = {"title": text.value, "controls": []}

            @callback
            def child(control, _):
                if len(row["controls"]) < 20 and owner(control) and user.IsWindowVisible(control):
                    reply = ctypes.c_size_t()
                    content = ctypes.create_unicode_buffer(256)
                    if user.SendMessageTimeoutW(control, 0xD, len(content), ctypes.addressof(content),
                                                2, 100, ctypes.byref(reply)) and content.value:
                        row["controls"].append(content.value)
                return True

            user.EnumChildWindows(handle, child, 0)
            rows.append(row)
            return True

        user.EnumWindows(window, 0)
        print(json.dumps({"blocked_history": command, "owned_windows": rows}), flush=True)

    observer = threading.Thread(target=observe, daemon=True)
    observer.start()
    try:
        return client.send(command)
    finally:
        finished.set()
        observer.join(timeout=2)


def settled_summary(client, group_id):
    deadline = time.monotonic() + 20
    while True:
        info, group = read_group(client, group_id)
        if group["summaryError"]:
            raise AssertionError(group)
        if group["summaryReady"]:
            assert group["summaryColumns"] > 0, group
            return info, group
        if time.monotonic() >= deadline:
            raise AssertionError(f"Group waveform did not finish: {group}")
        time.sleep(0.05)


def click_group_toggle(client, group_id, cancel=False):
    """Send press/release through the guarded owned pane, including cancellation."""
    from ctypes import wintypes as w

    info, group = read_group(client, group_id)
    panel = int(info["panelHandle"])
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT,
                                       w.UINT, ctypes.POINTER(ctypes.c_size_t)]
    user.SendMessageTimeoutW.restype = ctypes.c_size_t
    owner = w.DWORD()
    assert user.GetWindowThreadProcessId(panel, ctypes.byref(owner))
    assert owner.value == int(os.environ["AUTOPSY_OWNED_APP_PID"])
    assert_owned_test_display(owner.value, include_window=panel)
    rect = group["toggle"]
    assert rect["width"] > 0 and rect["height"] > 0, group
    position = ((rect["x"] + rect["width"] // 2) & 0xFFFF) | (((rect["y"] + rect["height"] // 2) & 0xFFFF) << 16)
    reply = ctypes.c_size_t()

    def message(code, flags, point):
        if not user.SendMessageTimeoutW(panel, code, flags, point, 2, 2000, ctypes.byref(reply)):
            raise RuntimeError(f"Folder toggle message {code:#x} to HWND={panel} failed: {ctypes.WinError()}")

    message(0x200, 0, position)
    message(0x201, 1, position)
    try:
        if cancel:
            message(0x200, 1, 0xFFFFFFFF)
        message(0x202, 0, 0xFFFFFFFF if cancel else position)
    finally:
        message(0x202, 0, 0xFFFFFFFF)


def check_track_groups(client, root):
    for name in ("tone-a.wav", "tone-b.wav", "tone-a.wav"):
        client.file("Import2", root / name)
    # Equal and opposite members must cancel in a summed waveform. Independent
    # peak envelopes or stacked shapes would fail this case.
    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=0 End=5")
    client.send("Invert")
    original = read_audio_project_state(client.data)
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Add")
    client.send('GroupTracks: Name="Dialogue"')
    initial = client.data("GetTrackGroups")
    assert len(initial["groups"]) == 1 and len(initial["tracks"]) == 4, initial
    group_id = initial["groups"][0]["id"]
    assert initial["groups"][0]["members"] == [1, 2], initial
    assert initial["groups"][0]["name"] == "Dialogue", initial
    members = {row["id"] for row in initial["tracks"] if row["group"] == group_id}
    expected_audio = original.copy()
    expected_audio["tracks"] = [original["tracks"][index] for index in (0, 2, 1)]
    expected_audio["clips"] = [original["clips"][index] for index in (0, 2, 1)]
    assert read_audio_project_state(client.data) == expected_audio
    client.send('GroupTracks: Name="Dialogue"')
    assert len(client.data("GetTrackGroups")["groups"]) == 1
    client.send("TrackMoveBottom")
    assert read_group(client, group_id)[1]["index"] == 1
    assert read_group(client, group_id)[1]["members"] == [2, 3]
    client.send("TrackMoveTop")
    assert read_group(client, group_id)[1]["index"] == 0

    def collapse(value):
        client.send(f'SetTrackGroup: Id="{group_id}" Collapsed={int(value)}')
        info, group = read_group(client, group_id)
        assert group["collapsed"] == value
        for row in info["tracks"]:
            hidden = row["group"] == group_id and value
            assert row["hidden"] == hidden, row
            assert (row["height"] == 0) == hidden, row
            assert not hidden or row["bounds"]["height"] == 0, row
        return info, group

    collapse(True)
    collapse(True)
    _, group = settled_summary(client, group_id)
    assert abs(group["minimum"]) < 0.00001 and abs(group["maximum"]) < 0.00001, group
    # Repeated collapse requests must not add extra history transitions.
    history_command(client, "Undo")
    undone, group = read_group(client, group_id)
    assert not group["collapsed"]
    assert {row["id"] for row in undone["tracks"] if row["group"] == group_id} == members
    history_command(client, "Redo")
    assert read_group(client, group_id)[1]["collapsed"]
    collapse(False)
    click_group_toggle(client, group_id, cancel=True)
    assert not read_group(client, group_id)[1]["collapsed"]
    click_group_toggle(client, group_id)
    assert read_group(client, group_id)[1]["collapsed"]
    click_group_toggle(client, group_id)
    assert not read_group(client, group_id)[1]["collapsed"]

    client.send("SelectTracks: Track=2 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=0 End=5")
    client.send("Invert")
    collapse(True)
    _, group = settled_summary(client, group_id)
    assert group["maximum"] > 0.4 and group["minimum"] < -0.4, group
    client.send("SelectTime: Start=7 End=9")
    client.send("ZoomSel")
    _, empty = settled_summary(client, group_id)
    assert empty["minimum"] == 0 and empty["maximum"] == 0, empty
    client.send("SelectTime: Start=0 End=2")
    client.send("ZoomSel")
    _, group = settled_summary(client, group_id)
    assert group["maximum"] > 0.4, group
    # Gain/mute/effect edits refresh the summary without flattening any member.
    def clip_content():
        return [{key: value for key, value in item.items() if key != "track"}
                for item in client.data("GetInfo: Type=Clips Format=JSON")]

    clips = clip_content()
    assert len(clips) == 3
    expected = []
    for collapsed, name in ((True, "_collapsed-folders.aup3"), (False, "_expanded-folders.aup3")):
        collapse(collapsed)
        project = root / name
        client.file("SaveProject2", project)
        state = read_audio_project_state(client.data)
        select_audio_project_tracks(client.send, client.data)
        client.send("SelectTime: Start=0 End=5")
        audio = root / (project.stem + ".wav")
        client.file("Export2", audio, "NumChannels=2")
        expected.append({"project": project.name, "state": state, "audio": audio.name})
    collapse(True)
    saved = root / "_group-roundtrip.aup3"
    save_reopen(client, saved)
    info, group = read_group(client, group_id)
    assert group["collapsed"] and {row["id"] for row in info["tracks"] if row["group"] == group_id} == members
    assert all(row["height"] == 0 for row in info["tracks"] if row["group"] == group_id)
    _, group = settled_summary(client, group_id)
    assert group["maximum"] > 0.4, group
    for _ in range(2):
        client.send(f'SetTrackGroup: Id="{group_id}" Ungroup=1')
        assert not client.data("GetTrackGroups")["groups"]
    assert clip_content() == clips
    history_command(client, "Undo")
    assert read_group(client, group_id)[1]["collapsed"]
    history_command(client, "Redo")
    assert not client.data("GetTrackGroups")["groups"]
    client.file("SaveProject2", root / "_ungrouped.aup3")
    (root / "_compatibility-expected.json").write_text(json.dumps(expected), encoding="utf-8")
