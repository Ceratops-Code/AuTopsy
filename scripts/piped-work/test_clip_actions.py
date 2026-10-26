"""Focused owned-window clip actions; fixtures contain only generated media.

Dialogs are identified by caption and their fields by adjacent labels. Only this
worker's PID is touched, and the shared display/input guards remain in force.
All outputs belong to the harness case directory and its existing cleanup owner.
Physical capture is a separate opt-in hardware case: its project lives only in
the disposable audio directory and no captured samples are retained as fixtures.
"""
import array
import ctypes
import faulthandler
import json
import math
import os
import threading
import time
from ctypes import wintypes as w

from run_fork_test_harness import assert_owned_test_display, operate_owned_video_track, run_process
from test_video_transport import PreviewNativeWindows, wait_for_native_state


class ClipDialogWindows(PreviewNativeWindows):
    """Read current owned controls, without an accessibility package dependency."""

    def __init__(self):
        super().__init__()
        self.callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        signatures = {
            "EnumWindows": ([self.callback_type, w.LPARAM], w.BOOL),
            "EnumChildWindows": ([w.HWND, self.callback_type, w.LPARAM], w.BOOL),
            "GetClassNameW": ([w.HWND, w.LPWSTR, ctypes.c_int], ctypes.c_int),
            "GetWindowRect": ([w.HWND, ctypes.POINTER(w.RECT)], w.BOOL),
            "GetAncestor": ([w.HWND, w.UINT], w.HWND),
            "GetParent": ([w.HWND], w.HWND),
            "GetDlgCtrlID": ([w.HWND], ctypes.c_int),
            "PostMessageW": ([w.HWND, w.UINT, w.WPARAM, w.LPARAM], w.BOOL),
            "SetForegroundWindow": ([w.HWND], w.BOOL),
            "GetForegroundWindow": ([], w.HWND),
        }
        for name, (arguments, result) in signatures.items():
            operation = getattr(self.api, name)
            operation.argtypes, operation.restype = arguments, result

    def message(self, handle, kind, first=0, second=0):
        assert self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"])
        result = ctypes.c_size_t()
        if not self.api.SendMessageTimeoutW(handle, kind, first, second, 2, 1000, ctypes.byref(result)):
            raise RuntimeError(f"Owned HWND={handle} message={kind:#x} failed: {ctypes.WinError()}")
        return result.value

    def windows(self, parent=None):
        result = []

        @self.callback_type
        def collect(handle, _):
            if self.owner(handle) == int(os.environ["AUTOPSY_OWNED_APP_PID"]):
                result.append(handle)
            return True

        if parent is None:
            self.api.EnumWindows(collect, 0)
        else:
            self.api.EnumChildWindows(parent, collect, 0)
        return result

    def kind(self, handle):
        name = ctypes.create_unicode_buffer(128)
        self.api.GetClassNameW(handle, name, len(name))
        return name.value

    def bounds(self, handle):
        rectangle = w.RECT()
        assert self.api.GetWindowRect(handle, ctypes.byref(rectangle))
        return rectangle.left, rectangle.top, rectangle.right, rectangle.bottom

    def dialog(self, title):
        found = [handle for handle in self.windows() if self.visible(handle) and self.caption(handle) == title]
        assert len(found) <= 1, (title, found)
        return found[0] if found else None

    def button(self, dialog, caption):
        found = [handle for handle in self.windows(dialog) if self.kind(handle) == "Button"
                 and self.caption(handle).replace("&", "") == caption]
        assert len(found) == 1, (caption, [(self.kind(h), self.caption(h)) for h in self.windows(dialog)])
        return found[0]

    def field(self, dialog, label, kind="Edit"):
        children = self.windows(dialog)
        labels = [h for h in children if self.caption(h).replace("&", "").casefold() == label.casefold()]
        assert len(labels) == 1, (label, [(self.kind(h), self.caption(h)) for h in children])
        left, top, right, bottom = self.bounds(labels[0])
        candidates = []
        for handle in children:
            if self.kind(handle) != kind:
                continue
            x0, y0, _, y1 = self.bounds(handle)
            if x0 >= right - 2 and abs((y0 + y1) - (top + bottom)) < 35:
                candidates.append((x0 - right, handle))
        assert candidates, (label, kind)
        return min(candidates)[1]

    def text(self, handle, value):
        buffer = ctypes.create_unicode_buffer(str(value))
        self.message(handle, 0xC, 0, ctypes.addressof(buffer))
        assert self.control_text(handle) == str(value)

    def activate(self, dialog):
        assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]), dialog)
        wait_for_native_state(lambda: self.visible(dialog) and self.enabled(dialog), "owned clip dialog ready")
        self.api.SetForegroundWindow(dialog)
        wait_for_native_state(lambda: self.active_in_thread(dialog),
                              lambda: f"owned clip dialog {dialog} active; foreground="
                              f"{self.api.GetForegroundWindow()} ({self.caption(self.api.GetForegroundWindow())!r})")

    def click(self, handle):
        # Standard Windows buttons may ignore BM_CLICK on an inactive dialog.
        # Establish that precondition here, including direct-command cases that
        # intentionally entered text without taking shared desktop focus.
        if self.kind(handle) == "Button":
            self.activate(self.api.GetAncestor(handle, 2))
        super().click(handle)


