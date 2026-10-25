"""Neutral flash/click cues detect accumulated export drift independently of PTS.

The native harness owns this case's process, display, files and finite result
archive. The source uses NTSC-rate picture and stationary 48-kHz audio; exporting
many source-frame cuts at 30 fps must keep every cue within one output frame.
"""
import array
import json
import math
import struct
import wave

from run_fork_test_harness import ready_video_preview, run_process


def write_pulse_audio(audio, duration, period):
    """Keep the sound oracle independent of decoded picture timestamps."""
    samples = bytearray()
    for index in range(round(duration * 48000)):
        at = index / 48000
        value = 0.5 * math.sin(2 * math.pi * 1000 * at) if at % period < 0.08 else 0
        samples.extend(struct.pack("<h", round(value * 32767)))
    with wave.open(str(audio), "wb") as stream:
        stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        stream.writeframes(samples)


def write_sync_source(root, ffmpeg):
    """Create generated picture flashes and an independent, stationary WAV."""
    audio = root / "_sync.wav"
    write_pulse_audio(audio, 9.009, 1.001)
    source = root / "_sync.mp4"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                 "color=c=0x404040:s=160x100:r=24000/1001:d=9.009,"
                 "drawbox=color=white:t=fill:enable='lt(mod(n,24),2)'",
                 "-i", str(audio), "-c:v", "libx264", "-preset", "ultrafast", "-crf", "0",
                 "-c:a", "aac", "-b:a", "256k", "-shortest", str(source)], timeout=30)
    return source, audio


def cue_starts(active, rate):
    """Read separated pulse onsets; codec noise inside one pulse cannot add cues."""
    result: list[float] = []
    previous = False
    for index, value in enumerate(active):
        if value and not previous and (not result or index / rate - result[-1] > 0.3):
            result.append(index / rate)
        previous = value
    return result


def read_export_cues(path, ffmpeg, ffprobe):
    """Inspect decoded pixels/samples as well as container duration and geometry."""
    data = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(path)]))
    video = next(stream for stream in data["streams"] if stream["codec_type"] == "video")
    assert (video["width"], video["height"]) == (160, 80), video
    numerator, denominator = map(int, video["avg_frame_rate"].split("/"))
    rate = numerator / denominator
    assert abs(rate - 30) < 1e-6, video
    pixels = run_process([ffmpeg, "-v", "error", "-i", str(path), "-an", "-vf", "scale=1:1",
                          "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"], timeout=30)
    picture = cue_starts([value > 150 for value in pixels], rate)
    border = run_process([ffmpeg, "-v", "error", "-ss", "0.3", "-i", str(path), "-frames:v", "1",
                          "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"], timeout=30)
    assert len(border) == 160 * 80 and min(border) > 25, "Export added black borders to a cropped frame."
    audio = array.array("f", run_process([ffmpeg, "-v", "error", "-i", str(path), "-vn",
                                         "-ar", "48000", "-ac", "1", "-f", "f32le", "pipe:1"], timeout=30))
    # Five-millisecond energy windows tolerate AAC priming and individual sine
    # zero crossings, without allowing a per-cut frame error to accumulate.
    block = 240
    sound = cue_starts([sum(value * value for value in audio[start:start + block]) / block > 0.02
                        for start in range(0, len(audio), block)], 48000 / block)
    assert len(picture) == len(sound) == 9, (picture, sound)
    assert max(abs(a - b) for a, b in zip(picture, sound, strict=True)) <= 1 / rate + 0.006, (picture, sound)
    assert abs(picture[-1] - sound[-1] - (picture[0] - sound[0])) <= 1 / rate + 0.006, (picture, sound)
    return {"picture_cues": picture, "audio_cues": sound, "width": 160, "height": 80}


def check_video_export_timing(client, root, ffmpeg, ffprobe):
    source, audio = write_sync_source(root, ffmpeg)
    client.file("ImportVideo", source)
    client.send('UnlinkMediaClips: Clips="0:0"')
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("RemoveTracks")
    client.file("Import2", audio)
    original_audio = client.data("GetInfo: Type=Clips Format=JSON")
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    for frame in range(6, 216, 6):
        at = frame * 1001 / 24000
        client.send(f"SelectTime: Start={at} End={at}")
        client.send("Split")
    clips = client.data("GetVideoClips")["clips"]
    for clip in clips:
        client.send(f'SetVideoClipCrop: Track=0 Id="{clip["id"]}" Top=0.2')
    first: dict | None = None
    for _ in range(2):
        output = root / "_sync-export.mp4"
        client.file("ExportVideoMix", output, "Overwrite=1 FrameRate=30")
        observed = read_export_cues(output, ffmpeg, ffprobe)
        assert client.data("GetInfo: Type=Clips Format=JSON") == original_audio
        if first is not None:
            assert observed == first
        else:
            first = observed
    client.file("SaveProject2", root / "_sync.aup3")
    check_low_rate_export(client, root, ffmpeg, ffprobe)


