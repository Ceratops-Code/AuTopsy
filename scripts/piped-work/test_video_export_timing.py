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

from run_fork_test_harness import run_process


def write_sync_source(root, ffmpeg):
    """Create generated picture flashes and an independent, stationary WAV."""
    audio = root / "_sync.wav"
    samples = bytearray()
    for index in range(round(9.009 * 48000)):
        at = index / 48000
        value = 0.5 * math.sin(2 * math.pi * 1000 * at) if at % 1.001 < 0.08 else 0
        samples.extend(struct.pack("<h", round(value * 32767)))
    with wave.open(str(audio), "wb") as stream:
        stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        stream.writeframes(samples)
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