def with_native_dialog(operation, title, handle_dialog, *, menu_gesture=False, needs_focus=True):
    """Run a normal menu/command while handling its modal window on this worker."""
    windows = ClipDialogWindows()
    errors = []

    def run():
        try:
            operation()
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    dialog = None
    try:
        deadline = time.monotonic() + 15
        while not (dialog := windows.dialog(title)):
            if errors:
                raise errors[0]
            assert time.monotonic() < deadline, f"Owned dialog {title!r} did not appear."
            time.sleep(0.025)
        if menu_gesture:
            # The common menu driver restores its previous focus after the item
            # opens a modal window. Finish that restoration before dialog input.
            worker.join(timeout=10)
            assert not worker.is_alive(), "The owned context-menu gesture did not finish."
            if errors:
                raise errors[0]
        if needs_focus:
            windows.activate(dialog)
        else:
            # Text messages need no desktop focus; the button owner establishes
            # its activation precondition immediately before a click.
            assert_owned_test_display(int(os.environ["AUTOPSY_OWNED_APP_PID"]), dialog)
            wait_for_native_state(lambda: windows.visible(dialog) and windows.enabled(dialog), "owned dialog ready")
        handle_dialog(windows, dialog)
        wait_for_native_state(lambda: not windows.visible(dialog), f"owned {title} closed")
    finally:
        if dialog and windows.visible(dialog):
            windows.api.PostMessageW(dialog, 0x10, 0, 0)
        worker.join(timeout=20)
    assert not worker.is_alive(), "The owned clip action did not return."
    if errors:
        raise errors[0]


def audio_context(client, at, track, item):
    """Use native pane/ruler geometry with the common owned menu gesture driver."""
    info = client.data("GetVideoPreviewInfo")
    boxes = client.data("GetInfo: Type=Boxes Format=JSON")
    pane = next(row["box"] for row in boxes if row.get("label") == "Track Panel")
    rulers = [row["box"] for row in boxes if row.get("label") == "VRuler"]
    ruler = rulers[track]
    row = {"index": track, "x": 0, "y": ruler[1] - pane[1], "height": ruler[3] - ruler[1],
           "dataTop": ruler[1] - pane[1], "dataLeft": info["videoTrackLeftOffset"],
           "dataWidth": info["videoTrackDataWidth"]}
    info["videoTrackRows"] = [row]
    operate_owned_video_track(client, at, track=track, gesture="context", menu_path=(item,), painted=info)


def selected_audio(client, root, name, track, start, end, ffmpeg):
    client.send(f"SelectTracks: Track={track} TrackCount=1 Mode=Set")
    client.send(f"SelectTime: Start={start} End={end}")
    target = root / name
    client.file("Export2", target, "NumChannels=1")
    return array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(target),
                                         "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))


def read_recording_devices():
    """Query MME input capabilities without opening or recording from a device."""
    class InputCaps(ctypes.Structure):
        _fields_ = [("manufacturer", w.WORD), ("product", w.WORD), ("driver", w.DWORD),
                    ("name", w.WCHAR * 32), ("formats", w.DWORD),
                    ("channels", w.WORD), ("reserved", w.WORD)]

    api = ctypes.WinDLL("winmm", use_last_error=True)
    api.waveInGetNumDevs.restype = w.UINT
    api.waveInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(InputCaps), w.UINT]
    api.waveInGetDevCapsW.restype = w.UINT
    result = []
    for index in range(api.waveInGetNumDevs()):
        caps = InputCaps()
        error = api.waveInGetDevCapsW(index, ctypes.byref(caps), ctypes.sizeof(caps))
        if error:
            raise RuntimeError(f"Reading MME input {index} failed with multimedia status {error}.")
        result.append({"name": caps.name, "index": index, "channels": int(caps.channels),
                       "driver": int(caps.driver), "manufacturer": int(caps.manufacturer),
                       "product": int(caps.product), "formats": int(caps.formats)})
    return result