def check_low_rate_export(client, root, ffmpeg, ffprobe):
    """Keeping a valid timelapse source rate must also work after picture edits."""
    source = root / "_timelapse.mp4"
    run_process([ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                 "color=c=0x404040:s=160x100:r=1/2:d=6", "-i", str(root / "_sync.wav"),
                 "-t", "6", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "0",
                 "-c:a", "aac", str(source)], timeout=30)
    client.send("SelectAll")
    client.send("RemoveTracks")
    client.file("ImportVideo", source)
    clip = client.data("GetVideoClips")["clips"][0]
    client.send(f'SetVideoClipCrop: Track=0 Id="{clip["id"]}" Top=0.2')
    assert ready_video_preview(client)["ready"], "Timelapse preview did not become ready."
    output = root / "_timelapse-export.mp4"
    for _ in range(2):
        client.file("ExportVideoMix", output, "Overwrite=1")
        data = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(output)]))
        video = next(stream for stream in data["streams"] if stream["codec_type"] == "video")
        assert (video["width"], video["height"]) == (160, 80), video
        numerator, denominator = map(int, video["avg_frame_rate"].split("/"))
        assert numerator / denominator == 0.5 and int(video["nb_frames"]) == 3, video
        assert abs(float(video["duration"]) - 6) < 0.01, video
    client.send("Save")


