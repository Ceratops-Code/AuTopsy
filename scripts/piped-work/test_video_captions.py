"""Focused caption journeys with independent subtitle and recognition results.

The native harness owns private processes, screen confinement and temporary
outputs. Recognition uses synthetic English or the licensed Hebrew fixture;
no microphone or personal media is read. Each handler has its own input identity.
"""
import hashlib
import inspect
from pathlib import Path
from typing import Any

from run_audacity_consumer import read_audio_project_state
from run_fork_test_harness import operate_owned_video_track, run_process, save_reopen
from test_clip_actions import with_native_dialog


def check_caption_interchange(client, root, ffmpeg, ffprobe, original, audio):
    """Subtitle interchange uses neutral multilingual cues.

    Public commands supply every project effect. File/word/timestamp checks,
    Undo, export frames and normal reopen observe the result independently.
    """
    from test_video_generators import export_picture

    track = original["track"]

    def rows():
        return client.data("GetVideoClips")["clips"]

    before = rows()
    source = root / "_captions.srt"
    source.write_text("1\n00:00:00,500 --> 00:00:02,000\nBlue headphones\n\n"
                      "2\n00:00:02,000 --> 00:00:04,000\nשלום עולם\n\n", encoding="utf-8")
    for repeat in range(2):
        client.file("ImportCaptions", source, f"Track={track}")
        added = rows()
        caption_rows = [row for row in added if row["pictureEffect"] == "text"]
        assert len(caption_rows) == 2 and len({row["track"] for row in caption_rows}) == 1, caption_rows
        captions = caption_rows[0]["track"]
        assert {row["text"] for row in caption_rows} == {"Blue headphones", "שלום עולם"}
        with_native_dialog(lambda: operate_owned_video_track(client, .5, track=track,
            gesture="context", menu_path=("Generate Captions...",)), "Generate Captions",
            lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")), menu_gesture=True)
        assert rows() == added, "Cancelling caption options changed the project."
        client.file("ImportCaptions", source, f"Track={track}")
        assert rows() == added, "Repeating the same caption import duplicated its clips."
        client.send("Undo"); assert rows() == before, "A batch needs exactly one Undo."
        client.send("Redo"); assert rows() == added
        for extension in ("srt", "vtt"):
            output = root / f"_captions-{repeat}.{extension}"
            for _ in range(2):
                client.file("ExportCaptions", output, f"Track={captions}")
            text = output.read_text(encoding="utf-8")
            assert "Blue headphones" in text and "שלום עולם" in text
            assert ("00:00:00,500" if extension == "srt" else "00:00:00.500") in text
            output.write_text("Existing user subtitle", encoding="utf-8")
            client.file("ExportCaptions", output, f"Track={captions}", success=False)
            assert output.read_text(encoding="utf-8") == "Existing user subtitle"
            client.file("ExportCaptions", output, f"Track={captions} Overwrite=1")
            assert output.read_text(encoding="utf-8") == text
        bad = root / "_bad-captions.srt"
        bad.write_text("1\n00:00:00,000 --> 00:00:01,000\nGood\n\n2\ninvalid time\nBad\n", encoding="utf-8")
        client.file("ImportCaptions", bad, f"Track={track}", success=False)
        assert rows() == added, "A malformed later cue published a partial track."
        client.send(f"GenerateCaptions: Track={track} Model=unknown Language=en", success=False)
        assert rows() == added, "An invalid model changed project content."
        assert read_audio_project_state(client.data) == audio
        save_reopen(client, root / f"_captions-{repeat}.aup3")
        assert rows() == added and read_audio_project_state(client.data) == audio
        movie = export_picture(client, root, ffmpeg, ffprobe, f"captions-{repeat}", 5)
        # Observe the encoded size. Halving an eight-pixel fixture font removes
        # its opaque cores through antialiasing without removing any caption.
        picture = run_process([ffmpeg, "-v", "error", "-ss", "1", "-i", str(movie), "-frames:v", "1",
            "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], text=False, timeout=30)
        assert len(picture) == 320 * 180 * 3
        assert sum(min(picture[i:i+3]) > 170 for i in range(0, len(picture), 3)) > 5, "Exported captions disappeared."
        if repeat == 0:
            # Reopen resets history, so remove the caption row through the normal
            # selected-track command before independently repeating the import.
            client.send(f"SelectTracks: Track={captions} TrackCount=1 Mode=Set")
            client.send("RemoveTracks")
            before = rows()

def check_caption_english(client, root, ffmpeg, ffprobe, original, audio):
    """Real speech, exact silence, cancellation, repeated publication and reopen."""
    def rows():
        return client.data("GetVideoClips")["clips"]

    silent = root / "_silence.wav"
    run_process([ffmpeg, "-v", "error", "-f", "lavfi", "-i",
        "anullsrc=r=16000:cl=mono", "-t", "2", "-c:a", "pcm_s16le", str(silent)], timeout=30)
    client.file("Import2", silent)
    silent_track = len(client.data("GetInfo: Type=Tracks Format=JSON"))-1
    before = rows()
    client.send(f"GenerateCaptions: Track={silent_track} Model=tiny Language=en", success=False)
    assert rows() == before, "Silence published hallucinated captions."
    client.send(f"SelectTracks: Track={silent_track} TrackCount=1 Mode=Set")
    client.send("RemoveTracks")

    speech = root / "_neutral-speech.wav"
    # The desktop worker deliberately uses a bare, stable Python. Speech COM is
    # owned by the repository-tools runtime, whose manifest declares pywin32.
    # Run only this fixture producer in that explicit runtime, without changing
    # global Python or invalidating the compiled application's acceptance.
    speech_code = """import sys,win32com.client
voice=win32com.client.Dispatch('SAPI.SpVoice')
voices=voice.GetVoices('Language=409')
assert voices.Count, 'An English Windows voice is required for this neutral fixture'
voice.Voice=voices.Item(0)
stream=win32com.client.Dispatch('SAPI.SpFileStream')
stream.Format.Type=22
stream.Open(sys.argv[1],3,False)
try:
    voice.AudioOutputStream=stream
    voice.Speak('A small green dinosaur is wearing blue headphones. This is a caption test.')
finally:
    stream.Close()
"""
    run_process(["uv", "run", "--project", str(Path(__file__).resolve().parents[1]),
        "--locked", "--python", "3.12", "python", "-c", speech_code, str(speech)], timeout=45)
    client.file("Import2", speech)
    tracks = client.data("GetInfo: Type=Tracks Format=JSON")
    speech_track = len(tracks)-1
    client.send("SelectTime: Start=0 End=0")
    baseline = rows()
    action = f"GenerateCaptions: Track={speech_track} Model=tiny Language=en"
    with_native_dialog(lambda: client.send(action, success=False), "Download Caption Model",
        lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")))
    assert rows() == baseline, "Cancelling model acquisition changed the project."
    client.send(action)
    generated = rows()
    fresh = [row for row in generated if row not in baseline and row["pictureEffect"] == "text"]
    words = " ".join(row["text"] for row in fresh).lower()
    assert all(word in words for word in ("green", "dinosaur", "blue", "headphones")), words
    assert len({row["track"] for row in fresh}) == 1
    speech_track = len(client.data("GetInfo: Type=Tracks Format=JSON"))-1
    action = f"GenerateCaptions: Track={speech_track} Model=tiny Language=en"
    client.send(action); assert rows() == generated, "Repeated recognition duplicated accepted captions."
    client.send("Undo"); assert rows() == baseline
    client.send("Redo"); assert rows() == generated
    save_reopen(client, root / "_recognized-captions.aup3")
    assert rows() == generated
    speech_track = len(client.data("GetInfo: Type=Tracks Format=JSON"))-1
    with_native_dialog(lambda: client.send(f"GenerateCaptions: Track={speech_track} Model=tiny Language=en", success=False),
        "Generate Captions", lambda windows, dialog: windows.click(windows.button(dialog, "Cancel")))
    assert rows() == generated, "Cancelling a recognition job changed the project."

def check_caption_hebrew(client, root, ffmpeg, ffprobe, original, audio):
    """A licensed Hebrew speech fixture checks words, timing and atomic history."""
    def rows():
        return client.data("GetVideoClips")["clips"]

    fixture = Path(__file__).resolve().parents[2] / "tests/fixtures/captions/hebrew.wav"
    client.file("Import2", fixture)
    speech_track = len(client.data("GetInfo: Type=Tracks Format=JSON"))-1
    client.send("SelectTime: Start=0 End=0")
    baseline = rows()
    client.send(f"GenerateCaptions: Track={speech_track} Model=base Language=he")
    fresh = [row for row in rows() if row not in baseline and row["pictureEffect"] == "text"]
    words = " ".join(row["text"] for row in fresh)
    assert sum(word in words for word in ("אנשים", "השמש", "הארץ")) >= 2, words
    assert len({row["track"] for row in fresh}) == 1
    completed = rows()
    for _ in range(2):
        client.send("Undo"); assert rows() == baseline
        client.send("Redo"); assert rows() == completed


_CAPTION_CHECKS = {
    "video-caption-interchange": check_caption_interchange,
    "video-caption-english": check_caption_english,
    "video-caption-hebrew": check_caption_hebrew,
}
CAPTION_CASES = tuple(_CAPTION_CHECKS)


def check_video_caption(client, root, ffmpeg, ffprobe, case, original, audio):
    """Dispatch one independent journey selected by the normal native harness."""
    _CAPTION_CHECKS[case](client, root, ffmpeg, ffprobe, original, audio)


def caption_test_inputs(case: str, executable: Path) -> dict[str, Any]:
    """Fingerprint relevant check inputs; behavioral evidence is the case result."""
    def sha(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    inputs: dict[str, Any] = {"caption_check_inputs": {
        operation.__name__: hashlib.sha256(inspect.getsource(operation).encode()).hexdigest()
        for operation in (_CAPTION_CHECKS[case], caption_test_inputs, check_video_caption)
    }}
    if case != "video-caption-interchange":
        inputs["caption_worker_sha256"] = sha(executable.parent / "autopsy-whisper.exe")
    if case == "video-caption-hebrew":
        inputs["caption_fixtures"] = {p.name: sha(p) for p in sorted(
            (Path(__file__).resolve().parents[2] / "tests/fixtures/captions").iterdir()) if p.is_file()}
    return inputs