def require_recording_device():
    """Bind hardware acceptance to one explicitly selected, currently present input."""
    requested = os.environ.get("AUTOPSY_TEST_RECORDING_DEVICE", "")
    if not requested or any(character in requested for character in "\r\n"):
        raise RuntimeError("Set AUTOPSY_TEST_RECORDING_DEVICE to one exact, available MME input name.")
    selected = [device for device in read_recording_devices() if device["name"] == requested]
    if len(selected) != 1 or not selected[0]["channels"]:
        raise RuntimeError("Set AUTOPSY_TEST_RECORDING_DEVICE to one exact, available MME input name.")
    return selected[0]


def check_cursor_recording(client, root, ffmpeg):
    """Capture in a bounded gap, interrupt it, and repeat Undo/Redo without recapture.

    The harness deletes the entire private audio directory after closing its
    process, including the project, autosave and SQLite companions. Only geometry
    and device capabilities enter the receipt. Only original generated ranges
    are selected for sound comparisons. Keep every post-capture audio comparison
    in that disposable directory too: even a regression writing into an original
    range must not leave microphone samples in retained test fixtures.
    """
    device = require_recording_device()
    selected = client.send('GetPreference: Name="/AudioIO/RecordingDevice"')
    assert device["name"] in selected, (device, selected)
    client.file("Import2", root / "tone-a.wav")
    imported = client.data("GetInfo: Type=Clips Format=JSON")
    duration = imported[0]["end"] - imported[0]["start"]
    left, right = duration / 4, duration * 3 / 4
    cursor = left + min(0.1, (right - left) / 4)
    client.send(f"SelectTime: Start={left} End={right}")
    client.send("SplitDelete")
    baseline = client.data("GetInfo: Type=Clips Format=JSON")
    assert len(baseline) == 2, baseline
    original_sound = [selected_audio(client, root, f"_original-{side}.wav", 0, start, end, ffmpeg)
                      for side, start, end in (("left", 0, left), ("right", right, duration))]
    client.file("SaveProject2", root / "_audio_temp" / "_cursor-recording.aup3")
    captures = []
    for interrupted in (False, True, False, True):
        client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
        client.send(f"SelectTime: Start={cursor} End={cursor}")
        client.send("Record1stChoice")
        if interrupted:
            time.sleep(min(0.12, (right - cursor) / 3))
            client.send("Stop")
        else:
            time.sleep(right - cursor + 0.6)
        clips = client.data("GetInfo: Type=Clips Format=JSON")
        added = [clip for clip in clips if abs(clip["start"] - cursor) <= 2 / 44100]
        if interrupted and clips == baseline:
            captures.append({"interrupted": True, "no_sample_published": True})
            continue
        assert len(clips) == 3 and len(added) == 1, (baseline, clips)
        clip = added[0]
        assert abs(clip["start"] - cursor) <= 2 / 44100, (cursor, clip)
        assert cursor < clip["end"] <= right + 2 / 44100, (right, clip)
        if not interrupted:
            assert abs(clip["end"] - right) <= 2 / 44100, (right, clip)
        assert all(any((original["start"], original["end"]) == (row["start"], row["end"])
                       for row in clips) for original in baseline), (baseline, clips)
        for expected, (side, start, end) in zip(original_sound,
                (("left", 0, left), ("right", right, duration)), strict=True):
            assert selected_audio(client, root / "_audio_temp", f"_after-{side}.wav", 0, start, end, ffmpeg) == expected
        captures.append({"interrupted": interrupted, "start": clip["start"], "end": clip["end"]})
        client.send("Undo")
        assert client.data("GetInfo: Type=Clips Format=JSON") == baseline
        client.send("Redo")
        assert client.data("GetInfo: Type=Clips Format=JSON") == clips
        client.send("Undo")
    (root / "_playback.json").write_text(json.dumps({"device": device, "captures": captures}), encoding="utf-8")