def check_variable_frame_rate_export(client, root, ffmpeg, ffprobe):
    """Vary real frame durations, cut repeatedly, then compare decoded flash/clicks.

    A nominal-FPS-only fixture would miss phone recordings with uneven PTS. The
    source alternates 15 and 40 fps without changing its wall clock or its WAV.
    Crop forces the actual editing/export path instead of original-stream copy.
    """
    audio, source = root / "_vfr.wav", root / "_vfr.mp4"
    write_pulse_audio(audio, 9, 1)
    run_process([ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                 "color=c=0x404040:s=160x100:r=120:d=9,"
                 "drawbox=color=white:t=fill:enable='lt(mod(t,1),0.08)',"
                 "select='if(lt(mod(t,2),1),not(mod(n,8)),not(mod(n,3)))'",
                 "-i", str(audio), "-fps_mode:v", "vfr", "-c:v", "libx264",
                 "-preset", "ultrafast", "-crf", "0", "-video_track_timescale", "120000",
                 "-c:a", "aac", "-b:a", "256k", "-t", "9", str(source)], timeout=30)
    frames = json.loads(run_process([ffprobe, "-v", "error", "-select_streams", "v:0",
        "-show_frames", "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(source)]))["frames"]
    points = [float(frame["best_effort_timestamp_time"]) for frame in frames]
    deltas = [right - left for left, right in zip(points, points[1:])]
    assert min(deltas) < 0.03 and max(deltas) > 0.06, "Fixture must contain genuinely uneven frame timestamps."
    client.file("ImportVideo", source)
    client.send('UnlinkMediaClips: Clips="0:0"')
    client.send("SelectTracks: Track=1 TrackCount=1 Mode=Set")
    client.send("RemoveTracks")
    client.file("Import2", audio)
    original_audio = client.data("GetInfo: Type=Clips Format=JSON")
    client.send("SelectTracks: Track=0 TrackCount=1 Mode=Set")
    for second in range(9):
        for offset in (0.31, 0.73):
            at = second + offset
            client.send(f"SelectTime: Start={at} End={at}")
            client.send("Split")
    for clip in client.data("GetVideoClips")["clips"]:
        client.send(f'SetVideoClipCrop: Track=0 Id="{clip["id"]}" Top=0.2')
    first = None
    for _ in range(2):
        output = root / "_vfr-export.mp4"
        client.file("ExportVideoMix", output, "Overwrite=1 FrameRate=30")
        observed = read_export_cues(output, ffmpeg, ffprobe)
        assert client.data("GetInfo: Type=Clips Format=JSON") == original_audio
        if first is not None:
            assert observed == first
        first = observed
    client.file("SaveProject2", root / "_vfr.aup3")


def video_hardware_scope():
    """Bind encoding observations to Windows' current adapters and CUDA driver."""
    import ctypes
    import hashlib
    from ctypes import wintypes as w
    from pathlib import Path

    class DisplayDevice(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("name", w.WCHAR * 32), ("description", w.WCHAR * 128),
                    ("state", w.DWORD), ("identifier", w.WCHAR * 128), ("key", w.WCHAR * 128)]

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.EnumDisplayDevicesW.argtypes = [w.LPCWSTR, w.DWORD, ctypes.POINTER(DisplayDevice), w.DWORD]
    adapters = []
    index = 0
    while True:
        device = DisplayDevice()
        device.cb = ctypes.sizeof(device)
        if not user.EnumDisplayDevicesW(None, index, ctypes.byref(device), 0):
            break
        adapters.append({"name": device.name, "description": device.description,
                         "identifier": device.identifier, "state": int(device.state)})
        index += 1
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetSystemDirectoryW.argtypes = [w.LPWSTR, w.UINT]
    directory = ctypes.create_unicode_buffer(32768)
    size = kernel.GetSystemDirectoryW(directory, len(directory))
    if not 0 < size < len(directory):
        raise ctypes.WinError(ctypes.get_last_error())
    driver = Path(directory.value) / "nvcuda.dll"
    digest = None
    if driver.is_file():
        with driver.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"adapters": adapters, "cuda_driver_sha256": digest}


def check_video_encoder_boundaries(client, root, ffmpeg, ffprobe):
    """Keep existing files and edits intact on rejected or unavailable encoders.

    A capable host must also complete and decode the actual hardware export.
    An unavailable host earns only rejection-path coverage, recorded explicitly.
    """
    import subprocess
    import threading
    from test_video_clip_edits import read_owned_native_windows

    def export(settings, *, success):
        replies, finished = [], threading.Event()
        errors: list[BaseException] = []

        def request():
            try:
                replies.append(client.file("ExportVideoMix", output, f"Overwrite=1 {settings}", success=success))
            except BaseException as error:
                errors.append(error)
            finally:
                finished.set()

        worker = threading.Thread(target=request, daemon=True)
        worker.start()
        assert finished.wait(30), (settings, "Export command did not finish", read_owned_native_windows())
        if errors:
            raise errors[0]
        return replies[0]

    source, _audio = write_sync_source(root, ffmpeg)
    client.file("ImportVideo", source)
    clip = client.data("GetVideoClips")["clips"][0]
    client.send(f'SetVideoClipCrop: Track={clip["track"]} Id="{clip["id"]}" Top=0.2')
    audio_before = client.data("GetInfo: Type=Clips Format=JSON")
    picture_before = client.data("GetVideoClips")
    output = root / "_encoder-output.mp4"
    original = b"existing destination must survive a failed export"
    output.write_bytes(original)
    for _ in range(2):
        for settings in ('Preset="unsupported"', 'Preset="h264" Width=159 Height=80',
                         'Preset="h264" Width=-2 Height=80', 'Preset="h264" Width=16386 Height=80',
                         'Preset="h264" FrameRate=-1', 'Preset="h264" FrameRate=0.5',
                         'Preset="h264" FrameRate=1001', 'Preset="h264" Quality=-1',
                         'Preset="h264" Quality=52'):
            export(settings, success=False)
            assert output.read_bytes() == original
            assert client.data("GetInfo: Type=Clips Format=JSON") == audio_before
            assert client.data("GetVideoClips") == picture_before
    command = [ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
               "testsrc2=size=128x72:rate=10", "-frames:v", "3", "-c:v", "h264_nvenc", "-f", "null", "-"]
    try:
        probe = subprocess.run(command, capture_output=True, timeout=20, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"Hardware encoder probe could not finish: {command!r}; {error}") from error
    for _ in range(2):
        reply = export('Preset="h264-nvenc" FrameRate=30', success=probe.returncode == 0)
        if probe.returncode:
            assert "ffmpeg" in reply.lower() or "nvenc" in reply.lower(), reply
            assert output.read_bytes() == original
        else:
            read_export_cues(output, ffmpeg, ffprobe)
            streams = json.loads(run_process([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(output)]))["streams"]
            picture = next(stream for stream in streams if stream["codec_type"] == "video")
            assert "nvenc" in picture.get("tags", {}).get("encoder", "").lower(), picture
        assert client.data("GetInfo: Type=Clips Format=JSON") == audio_before
        assert client.data("GetVideoClips") == picture_before
        assert not list(root.glob("_autopsy-video-export-*")), "Failed/completed export retained staging."
    (root / "_playback.json").write_text(json.dumps({
        "nvenc_export_passed": probe.returncode == 0,
        "unavailable_encoder_rejection_passed": probe.returncode != 0,
        "probe": {"command": command, "exit_status": probe.returncode,
                  "stdout": probe.stdout.decode("utf-8", "replace") or "<absent>",
                  "stderr": probe.stderr.decode("utf-8", "replace") or "<absent>"},
    }), encoding="utf-8")
    client.file("SaveProject2", root / "_encoder-boundaries.aup3")