def check_recording_save_boundary(client, root, ffmpeg, phase):
    """Repeat real record/save/reopen, then wait in the exact native save barrier.

    All captured projects, comparisons and expected data stay in _audio_temp.
    The outer owner stops this process only after the native barrier signals.
    No successful result is returned by this deliberately interrupted worker.
    """
    # A wedged IPC/window call must leave its Python stack in the bounded
    # worker error stream before the outer owner stops the private process.
    faulthandler.dump_traceback_later(55)
    try:
        require_recording_device()
        private = root / "_audio_temp"
        project = private / "_recording-storage.aup3"
        client.file("Import2", root / "tone-a.wav")
        duration = client.data("GetInfo: Type=Clips Format=JSON")[0]["end"]
        left, right = duration / 4, duration * 3 / 4
        cursor = left + 0.1
        client.send(f"SelectTime: Start={left} End={right}")
        client.send("SplitDelete")
        baseline = client.data("GetInfo: Type=Clips Format=JSON")
        client.file("SaveProject2", project)
        # The outer owner repeats the entire journey with a fresh process/project.
        # One normal save/reopen per journey avoids duplicating hardware capture
        # inside that repeat and keeps the input reservation bounded.
        client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
        client.send(f"SelectTime: Start={cursor} End={cursor}")
        client.send("Record1stChoice")
        time.sleep(right - cursor + 0.6)
        clips = client.data("GetInfo: Type=Clips Format=JSON")
        assert len(clips) == 3 and all(row in clips for row in baseline), (baseline, clips)
        recorded = next(row for row in clips if abs(row["start"] - cursor) < 2 / 44100)
        assert abs(recorded["end"] - right) < 2 / 44100, recorded
        expected_audio = selected_audio(client, private, "_saved-mix-0.wav", 0, 0, duration, ffmpeg)
        client.send("Save")
        client.send("Close")
        client.file("OpenProject2", project)
        assert client.data("GetInfo: Type=Clips Format=JSON") == clips
        actual = selected_audio(client, private, "_reopened-mix-0.wav", 0, 0, duration, ffmpeg)
        assert len(actual) == len(expected_audio)
        assert max(abs(a - b) for a, b in zip(actual, expected_audio, strict=True)) < 0.0002
        tracks = client.data("GetInfo: Type=Tracks Format=JSON")
        pending_name = "Recording boundary edit"
        expected = {"clips": clips, "duration": duration, "audio": "_saved-mix-0.wav",
                    "track_names": [tracks[0]["name"], pending_name], "normal_reopen_count": 1, "phase": phase}
        (private / "_recording_expected.json").write_text(json.dumps(expected), encoding="utf-8")
        client.send(f'SetTrackStatus: Name="{pending_name}"')
        (private / "_save_boundary_arm").write_text(phase, encoding="utf-8")
        client.send("Save")
        raise AssertionError("The native save boundary returned without the requested process interruption.")
    finally:
        faulthandler.cancel_dump_traceback_later()



def check_reopened_recording(client, root, ffmpeg):
    """Read the actual interrupted project through the ordinary native importer."""
    private = root / "_audio_temp"
    expected = json.loads((private / "_recording_expected.json").read_text(encoding="utf-8"))
    client.file("OpenProject2", private / "_recording-storage.aup3")
    clips = client.data("GetInfo: Type=Clips Format=JSON")
    assert clips == expected["clips"], (expected["clips"], clips)
    name = client.data("GetInfo: Type=Tracks Format=JSON")[0]["name"]
    assert name in expected["track_names"], name
    if expected["phase"] == "after":
        assert name == expected["track_names"][1], name
    before = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(private / expected["audio"]),
                                          "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
    after = selected_audio(client, private, "_after-cut.wav", 0, 0, expected["duration"], ffmpeg)
    assert len(before) == len(after)
    assert max(abs(a - b) for a, b in zip(before, after, strict=True)) < 0.0002
    client.send("Save")
    (root / "_playback.json").write_text(json.dumps({"normal_reopen_count": expected["normal_reopen_count"],
        "phase": expected["phase"], "clips": len(clips), "saved_audio_preserved": True,
        "saved_name_is_old_or_complete_new": True}), encoding="utf-8")


def run_recording_save_faults(command, environment, app, root, restart_app, cleanup_audio):
    """Own two fresh native stop/reopen journeys and all private child processes.

    The native save signal is live synchronization, not a saved execution plan.
    Each normal case starts from generated media; no prior execution is resumed.
    Captured data is removed only after both old and reopened apps have stopped.
    """
    import sqlite3
    from contextlib import closing
    from run_fork_test_harness import run_behavior_worker

    observations = []
    try:
        for repeat in range(2):
            if repeat:
                cleanup_audio()
                (root / "_audio_temp").mkdir()
                app = restart_app(True)
            stopped = threading.Event()
            replies, failures = [], []
            worker_environment = {**environment, "AUTOPSY_OWNED_APP_PID": str(app.pid)}

            def prepare():
                try:
                    replies.append(run_behavior_worker(command, worker_environment, app, root,
                                                       timeout_seconds=90, expected_app_stop=stopped))
                except BaseException as error:
                    failures.append(error)

            worker = threading.Thread(target=prepare, daemon=True)
            worker.start()
            boundary_failure = None
            try:
                ready = root / "_audio_temp" / "_save_boundary_ready"
                deadline = time.monotonic() + 65
                while not ready.is_file() or ready.read_text(encoding="utf-8") not in ("before", "after"):
                    if not worker.is_alive() or app.poll() is not None or time.monotonic() >= deadline:
                        boundary_failure = "The worker ended, its app exited, or the 65-second boundary deadline expired."
                        break
                    time.sleep(0.02)
                if boundary_failure is None:
                    assert not failures and worker.is_alive() and app.poll() is None
                    stopped.set()
                    app.terminate()
                    app.wait(timeout=10)
            finally:
                if app.poll() is None:
                    app.terminate()
                    app.wait(timeout=10)
                worker.join(timeout=12)
            if boundary_failure is not None:
                output = replies[0].stdout if replies else (root / "_commands.log").read_text(encoding="utf-8")
                errors = replies[0].stderr if replies else (root / "_errors.log").read_text(encoding="utf-8")
                raise RuntimeError(f"Recording did not reach its native commit boundary: {boundary_failure}\n"
                    f"Command: {command!r}\nWorker failures: {failures or '<absent>'}\n"
                    f"stdout:\n{output[-12000:] or '<absent>'}\nstderr:\n{errors or '<absent>'}")
            if worker.is_alive() or failures or not replies:
                raise RuntimeError(f"Recording fault worker cleanup failed: {failures}")
            project = root / "_audio_temp" / "_recording-storage.aup3"
            with closing(sqlite3.connect(project.as_uri() + "?mode=rw", uri=True)) as connection:
                assert connection.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
            app = restart_app(False)
            result = run_behavior_worker([*command, "--recording-storage-recovery"],
                {**environment, "AUTOPSY_OWNED_APP_PID": str(app.pid)}, app, root)
            if result.returncode:
                raise RuntimeError(f"Recording reopen worker exited {result.returncode}: {result.stdout}\n{result.stderr}")
            assert app.wait(timeout=20) == 0
            observations.append({**json.loads((root / "_playback.json").read_text(encoding="utf-8")),
                                 "integrity_check": "ok", "repeat": repeat + 1})
        (root / "_playback.json").write_text(json.dumps({"fault_journeys": observations}), encoding="utf-8")
        return app
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=10)


def check_background_video_menu(client):
    """Create a real video row from the empty pane menu, including cancel/history."""
    assert client.data("GetInfo: Type=Tracks Format=JSON") == []
    for repeat in range(2):
        info = client.data("GetVideoPreviewInfo")
        boxes = client.data("GetInfo: Type=Boxes Format=JSON")
        pane = next(row["box"] for row in boxes if row.get("label") == "Track Panel")
        # This is the painted empty background, not a fabricated media track.
        info["videoTrackRows"] = [{"index": -1, "x": 0, "y": 0,
            "height": pane[3] - pane[1], "dataTop": 20,
            "dataLeft": info["videoTrackLeftOffset"], "dataWidth": info["videoTrackDataWidth"]}]
        operate_owned_video_track(client, 0, track=-1, gesture="context", painted=info)
        assert client.data("GetInfo: Type=Tracks Format=JSON") == []
        operate_owned_video_track(client, 0, track=-1, gesture="context", painted=info,
                                  menu_path=("Add Video Track",))
        created = client.data("GetInfo: Type=Tracks Format=JSON")
        assert len(created) == 1 and created[0]["kind"] == "video", (repeat, created)
        client.send("Undo")
        assert client.data("GetInfo: Type=Tracks Format=JSON") == []
        client.send("Redo")
        assert client.data("GetInfo: Type=Tracks Format=JSON") == created
        client.send("Undo")


def check_audio_clip_actions(client, root, ffmpeg):
    """Whole clicked clip scope, negative/positive dB, cancellation and Undo."""
    check_background_video_menu(client)
    for name in ("tone-a.wav", "tone-b.wav"):
        client.file("Import2", root / name)
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=0.5 End=0.5")
    client.send("Split")
    client.send("FitInWindow")
    client.send("FitV")
    clips = client.data("GetInfo: Type=Clips Format=JSON")
    end = max(row["end"] for row in clips if row["track"] == 0)
    baseline = selected_audio(client, root, "_volume-before.wav", 0, 0, end, ffmpeg)
    other = selected_audio(client, root, "_other-before.wav", 1, 0, end, ffmpeg)

    def dismiss_with_project_query(windows, dialog, button):
        # A context menu opens asynchronously, so another normal scripting
        # request can arrive inside its modal loop. It must wait until the
        # stack-owned window and selection adapter have returned, not merely
        # until that window becomes hidden.
        completed, failures, replies = threading.Event(), [], []

        def query():
            try:
                replies.append(client.data("GetInfo: Type=Tracks Format=JSON"))
            except BaseException as error:
                failures.append(error)
            finally:
                completed.set()

        pending = threading.Thread(target=query, daemon=True)
        pending.start()
        assert not completed.wait(0.25), "A project command ran inside the live clip dialog."
        windows.click(windows.button(dialog, button))
        pending.join(timeout=10)
        assert not pending.is_alive() and not failures and replies, (failures, replies)

    def change(db, commit, context=False):
        client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
        client.send("SetTrackStatus: Focused=1")
        client.send(f"SelectTime: Start=0.5 End={end}")

        def fill(windows, dialog):
            windows.text(windows.field(dialog, "Amplification (dB):"), db)
            if context:
                dismiss_with_project_query(windows, dialog, "Apply" if commit else "Cancel")
            else:
                windows.click(windows.button(dialog, "Apply" if commit else "Cancel"))

        with_native_dialog(lambda: audio_context(client, 0.75, 0, "Clip Volume...") if context
                           else client.send("ChangeAudioClipVolume"), "Amplify", fill, menu_gesture=context)

    for repeat in range(2):
        change(-6, False, context=repeat == 0)
        assert selected_audio(client, root, f"_cancel-{repeat}.wav", 0, 0, end, ffmpeg) == baseline
        change(-6, True)
        quieter = selected_audio(client, root, f"_quiet-{repeat}.wav", 0, 0, end, ffmpeg)
        for low, high, factor in ((0.05, 0.45, 1), (0.55, end - 0.05, 10 ** (-6 / 20))):
            before, after = baseline[round(low * 8000):round(high * 8000)], quieter[round(low * 8000):round(high * 8000)]
            ratio = sum(a * b for a, b in zip(before, after, strict=True)) / sum(a * a for a in before)
            assert abs(ratio - factor) < 0.002, (ratio, factor)
        change(6, True)
        louder = selected_audio(client, root, f"_louder-{repeat}.wav", 0, 0, end, ffmpeg)
        assert max(abs(a - b) for a, b in zip(baseline, louder, strict=True)) < 0.0002
        client.send("Undo")
        client.send("Undo")
        assert selected_audio(client, root, f"_undo-{repeat}.wav", 0, 0, end, ffmpeg) == baseline
    assert selected_audio(client, root, "_other-after.wav", 1, 0, end, ffmpeg) == other

    # The menu must replace the older, broader selection with exactly its clip.
    for repeat in range(2):
        if repeat:
            client.send("SelectAll")
            client.send(f"SelectTime: Start=0 End={end}")
        else:
            client.send("SelectTracks: Track=0 TrackCount=0 Mode=Set")
            client.send("SelectTime: Start=0.25 End=0.25")

        def export_dialog(windows, dialog):
            radio = windows.button(dialog, "Current Selection")
            assert windows.enabled(radio) and windows.message(radio, 0xF0) == 1
            if repeat:
                windows.text(windows.field(dialog, "File Name:"), "_audio-clip.wav")
                windows.text(windows.field(dialog, "Folder:"), root)
            dismiss_with_project_query(windows, dialog, "Export" if repeat else "Cancel")

        with_native_dialog(lambda: audio_context(client, 0.75, 0, "Export Clip..."),
                           "Export Audio", export_dialog, menu_gesture=True)
    exported = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(root / "_audio-clip.wav"),
                                            "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
    expected = baseline[4000:]
    assert abs(len(exported) - len(expected)) < 8
    assert max(abs(a - b) for a, b in zip(exported[32:-32], expected[32:-32], strict=True)) < 0.002
    # Explicit clip scope remains selected when mute/solo excludes its sound.
    # The native exporter can report inaudibility; it must never select another
    # track or silently offer the entire project instead of this clip's range.
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SetTrackAudio: Mute=1")
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("SetTrackAudio: Solo=1")
    client.send("SelectTracks: Track=0 TrackCount=0 Mode=Set")
    client.send("SelectTime: Start=0.25 End=0.25")

    def muted_export(windows, dialog):
        radio = windows.button(dialog, "Current Selection")
        assert windows.enabled(radio) and windows.message(radio, 0xF0) == 1
        windows.click(windows.button(dialog, "Cancel"))

    with_native_dialog(lambda: audio_context(client, 0.75, 0, "Export Clip..."),
                       "Export Audio", muted_export, menu_gesture=True)
    client.file("SaveProject2", root / "_audio-clip-actions.aup3")


def check_video_clip_export(client, root, ffmpeg, ffprobe):
    """Clicked picture and the audible project mix use the same project window."""
    client.file("ImportVideo", root / "source.mp4")
    extra = root / "_late-tone.wav"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                 "aevalsrc=if(between(t\\,2\\,4)\\,0.08*sin(2*PI*880*t)\\,0):s=48000:d=4",
                 str(extra)], timeout=30)
    client.file("Import2", extra)
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    client.send("SelectTime: Start=2 End=2")
    client.send("Split")
    client.send("FitInWindow")
    client.send("FitV")
    output = root / "_exported-clip.mp4"

    def export_menu():
        operate_owned_video_track(client, 3, track=0, gesture="context", menu_path=("Export Clip...",))

    def save_file(windows, dialog):
        fields = [h for h in windows.windows(dialog) if windows.kind(h) == "Edit" and windows.visible(h)]
        assert len(fields) == 1, [(windows.kind(h), windows.caption(h)) for h in windows.windows(dialog)]
        windows.text(fields[0], output)
        windows.click(windows.button(dialog, "Save"))

    def export_dialogs():
        for commit in (False, True):
            errors = []

            def handle_settings(commit=commit):
                try:
                    windows = ClipDialogWindows()
                    wait_for_native_state(lambda: windows.dialog("Video Export Settings"), "clip export settings")
                    dialog = windows.dialog("Video Export Settings")
                    windows.activate(dialog)
                    windows.click(windows.button(dialog, "OK" if commit else "Cancel"))
                    wait_for_native_state(lambda: not windows.visible(dialog), "clip export settings closed")
                except BaseException as error:
                    errors.append(error)

            worker = threading.Thread(target=handle_settings, daemon=True)
            worker.start()
            with_native_dialog(export_menu, "Export Video Clip with Audio Mix", save_file, menu_gesture=True)
            worker.join(timeout=20)
            assert not worker.is_alive()
            if errors:
                raise errors[0]
            if commit:
                wait_for_native_state(lambda: output.is_file(), "video clip export committed")
            else:
                assert not output.exists()

    export_dialogs()
    metadata = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json", str(output)], timeout=30))
    assert abs(float(metadata["format"]["duration"]) - 2) < 0.08, metadata
    pixel = run_process([ffmpeg, "-v", "error", "-ss", "0.5", "-i", str(output), "-frames:v", "1",
                         "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
    assert pixel[2] > 180 and max(pixel[:2]) < 30, pixel
    pcm = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(output), "-map", "0:a:0",
                                       "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
    # A soundtrack from time zero has 440 Hz only. Both 440 and the late 880 Hz
    # source must be audible in a mix taken from the clicked [2,4) seconds.
    sample = pcm[4000:12000]
    for frequency in (440, 880):
        magnitude = abs(sum(value * complex(math.cos(2 * math.pi * frequency * index / 8000),
                                           math.sin(2 * math.pi * frequency * index / 8000))
                            for index, value in enumerate(sample))) / len(sample)
        assert magnitude > 0.012, (frequency, magnitude)
    expected_video = next(stream for stream in metadata["streams"] if stream["codec_type"] == "video")
    clip_id = next(row["id"] for row in client.data("GetVideoClips")["clips"] if row["track"] == 0 and row["start"] == 2)
    client.send(f'SetVideoClipMute: Track=0 Id="{clip_id}" Muted=1')
    source, offline = root / "source.mp4", root / "_offline-source.mp4"
    source.replace(offline)
    try:
        for repeat in range(2):
            output = root / f"_exported-muted-clip-{repeat}.mp4"
            export_dialogs()
            hidden = json.loads(run_process([ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json", str(output)], timeout=30))
            video = next(stream for stream in hidden["streams"] if stream["codec_type"] == "video")
            assert abs(float(hidden["format"]["duration"]) - 2) < 0.08, hidden
            assert (video["width"], video["height"]) == (expected_video["width"], expected_video["height"]), video
            for at in (0.25, 1.75):
                pixel = run_process([ffmpeg, "-v", "error", "-ss", str(at), "-i", str(output), "-frames:v", "1",
                                     "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=30)
                assert len(pixel) == 3 and max(pixel) < 15, pixel
            sound = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(output), "-map", "0:a:0",
                                                  "-ar", "8000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
            assert len(sound) == len(pcm) and max(abs(a - b) for a, b in zip(sound, pcm, strict=True)) < 0.0001
            output.unlink()
    finally:
        offline.replace(source)
    client.file("SaveProject2", root / "_video-clip-export.aup3")


def check_edit_cursor_line(client, root, ffmpeg):
    """Read the actual line through audio, video and the unused part of the pane."""
    client.file("ImportVideo", root / "source.mp4")
    client.file("Import2", root / "tone-b.wav")
    client.send("FitInWindow")
    windows = ClipDialogWindows()
    user = windows.api
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    user.SetWindowPos.restype = w.BOOL
    user.GetWindowLongW.argtypes, user.GetWindowLongW.restype = [w.HWND, ctypes.c_int], w.LONG
    user.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    previous_dpi = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    info = client.data("GetVideoPreviewInfo")
    pane = int(info["videoTrackPanelHandle"])
    main = user.GetAncestor(pane, 2)
    pid = int(os.environ["AUTOPSY_OWNED_APP_PID"])
    assert windows.owner(pane) == pid
    assert_owned_test_display(pid, main)
    pinned = bool(user.GetWindowLongW(main, -20) & 8)
    hidden = [h for h in windows.windows() if windows.visible(h) and windows.caption(h).startswith("Video Preview")]

    def capture():
        left, top, right, bottom = windows.bounds(pane)
        rgb = run_process([ffmpeg, "-v", "error", "-nostdin", "-f", "gdigrab", "-draw_mouse", "0",
                           "-framerate", "30", "-offset_x", str(left), "-offset_y", str(top),
                           "-video_size", f"{right - left}x{bottom - top}", "-i", "desktop", "-frames:v", "1",
                           "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], timeout=15)
        assert len(rgb) == (right - left) * (bottom - top) * 3
        return rgb, right - left, bottom - top

    try:
        for handle in hidden:
            user.ShowWindow(handle, 0)
        assert user.SetWindowPos(main, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x13)
        for area in ("picture", "header"):
            client.send("SelectTime: Start=0.5 End=0.5")
            time.sleep(0.15)
            before, width, height = capture()
            # A real clip click must keep its exact time and paint the full line.
            operate_owned_video_track(client, 1.25, track=0, gesture="click", area=area)
            selection = client.data("GetInfo: Type=Selection Format=JSON")
            at = selection["Start"]
            assert abs(at - 1.25) < 0.02 and selection["End"] == at, (area, selection)
            time.sleep(0.15)
            after, _, _ = capture()
            x = round((info["videoTrackLeftOffset"] + (at - info["viewStart"])
                       / (info["viewEnd"] - info["viewStart"]) * info["videoTrackDataWidth"])
                      * width / info["videoTrackPanelWidth"])
            changed = 0
            for y in range(3, height - 16):
                if max(sum(abs(after[(y * width + column) * 3 + channel]
                               - before[(y * width + column) * 3 + channel]) for channel in range(3))
                       for column in range(x - 2, x + 3)) > 20:
                    changed += 1
            assert changed > (height - 19) * 0.9, (area, changed, height)
    finally:
        user.SetWindowPos(main, ctypes.c_void_p(-1 if pinned else -2), 0, 0, 0, 0, 0x13)
        for handle in hidden:
            user.ShowWindow(handle, 8)
        user.SetThreadDpiAwarenessContext(previous_dpi)
    client.file("SaveProject2", root / "_edit-cursor-line.aup3")
