"""Select named source/native checks from one input map through SDLC.

The native desktop harness owns its own executable acceptance. This runner reports
native impact separately and never substitutes portable passes for desktop tests.
Result files are immutable check receipts, not saved execution progress: every call
reads current source/build inputs and chooses only missing acceptance. The owner
keeps three receipts per named check at startup and after publication. Pending
files are removed under an exclusive, live-process lock; unrelated files are left
alone. Local and CI callers use this same repository-owned entry point.
The --before-package scope checks an already compiled host before Inno Setup;
native receipts remain with the desktop harness and fixture receipts belong to
the build's bounded _prepackage-results directory.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import uuid
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any


@dataclass(frozen=True)
class CheckGroup:
    name: str
    inputs: tuple[str, ...]
    target: str | None = None


COMMON_INPUTS = (
    "src/video/CMakeLists.txt", "sdlc/sdlc.yml",
    "scripts/pyproject.toml", "scripts/uv.lock", "tests/Catch2Main.cpp",
)
TIMELINE_INPUTS = ("src/video/EditVideoContent.*", "src/video/RenderVideoComposition.*", "src/video/JoinVideoClips.*", "src/video/EditVideoTimeline.h", "src/clip-groups/EditClipMarkers.h")
GROUPS = (
    CheckGroup("video-captions", ("src/video/EditCaptionCues.*", "src/video/TestVideoCaptions.cpp") + TIMELINE_INPUTS,
        "video-captions-test"),
    CheckGroup("native-test-results", ("scripts/test_native_test_results.py",
        "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_native_test_sessions.py")),
    CheckGroup("native-test-sessions", ("scripts/test_native_test_sessions.py",
        "scripts/piped-work/run_native_test_sessions.py", "scripts/piped-work/prepare_test_media.py",
        "scripts/piped-work/test_fork_features.py")),
    CheckGroup("media-collection", ("src/media/CollectMediaFiles.*", "src/media/TestMediaCollection.cpp"),
        "media-collection-test"),
    CheckGroup("media-clip-selection", ("src/clip-groups/SelectMediaClips.h",
        "src/clip-groups/PlanRippleDeletion.h",
        "src/clip-groups/TestMediaClipSelection.cpp"), "media-clip-selection-test"),
    CheckGroup("project-storage", ("libraries/lib-project-file-io/ConfigureProjectStorage.*",
        "libraries/lib-project-file-io/tests/*", "lib-src/sqlite/sqlite3.c", "lib-src/sqlite/sqlite3.h"),
        "project-storage-test"),
    CheckGroup("cursor-recording", ("src/recording/PlanCursorRecording.*",
        "src/recording/TestCursorRecording.cpp", "src/recording/CMakeLists.txt"), "cursor-recording-test"),
    CheckGroup("video-recording", ("src/recording/PlanVideoRecording.*",
        "src/recording/TestVideoRecording.cpp", "src/recording/CMakeLists.txt"), "video-recording-test"),
    CheckGroup("track-groups", ("src/track-groups/EditTrackGroups.*", "src/track-groups/RenderGroupWaveform.*",
        "src/track-groups/TestTrackGroups.cpp", "src/track-groups/CMakeLists.txt"), "track-groups-test"),
    CheckGroup("video-playback", TIMELINE_INPUTS + (
        "src/video/SyncVideoPlayback.h", "src/video/ChoosePreviewQuality.h", "src/video/ChooseVideoExport.h",
        "src/video/RelinkVideoMedia.h", "src/video/TestVideoPlayback.cpp"), "video-playback-test"),
    CheckGroup("video-edits", TIMELINE_INPUTS + (
         "src/video/PlanVideoEdits.*", "src/video/TestVideoEdits.cpp", "src/clip-groups/PlanClipPlacement.h",
         "src/clip-groups/NavigateClipFlags.h"), "video-edits-test"),
    CheckGroup("video-composition", TIMELINE_INPUTS + (
        "src/video/PlanVideoEdits.*", "src/video/EvaluateVideoTimeline.*", "src/video/RenderVideoComposition.*",
        "src/video/PlanVideoExport.*", "src/video/PlanVideoTransitions.*",
        "src/video/TestVideoComposition.cpp", "src/update/ChooseProductUpdate.h"), "video-composition-test"),
    CheckGroup("product-branding-core", (
        "libraries/lib-strings/ApplyProductBranding.*", "libraries/lib-strings/tests/TestProductBranding.cpp"),
        "product-branding-core-test"),
    CheckGroup("fork-installer-build", ("scripts/build/windows/*.py", "win/Inno_Setup_Wizard/*")),
    CheckGroup("fork-display-policy", (
        "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_fork_test_harness.py",
        "scripts/piped-work/pipeclient.py")),
    CheckGroup("fork-worker-lifecycle", (
        "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_fork_test_harness.py",
        "scripts/piped-work/pipeclient.py")),
    CheckGroup("affected-test-selection", (
        "scripts/test_run_affected_tests.py", "scripts/run_affected_tests.py",
        "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_native_test_sessions.py",
        "scripts/piped-work/run_fork_test_harness.py", "scripts/piped-work/pipeclient.py",
        "scripts/piped-work/test_video_clip_edits.py", "scripts/piped-work/test_video_transport.py", "scripts/piped-work/test_preview_controls.py",
        "scripts/piped-work/test_video_generators.py", "scripts/piped-work/test_video_captions.py", "scripts/piped-work/test_product_updates.py")) ,
    CheckGroup("fork-audacity-consumer-runtime", (
        "scripts/piped-work/run_audacity_consumer.py",
        "scripts/piped-work/test_run_audacity_consumer.py",
        "scripts/piped-work/test_fork_features.py")),
    CheckGroup("fork-audacity-consumer-cache", (
        "scripts/piped-work/prepare_audacity_consumer.py",
        "scripts/piped-work/test_prepare_audacity_consumer.py")),
)
# Native bodies are outside portable acceptance. These hints keep their required
# next scope visible without launching a GUI or rebuilding an accepted installer.
NATIVE_INPUTS = (
    "src/video/SlipVideoPicture.*", "scripts/piped-work/test_video_navigation.py",
    "src/video/EditVideoClipAppearance.*", "src/clip-groups/ApplyRippleDeletion.*",
    "src/clip-groups/PlanRippleDeletion.h", "scripts/piped-work/test_clip_workflows.py",
    "src/video/GenerateVideoCaptions.*", "src/video/PrepareCaptionModels.*", "src/video/BuildSpeechEngine.cmake",
    "src/update/CheckProductUpdates.*", "src/AudacityApp.cpp", "src/AboutDialog.cpp", "resources/autopsy-release.json",
    "scripts/piped-work/test_product_updates.py",
    "src/recording/RecordVideoInput.*", "scripts/piped-work/test_video_recording.py",
    "libraries/lib-project-file-io/ProjectFileIO.cpp",
    "src/recording/CreateCursorCapture.*",
    "src/video/PrepareVideoEffects.*", "src/video/PrepareGeneratedVideo.*", "src/video/EditVideoFades.*",
    "scripts/piped-work/test_video_generators.py", "scripts/piped-work/test_video_captions.py",
    "src/media/CollectProjectMedia.*",
    "scripts/piped-work/test_beginner_edits.py",
    "src/tracks/playabletrack/wavetrack/ui/WaveClipAdjustBorderHandle.cpp",
    "src/clip-groups/EditClipPlacement.*",
    "src/clip-groups/GuardMediaDialogs.*", "src/commands/ScriptCommandRelay.cpp",
    "src/clip-groups/SelectMediaClips.*", "scripts/piped-work/test_clip_selection.py",
    "src/tracks/ui/TimeShiftHandle.*", "src/tracks/ui/SelectHandle.cpp",
    "src/tracks/ui/AffordanceHandle.*", "src/export/ExportAudioDialog.*",
    "src/tracks/playabletrack/wavetrack/ui/WaveTrackAffordanceControls.cpp",
    "src/tracks/playabletrack/wavetrack/ui/WaveTrackShifter.cpp",
    "src/clip-groups/OpenClipActions.*", "src/ProjectAudioManager.cpp", "src/ProjectAudioManager.h",
    "src/tracks/ui/EditCursorOverlay.cpp", "src/tracks/playabletrack/wavetrack/ui/WaveClipUIUtilities.cpp",
    "scripts/piped-work/test_clip_actions.py",
    "src/track-groups/CreateTrackGroup.*", "src/track-groups/ShowTrackGroups.*",
    "scripts/piped-work/test_track_groups.py", "src/tracks/ui/ChannelView.cpp", "src/TrackPanel.cpp",
    "src/TrackUtilities.cpp", "src/tracks/ui/CommonTrackControls.h",
    "libraries/lib-wave-track/WaveTrack.cpp",
    "scripts/piped-work/test_video_clip_edits.py", "scripts/piped-work/test_video_transport.py",
    "scripts/piped-work/test_video_export_timing.py",
    "scripts/piped-work/test_preview_controls.py",
    "src/video/EditVideoTimeline.cpp", "src/video/ShowVideoPreview.cpp",
    "src/video/PlayVideoMedia.cpp", "src/video/PlayVideoMedia.h",
    "src/video/EditVideoComposition.cpp", "src/video/EditVideoComposition.h",
    "src/video/ShowVideoCrop.*",
    "src/video/CreateVideoTrack.h", "src/video/CreateVideoTrack.cpp",
    "src/video/ShowVideoTrack.h", "src/video/ShowVideoTrack.cpp",
    "src/video/PrepareVideoPreview.h", "src/video/PrepareVideoPreview.cpp",
    "src/video/ExportVideoMix.h", "src/video/ExportVideoMix.cpp",
    "src/video/ChooseVideoExport.cpp", "src/video/RelinkVideoMedia.cpp",
    "src/clip-groups/LinkMediaClips.h", "src/clip-groups/LinkMediaClips.cpp",
    "src/clip-groups/EditClipMarkers.cpp", "src/clip-groups/GroupAudioClips.h",
    "src/clip-groups/GroupAudioClips.cpp",
    "src/menus/*.cpp", "src/commands/GetInfoCommand.cpp",
    "src/toolbars/ControlToolBar.*",
    "src/tracks/ui/CommonTrackControls.cpp",
    "libraries/lib-project-history/*", "libraries/lib-project-file-io/*", "libraries/lib-viewport/*",
    "CMakeLists.txt", "CMakePresets.json",
)
OWNED_INPUTS = ("src/update/ChooseProductUpdate.h", "src/update/CheckProductUpdates.*", "resources/autopsy-release.json", "src/video/*", "src/media/*", "src/clip-groups/*", "src/track-groups/*", "src/recording/*", "scripts/*.py", "scripts/build/windows/*")
DOCUMENT_SUFFIXES = {".md", ".rst"}

# Native names are adapter contracts checked against CTest's structured catalogue.
# Keep desktop impact beside the portable input map, never in the installer or CI.
INSTALLER_BASELINE_TESTS = ("fork-fixtures", "fork-preview", "fork-project-compatibility")
INSTALLER_NATIVE_GROUPS = (
    (("video-slip",), ("src/video/SlipVideoPicture.*", "src/video/EditVideoTimeline.h",
        "src/video/EditVideoContent.*", "src/video/CreateVideoTrack.*", "src/video/JoinVideoClips.*",
        "src/video/ShowVideoTrack.*", "src/clip-groups/LinkMediaClips.*", "scripts/piped-work/test_video_navigation.py")),
    (("clip-flag-navigation",), ("src/clip-groups/NavigateClipFlags.h", "src/clip-groups/EditClipMarkers.*",
        "src/video/CreateVideoTrack.*", "src/video/SlipVideoPicture.*", "scripts/piped-work/test_video_navigation.py")),
    (("video-range-export",), ("src/video/EditVideoTimeline.h", "src/video/ShowVideoPreview.cpp",
        "src/video/ChooseVideoExport.*", "src/video/ExportVideoMix.*", "src/video/PrepareVideoEffects.*",
        "scripts/piped-work/test_clip_workflows.py")),
    (("video-clip-appearance",), ("src/video/EditVideoClipAppearance.*", "src/video/EditVideoContent.*",
        "src/video/CreateVideoTrack.*", "src/video/JoinVideoClips.*", "src/video/ShowVideoTrack.*",
        "scripts/piped-work/test_clip_workflows.py")),
    (("linked-ripple-delete", "delete-gap", "clip-properties"), ("src/clip-groups/PlanRippleDeletion.h",
        "src/clip-groups/ApplyRippleDeletion.*", "src/menus/EditMenus.cpp", "scripts/piped-work/test_clip_workflows.py")),
    (("video-caption-interchange", "video-caption-english", "video-caption-hebrew", "video-caption-warp"), (
        "src/video/EditCaptionCues.*", "src/video/GenerateVideoCaptions.*", "scripts/piped-work/test_video_captions.py")),
    (("video-caption-english", "video-caption-hebrew", "video-caption-warp"), ("src/video/PrepareCaptionModels.*", "src/video/BuildSpeechEngine.cmake")),
    (("video-caption-hebrew",), ("tests/fixtures/captions/*",)),
    (("product-updates",), ("src/update/ChooseProductUpdate.h", "src/update/CheckProductUpdates.*",
        "resources/autopsy-release.json", "src/AudacityApp.cpp", "src/AboutDialog.cpp", "src/menus/HelpMenus.cpp",
        "scripts/piped-work/test_product_updates.py")),
    (("video-reverse", "video-freeze", "video-repeat", "video-pictures", "video-text",
        "video-caption-interchange", "video-caption-english", "video-caption-hebrew", "video-caption-warp"), (
        "src/video/EditVideoContent.*", "src/video/EditVideoTimeline.*", "src/video/PrepareGeneratedVideo.*",
        "src/video/PrepareVideoEffects.*", "src/video/PlanVideoTransitions.*", "src/video/CreateVideoTrack.*",
        "src/video/EditVideoComposition.*", "src/video/ShowVideoTrack.*", "src/video/ShowVideoPreview.cpp",
        "src/video/EvaluateVideoTimeline.*", "src/video/RenderVideoComposition.*", "src/video/JoinVideoClips.*", "src/video/RelinkVideoMedia.*",
        "scripts/piped-work/test_video_generators.py")),
    (("video-embedding", "video-embedding-controls"), (
        "src/video/EditVideoContent.*", "src/video/EditVideoTimeline.*", "src/video/CreateVideoTrack.*",
        "src/video/PlanVideoTransitions.*", "src/video/PrepareVideoEffects.*", "src/video/EditVideoComposition.*",
        "src/video/EvaluateVideoTimeline.*", "src/video/RenderVideoComposition.*", "src/video/ShowVideoCrop.*",
        "src/video/ShowVideoPreview.cpp", "src/video/ShowVideoTrack.*", "src/clip-groups/GroupAudioClips.*",
        "src/clip-groups/LinkMediaClips.*", "scripts/piped-work/test_video_clip_edits.py")),
    (("video-recording", "video-recording-stop", "video-recording-failures", "video-recording-controls"), ("src/recording/*VideoRecording*", "src/recording/RecordVideoInput.*",
        "src/video/ExportVideoMix.*", "src/video/ShowVideoPreview.cpp", "src/clip-groups/LinkMediaClips.*",
        "scripts/piped-work/test_video_recording.py")),
    (("display-guard",), ("modules/scripting/mod-script-pipe/ScripterCallback.cpp",)),
    (("clip-snapping",), ("src/tracks/ui/TimeShiftHandle.*", "src/clip-groups/EditClipPlacement.*")),
    (("video-fades", "preview-quality", "video-export-timing"), (
        "src/video/PlanVideoTransitions.*", "src/video/PrepareVideoEffects.*", "src/video/EditVideoFades.*")),
    (("collect-media",), ("src/media/CollectProjectMedia.*", "src/media/CollectMediaFiles.*")),
    (("delete-gap", "clip-properties", "visual-crop", "clip-selection", "video-linked-split-edges"), (
        "src/clip-groups/EditClipPlacement.*", "src/clip-groups/PlanClipPlacement.h",
        "src/clip-groups/GuardMediaDialogs.*", "src/commands/ScriptCommandRelay.cpp",
        "src/clip-groups/GroupAudioClips.*", "src/clip-groups/LinkMediaClips.*",
        "src/tracks/playabletrack/wavetrack/ui/WaveClipAdjustBorderHandle.cpp",
        "scripts/piped-work/test_beginner_edits.py")),
    (("preview-controls", "preview-quality", "preview-navigation", "video-cursor", "video-transport", "preview-soak", "video-preview-close"), (
        "src/video/ShowVideoPreview.cpp", "src/video/PlayVideoMedia.*", "src/video/PrepareVideoPreview.*",
        "src/video/ChoosePreviewQuality.*", "src/video/SyncVideoPlayback.*",
        "scripts/piped-work/test_preview_controls.py")),
    (("video-selection", "video-edges", "video-linked-edges", "video-split-edges", "video-linked-split-edges",
      "video-clip-edits", "video-linked-clip-edits", "video-clipboard", "clip-selection", "video-audio-parity", "video-audio-join-links"), (
        "src/video/EditVideoTimeline.*", "src/video/CreateVideoTrack.*", "src/video/ShowVideoTrack.*",
        "src/video/PlanVideoEdits.*", "src/video/EditVideoContent.*", "src/video/JoinVideoClips.*",
        "src/clip-groups/SelectMediaClips.*", "src/clip-groups/LinkMediaClips.*",
        "src/tracks/ui/TimeShiftHandle.*", "src/tracks/ui/SelectHandle.cpp", "src/tracks/ui/AffordanceHandle.*",
        "src/tracks/playabletrack/wavetrack/ui/WaveTrackShifter.cpp", "scripts/piped-work/test_clip_selection.py")),
    (("video-crop", "video-crop-controls", "visual-crop", "video-compound-render", "video-parent-links",
      "video-visibility", "video-clip-mute", "video-speed", "video-speed-storage", "video-clip-storage"), (
        "src/video/EditVideoComposition.*", "src/video/ShowVideoCrop.*", "src/video/EvaluateVideoTimeline.*",
        "src/video/RenderVideoComposition.*", "src/video/PlanVideoExport.*")),
    (("clip-actions", "video-clip-export", "edit-cursor-line", "video-export-timing", "video-vfr-export", "video-encoder-boundaries"), (
        "src/clip-groups/OpenClipActions.*", "src/video/ExportVideoMix.*", "src/video/ChooseVideoExport.*",
        "src/export/ExportAudioDialog.*", "src/tracks/ui/EditCursorOverlay.cpp",
        "src/tracks/playabletrack/wavetrack/ui/WaveTrackAffordanceControls.cpp",
        "src/tracks/playabletrack/wavetrack/ui/WaveClipUIUtilities.cpp",
        "src/tracks/ui/BackgroundCell.cpp",
        "scripts/piped-work/test_clip_actions.py", "scripts/piped-work/test_video_export_timing.py")),
    (("tracks", "groups", "video-track-menu", "media"), (
        "src/track-groups/*", "src/clip-groups/GroupAudioClips.*", "src/clip-groups/EditClipMarkers.*",
        "src/video/RelinkVideoMedia.*", "src/tracks/ui/CommonTrackControls.*",
        "scripts/piped-work/test_track_groups.py")),
    (("history-view", "transport-toolbar", "preview-navigation"), (
        "libraries/lib-project-history/*", "libraries/lib-viewport/*", "src/toolbars/ControlToolBar.*")),
)
INSTALLER_SHARED_INPUTS = (
    "src/recording/CreateCursorCapture.*",
    "src/commands/ScriptCommandRelay.cpp",
    "CMakeLists.txt", "CMakePresets.json", "src/video/CMakeLists.txt", "src/menus/*.cpp",
    "src/commands/GetInfoCommand.cpp", "src/ProjectAudioManager.*", "src/TrackPanel.cpp",
    "src/TrackUtilities.cpp", "src/tracks/ui/ChannelView.cpp", "libraries/lib-wave-track/WaveTrack.cpp",
    "libraries/lib-project-file-io/*", "lib-src/sqlite/*", "scripts/uv.lock", "scripts/pyproject.toml",
    "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_fork_test_harness.py",
    "scripts/piped-work/pipeclient.py", "scripts/piped-work/test_video_clip_edits.py", "scripts/piped-work/test_video_transport.py",
)


class CommandFailure(RuntimeError):
    """Keep invocation, runtime, exit status and both child streams together."""

    def __init__(self, command: list[str], result: subprocess.CompletedProcess[str]):
        self.details: dict[str, Any] = {"command": command, "runtime": shutil.which(command[0]) or command[0], "exit_status": result.returncode,
                        "stdout": result.stdout or "<absent>", "stderr": result.stderr or "<absent>"}
        self.diagnostic_output: str | None = None
        self.diagnostic_error: str | None = None
        super().__init__(summarize_command_failure(self.details))


def summarize_command_failure(details: dict[str, Any]) -> str:
    """Describe failed cases without embedding child streams or JSON recursively."""
    command = details.get("command", details.get("argv", ["command"]))
    status = details.get("exit_status", details.get("exit", "unknown"))
    text = "\n".join(str(details.get(stream, "")) for stream in ("stdout", "stderr"))
    names = list(dict.fromkeys(re.findall(
        r"Test\s+#\d+:\s+(.+?)\s+\.{2,}\*\*\*(?:Failed|Timeout|Exception)", text)))
    reasons = [line.strip() for line in text.splitlines() if re.search(
        r"^(?:AssertionError|ValueError|TypeError|RuntimeError):|: error [A-Z]\d+:", line.strip())
        and not line.strip().startswith("RuntimeError: Command:")]
    parts = [f"Command {Path(command[0]).name} exited {status}."]
    if names:
        parts.append("Failed tests: " + ", ".join(name[:100] for name in names[:8]) + ".")
    if reasons:
        parts.append(reasons[-1][:300])
    return " ".join(parts)


def run_command(command: list[str], root: Path, *, timeout: int = 600) -> str:
    try:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=False, timeout=timeout)
    except OSError as error:
        raise CommandFailure(command, subprocess.CompletedProcess(command, -1, "", str(error))) from error
    except subprocess.TimeoutExpired as error:
        def stream_text(value: str | bytes | None) -> str:
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        raise CommandFailure(command, subprocess.CompletedProcess(
            command, -1, stream_text(error.stdout), stream_text(error.stderr) + f"\nTimed out after {timeout} seconds.")) from error
    if result.returncode:
        raise CommandFailure(command, result)
    return result.stdout


def matches(path: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def select_groups(changed: list[str], all_checks: bool = False) -> tuple[list[CheckGroup], list[str]]:
    """Conservative shared-input closure; unknown owned executable inputs block."""
    native = sorted({path for path in changed if matches(path, NATIVE_INPUTS)
                     and not any(matches(path, group.inputs) for group in GROUPS)})
    unknown = [path for path in changed if Path(path).suffix not in DOCUMENT_SUFFIXES
               and matches(path, OWNED_INPUTS) and path not in native
               and not matches(path, COMMON_INPUTS)
               and not any(matches(path, group.inputs) for group in GROUPS)]
    if unknown and not all_checks:
        raise RuntimeError(f"No portable check owns {unknown!r}; declare inputs or choose --all explicitly.")
    if all_checks or any(matches(path, COMMON_INPUTS) for path in changed):
        return list(GROUPS), native
    return [group for group in GROUPS if any(matches(path, group.inputs) for path in changed)], native


def select_qualification_baseline(releases: list[dict[str, Any]], level: str) -> dict[str, Any] | None:
    """Choose the newest product version at the requested or a higher level.

    Published AuTopsy preview releases are beta; explicitly named alpha previews
    remain alpha. Stable releases rank higher, but age alone never wins over a
    newer eligible version. Drafts and releases without our installer/manifest
    are not product acceptance. This selection does not grant any test pass.
    """
    levels = {"alpha": 0, "beta": 1, "stable": 2}
    if level not in levels:
        raise ValueError(f"Unknown qualification level: {level}")
    candidates = []
    for release in releases:
        if release.get("draft") or not release.get("published_at"):
            continue
        tag = str(release.get("tag_name", ""))
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:-autopsy\.(?:(alpha|beta)\.)?(\d+))?", tag)
        if not match:
            continue
        preview = match[5] is not None
        if bool(release.get("prerelease")) != preview:
            raise RuntimeError(f"Published release {tag} has conflicting version/level metadata.")
        actual = (match[4] or "beta") if preview else "stable"
        assets = {item.get("name") for item in release.get("assets", [])}
        if (levels[actual] < levels[level] or "_build-manifest.json" not in assets
                or not any(isinstance(name, str) and re.fullmatch(r"autopsy-win-.+-x64\.exe", name) for name in assets)):
            continue
        version = tuple(int(match[i]) for i in (1, 2, 3)) + (levels[actual], int(match[5] or 0))
        candidates.append((version, {"tag": tag, "level": actual, "release_id": release.get("id"),
                                     "source_revision": release.get("target_commitish")}))
    if not candidates:
        return None
    candidates.sort(key=lambda item: item[0], reverse=True)
    if len(candidates) > 1 and candidates[0][0] == candidates[1][0] and candidates[0][1] != candidates[1][1]:
        raise RuntimeError("Published qualification baselines have an ambiguous product version.")
    return candidates[0][1]


def read_qualification_baseline(root: Path, level: str) -> dict[str, Any] | None:
    """Read current published releases, then resolve only the selected Git tag.

    GitHub's release metadata owns level/publication state; Git owns the commit.
    Every call reads current service state. There is no second release catalog,
    saved execution progress or local override that can select an older release.
    """
    repository = json.loads(run_command(["gh", "repo", "view", "--json", "nameWithOwner"], root))["nameWithOwner"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise RuntimeError("GitHub returned an unusable repository identity.")
    pages = json.loads(run_command(["gh", "api", "--paginate", "--slurp",
                                   f"repos/{repository}/releases?per_page=100"], root))
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise RuntimeError("GitHub release metadata has an unexpected page shape.")
    selected = select_qualification_baseline([release for page in pages for release in page], level)
    if selected is None:
        return None
    tag = selected["tag"]
    if not isinstance(selected["source_revision"], str) or not re.fullmatch(r"[0-9a-f]{40}", selected["source_revision"]):
        raise RuntimeError("The newest eligible release does not identify its immutable source revision.")
    # Fetch exactly the selected public tag. Git refuses a conflicting local tag;
    # never silently move it or fall back to a different comparison base.
    run_command(["git", "fetch", "--no-tags", "origin", f"refs/tags/{tag}:refs/tags/{tag}"], root)
    selected["commit"] = run_command(["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"], root).strip()
    if selected["commit"] != selected["source_revision"]:
        raise RuntimeError("Published qualification metadata and the selected Git tag disagree.")
    return selected


def changed_paths(root: Path, base: str | None, level: str | None = None) -> tuple[list[str], str]:
    """Include staged/unstaged changes, renames, deletions and untracked inputs."""
    if level:
        selected = read_qualification_baseline(root, level)
        if selected is None:
            return [], "all: no published same-or-higher-level baseline"
        if base is not None:
            supplied = run_command(["git", "rev-parse", "--verify", f"{base}^{{commit}}"], root).strip()
            if supplied != selected["commit"]:
                raise RuntimeError("Explicit test base differs from the newest eligible qualification baseline.")
        base = selected["commit"]
        print(json.dumps({"qualification_baseline": selected, "requested_level": level}), file=sys.stderr)
    if base is None:
        try:
            base = run_command(["git", "merge-base", "HEAD", "origin/main"], root).strip()
        except CommandFailure:
            return [], "all: no local comparison base"
    if not base or set(base) == {"0"}:
        return [], "all: initial revision"
    try:
        revision = run_command(["git", "rev-parse", "--verify", f"{base}^{{commit}}"], root).strip()
    except CommandFailure:
        return [], "all: comparison base unavailable in shallow checkout"
    tracked = run_command(["git", "diff", "--name-only", "--no-renames", "-z", revision, "--"], root)
    untracked = run_command(["git", "ls-files", "--others", "--exclude-standard", "-z"], root)
    return sorted(set(filter(None, (tracked + untracked).split("\0")))), revision


def digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_input_identity(root: Path, group: CheckGroup) -> str:
    paths = sorted({path for pattern in COMMON_INPUTS + group.inputs
                    for path in root.glob(pattern) if path.is_file()})
    files = {path.relative_to(root).as_posix(): file_digest(path) for path in paths}
    return digest({"group": group.name, "patterns": COMMON_INPUTS + group.inputs, "files": files})


def check_input_identity(root: Path, group: CheckGroup, test: dict[str, Any], runtime: dict[str, Any]) -> str:
    program = Path(test["command"][0])
    if not program.is_absolute():
        program = Path(shutil.which(str(program)) or root / program)
    return digest({"inputs": source_input_identity(root, group), "test": test, "runtime": runtime,
                   "program": file_digest(program) if program.is_file() else "missing"})


class ReceiptStore:
    """Single-owner, finite check results; no workflow can resume from these files."""

    def __init__(self, root: Path):
        self.root = root
        self.lock = root / "_runner.lock"
        self.stream: IO[bytes] | None = None

    def __enter__(self) -> ReceiptStore:
        self.root.mkdir(parents=True, exist_ok=True)
        # OS locks disappear when a worker dies. A PID/checkpoint lock could be
        # left empty between creation and writing, or collide with a reused PID.
        self.stream = self.lock.open("a+b")
        self.stream.seek(0, os.SEEK_END)
        if self.stream.tell() == 0:
            self.stream.write(b"0")
            self.stream.flush()
        self.stream.seek(0)
        try:
            if sys.platform == "win32":
                import msvcrt
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            self.stream.close()
            self.stream = None
            raise RuntimeError(f"Another affected-test runner owns {self.lock}: {error}") from error
        try:
            self.prune()
        except BaseException as error:
            self.__exit__(type(error), error, error.__traceback__)
            raise
        return self

    def finish_command_report(self, error: BaseException | None) -> None:
        # One current command report belongs to this runner, under the same
        # exclusive owner as its receipts. Success removes it; immutable check
        # receipts retain the evidence that controls acceptance separately.
        report = self.root / "_command-failure.json"
        pending = self.root / ("_pending-" + uuid.uuid4().hex + ".json")
        try:
            if isinstance(error, CommandFailure):
                evidence = dict(error.details)
                native_build = os.environ.get("AUTOPSY_VIDEO_BUILD_DIR")
                child = Path(native_build) / "_test_command_failure.json" if native_build else None
                # Read only the known native producer's report when this exact
                # failing command names it. Keep a reference, never another copy
                # of its complete nested output in our public response.
                if child and str(child) in str(evidence["stdout"]) and child.is_file():
                    try:
                        child_data = json.loads(child.read_text(encoding="utf-8"))
                        evidence["child_diagnostic_output"] = str(child)
                        error.args = (summarize_command_failure(child_data),)
                    except (OSError, ValueError, KeyError, TypeError) as child_error:
                        evidence["child_report_error"] = str(child_error)
                with pending.open("x", encoding="utf-8") as stream:
                    json.dump(evidence, stream, indent=2)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(pending, report)
                error.diagnostic_output = str(report)
            elif error is None:
                report.unlink(missing_ok=True)
        except (OSError, ValueError) as report_error:
            if isinstance(error, CommandFailure):
                error.diagnostic_error = str(report_error)
            else:
                print(json.dumps({"cleanup_error": str(report_error), "diagnostic_output": str(report)}), file=sys.stderr)
        finally:
            try:
                pending.unlink(missing_ok=True)
            except OSError as cleanup_error:
                print(json.dumps({"cleanup_error": str(cleanup_error), "diagnostic_output": str(pending)}), file=sys.stderr)

    def __exit__(self, exception_type: Any, error: BaseException | None, traceback: Any) -> None:
        try:
            self.finish_command_report(error)
        finally:
            if self.stream:
                self.stream.seek(0)
                if sys.platform == "win32":
                    import msvcrt
                    msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
                self.stream.close()
                self.stream = None
        # Keep the one lock inode: unlinking it can let a second process lock a
        # different inode while another caller still holds the old one.

    def prune(self) -> None:
        for pending in self.root.glob("_pending-*.json"):
            pending.unlink()
        for group in self.root.iterdir():
            if not group.is_dir() or not re.fullmatch(r"[0-9a-f]{64}", group.name):
                continue
            records = sorted(group.glob("_result-*.json"),
                             key=lambda p: json.loads(p.read_text(encoding="utf-8"))["finished_ns"],
                             reverse=True)
            for stale in records[3:]:
                stale.unlink()

    def accepted(self, name: str, identity: str) -> dict[str, Any] | None:
        folder = self.root / digest(name)
        matching = []
        for path in folder.glob("_result-*.json"):
            record = json.loads(path.read_text(encoding="utf-8"))
            if record["name"] == name and record["input_identity"] == identity:
                matching.append(record)
        latest = max(matching, key=lambda record: record["finished_ns"], default=None)
        return latest if latest and latest["result"] == "passed" else None

    def publish(self, name: str, identity: str, result: str, evidence: dict[str, Any]) -> dict[str, Any]:
        folder = self.root / digest(name)
        folder.mkdir(exist_ok=True)
        # The exclusive owner orders results by publication, even when the wall
        # clock stalls or goes backwards. File timestamps cannot supply that order.
        previous = max((json.loads(path.read_text(encoding="utf-8"))["finished_ns"]
                        for path in folder.glob("_result-*.json")), default=0)
        record = {"id": uuid.uuid4().hex, "name": name, "input_identity": identity,
                  "result": result, "finished_ns": max(time.time_ns(), previous + 1), "evidence": evidence}
        pending = self.root / f"_pending-{record['id']}.json"
        target = folder / f"_result-{record['id']}.json"
        try:
            with pending.open("x", encoding="utf-8") as stream:
                json.dump(record, stream, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(pending, target)
        finally:
            pending.unlink(missing_ok=True)
        self.prune()
        return record


def run_named_check(root: Path, build: Path, test: dict[str, Any], identity: str,
                    store: ReceiptStore, input_is_current: Callable[[], bool] | None = None) -> dict[str, Any]:
    if input_is_current and not input_is_current():
        raise RuntimeError(f"Inputs changed after compilation for {test['name']}; no acceptance was used.")
    accepted = store.accepted(test["name"], identity)
    if accepted:
        return {"name": test["name"], "result": "reused", "receipt": accepted["id"]}
    command = ["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure",
               "--no-tests=error", "-R", "^" + re.escape(test["name"]) + "$"]
    try:
        run_command(command, root)
        if input_is_current and not input_is_current():
            raise RuntimeError("Inputs changed while the check ran; no passing receipt was published.")
    except (CommandFailure, RuntimeError) as error:
        evidence = error.details if isinstance(error, CommandFailure) else {"error": str(error)}
        record = store.publish(test["name"], identity, "failed", evidence)
        return {"name": test["name"], "result": "failed", "receipt": record["id"],
                "failure": {"message": str(error), "diagnostic_output": str(
                    store.root / digest(test["name"]) / f"_result-{record['id']}.json")}}
    record = store.publish(test["name"], identity, "passed", {"command": command})
    return {"name": test["name"], "result": "passed", "receipt": record["id"]}


def run_selected_tests(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(".build/_core"))
    parser.add_argument("--base", default=os.environ.get("AUTOPSY_TEST_BASE"))
    parser.add_argument("--qualification-level", choices=("alpha", "beta", "stable"),
                        default=os.environ.get("AUTOPSY_TEST_LEVEL"),
                        help="Compare with the newest published version at this level or higher.")
    parser.add_argument("--all", action="store_true", help="Explicitly select every portable check.")
    parser.add_argument("--before-package", type=Path, help="Check an already compiled native payload before Inno Setup.")
    parser.add_argument("--native-build", type=Path, help="Run an explicit named scope from an existing native CTest catalogue.")
    parser.add_argument("--native-tests", help="Additional native CTest expression, or an explicit scope when no base exists.")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    if args.native_build:
        if not args.native_tests or args.before_package:
            raise RuntimeError("Choose --native-tests for exactly one existing native build scope.")
        native_build = args.native_build.resolve()
        catalogue = json.loads(run_command(["ctest", "--test-dir", str(native_build), "-C", "Release",
                                           "--show-only=json-v1"], root))["tests"]
        expression = re.compile(args.native_tests)
        names = [test["name"] for test in catalogue if expression.search(test["name"])]
        if not names or len({test["name"] for test in catalogue}) != len(catalogue):
            raise RuntimeError("Explicit native scope needs unique existing CTest names.")
        results = run_native_catalogue(root, native_build, catalogue, names, collect_receipts=False)
        print(json.dumps({"result": "passed", "scope": "native", "checks": results}))
        return 0
    if args.before_package:
        print(json.dumps(run_prepackage_checks(root, args.before_package.resolve(), args.base, args.native_tests,
                                              args.qualification_level)))
        return 0
    build = args.build_dir.resolve()
    if not build.is_relative_to(root / ".build"):
        raise RuntimeError("Portable build/results must stay inside this worktree's .build directory.")
    for command in ("git", "cmake", "ctest"):
        if not shutil.which(command):
            raise RuntimeError(f"Required test tool is missing: {command}")
    changed, comparison = changed_paths(root, args.base, args.qualification_level)
    groups, native = select_groups(changed, args.all or comparison.startswith("all:"))
    native_executable = os.environ.get("AUTOPSY_TEST_EXECUTABLE")
    if native_executable:
        if os.environ.get("AUTOPSY_VIDEO_BUILD_DIR") or not os.environ.get("AUTOPSY_VIDEO_TEST_NAME"):
            raise RuntimeError("A private runtime needs AUTOPSY_VIDEO_TEST_NAME and no native build directory.")
        print(json.dumps(run_private_runtime_checks(root, build.parent / "_native-runtime-tests",
            Path(native_executable), os.environ["AUTOPSY_VIDEO_TEST_NAME"], comparison)))
        return 0
    if bool(os.environ.get("AUTOPSY_VIDEO_BUILD_DIR")) != bool(os.environ.get("AUTOPSY_VIDEO_TEST_NAME")):
        raise RuntimeError("A native test session needs both AUTOPSY_VIDEO_BUILD_DIR and AUTOPSY_VIDEO_TEST_NAME.")
    native_session = bool(os.environ.get("AUTOPSY_VIDEO_BUILD_DIR") and os.environ.get("AUTOPSY_VIDEO_TEST_NAME"))
    if native_session:
        # Native sessions test their explicitly selected compiled host. Never
        # stamp current-source portable acceptance onto an older core binary.
        groups = []
    if not groups and not native_session:
        print(json.dumps({"result": "no-portable-tests-affected", "comparison": comparison,
                          "native_inputs_requiring_separate_acceptance": native}))
        return 0
    with ReceiptStore(build.parent / "_test-results") as store:
        source_inputs = {group.name: source_input_identity(root, group) for group in groups}
        native_python = Path(sys.base_prefix) / ("python.exe" if os.name == "nt" else "bin/python3")
        if not native_python.is_file():
            raise RuntimeError(f"Persistent standard-library test runtime is missing: {native_python}")
        run_command(["cmake", "-S", "src/video", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                     f"-DPython3_EXECUTABLE:FILEPATH={native_python}"], root)
        targets = sorted({group.target for group in groups if group.target})
        if targets and not native_session:
            run_command(["cmake", "--build", str(build), "--config", "Release", "--parallel", "2",
                         "--target", *targets], root)
        if any(source_input_identity(root, group) != source_inputs[group.name] for group in groups):
            raise RuntimeError("Inputs changed during compilation; no tests were accepted. Start a fresh normal call.")
        catalogue = json.loads(run_command(["ctest", "--test-dir", str(build), "-C", "Release",
                                           "--show-only=json-v1"], root))["tests"]
        runtime = {"python": sys.version, "executable": file_digest(Path(sys.executable)),
                   "platform": platform.platform(), "cmake": run_command(["cmake", "--version"], root),
                   "environment": {key: os.environ.get(key) for key in (
                       "CC", "CXX", "CXXFLAGS", "CFLAGS", "AUTOPSY_VIDEO_BUILD_DIR", "AUTOPSY_VIDEO_TEST_NAME")},
                   "cache": file_digest(build / "CMakeCache.txt")}
        results = []
        for test in catalogue:
            group = next((group for group in groups if test["name"] == group.name
                          or test["name"].startswith(group.name + "::")), None)
            if native_session and test["name"] == "fork-native-media-session":
                # Native harness maintains exact executable/check receipts itself.
                run_command(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure",
                             "--no-tests=error", "-R", "^fork-native-media-session$"], root)
                results.append({"name": test["name"], "result": "passed"})
            elif group:
                identity = check_input_identity(root, group, test, runtime)
                def inputs_still_current(group=group, expected=identity, test=test) -> bool:
                    return check_input_identity(root, group, test, runtime) == expected
                results.append(run_named_check(root, build, test, identity, store, inputs_still_current))
        missing = [group.name for group in groups if not any(
            item["name"] == group.name or item["name"].startswith(group.name + "::") for item in results)]
        if missing and not native_session:
            raise RuntimeError(f"Selected check groups have no compiled/discovered tests: {missing}")
        failed = any(item["result"] == "failed" for item in results)
        print(json.dumps({"result": "failed" if failed else "passed", "scope": "native" if native_session else "portable",
                          "comparison": comparison,
                          "checks": [item for item in results if item["result"] == "failed"] if failed else results,
                          "passed_checks": sum(item["result"] == "passed" for item in results),
                          "reused_checks": sum(item["result"] == "reused" for item in results),
                          "native_inputs_requiring_separate_acceptance": native}))
        return 1 if failed else 0


def select_installer_tests(changed: list[str], catalogue: list[dict[str, Any]],
                           extra: str | None = None, unknown_base: bool = False) -> list[str]:
    """Select desktop impact by declared inputs; absent contracts stop packaging."""
    names = [test["name"] for test in catalogue]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise RuntimeError(f"CTest names need one registration owner before desktop checks: {duplicates}")
    available = {test["name"] for test in catalogue}
    selected = set(INSTALLER_BASELINE_TESTS)
    if unknown_base and not extra:
        raise RuntimeError("Installer desktop scope needs an available comparison base or explicit --native-tests.")
    for path in changed:
        if Path(path).suffix in DOCUMENT_SUFFIXES:
            continue
        owners = [cases for cases, patterns in INSTALLER_NATIVE_GROUPS if matches(path, patterns)]
        if matches(path, INSTALLER_SHARED_INPUTS):
            selected.update(test["name"] for test in catalogue
                            if "--case" in test.get("command", []) and "--executable" in test.get("command", [])
                            and test["name"] != "fork-display-rejection")
        elif owners:
            selected.update("fork-" + case for cases in owners for case in cases)
        elif matches(path, NATIVE_INPUTS) or (matches(path, OWNED_INPUTS)
                and not any(matches(path, group.inputs) for group in GROUPS)):
            raise RuntimeError(f"No installer desktop check owns {path!r}; declare its affected named tests.")
    if extra:
        expression = re.compile(extra)
        additional = {test["name"] for test in catalogue if expression.search(test["name"])
                      and "--case" in test.get("command", []) and "--executable" in test.get("command", [])}
        if not additional:
            raise RuntimeError(f"Installer native selection matches no tests: {extra}")
        selected.update(additional)
    missing = sorted(selected - available)
    if missing:
        raise RuntimeError(f"Required prepackage tests are absent from this build: {missing}")
    return sorted(selected)


def native_payload_identity(catalogue: list[dict[str, Any]]) -> dict[str, str]:
    """Identify the compiled app and bundled tools/modules without launching them."""
    executables = {Path(command[index + 1]).resolve()
                   for test in catalogue if "--executable" in (command := test.get("command", []))
                   for index in [command.index("--executable")]}
    if len(executables) != 1:
        raise RuntimeError("Native CTest catalogue must identify exactly one compiled AuTopsy executable.")
    executable = executables.pop()
    if not executable.is_file():
        raise RuntimeError(f"The compiled AuTopsy executable is missing: {executable}")
    paths = {executable, *(path for path in executable.parent.rglob("*")
                           if path.is_file() and path.suffix.lower() in (".exe", ".dll"))}
    return {path.relative_to(executable.parent).as_posix(): file_digest(path) for path in sorted(paths)}


def read_native_acceptance(output: str, test: dict[str, Any], build: Path) -> dict[str, Any]:
    """Consume the harness's public JSON result and retain original receipt IDs.

    CTest prefixes verbose child lines with a test number. Missing or ambiguous
    JSON blocks packaging; compatibility's application/consumer results remain
    distinct. Only completed immutable records inside this build are consumed.
    """
    command = test["command"]
    case = command[command.index("--case") + 1]
    results = []
    for line in output.splitlines():
        match = re.match(r"^\s*(?:\d+:\s*)?(\{.*\})\s*$", line)
        if match:
            try:
                value = json.loads(match[1])
            except ValueError:
                continue
            if value.get("case") == case and value.get("result") in ("passed", "reused"):
                results.append(value)
    if len(results) != 1:
        raise RuntimeError(f"Native check {test['name']} did not return one completed harness result.")
    receipts = []

    def collect(value: dict[str, Any]) -> None:
        if "receipt" in value and "original_run" in value:
            path = Path(value["receipt"]).resolve()
            if not path.is_relative_to(build):
                raise RuntimeError("Native acceptance must belong to the selected build.")
            record = json.loads(path.read_text(encoding="utf-8"))
            if record.get("result") != "passed" or record.get("run") != value["original_run"]:
                raise RuntimeError("Native result and its completed acceptance record disagree.")
            receipts.append({"id": record["run"], "input_identity": record["input_identity"],
                "check_version": record.get("check_sha256") or digest(record["check_inputs"]),
                "receipt": path.relative_to(build).as_posix(), "receipt_sha256": file_digest(path)})
        else:
            for child in value.values():
                if isinstance(child, dict):
                    collect(child)

    collect(results[0])
    if not receipts:
        raise RuntimeError(f"Native check {test['name']} returned no acceptance identity.")
    return {"name": test["name"], "result": results[0]["result"], "acceptance": receipts}


def run_native_catalogue(root: Path, build: Path, catalogue: list[dict[str, Any]],
                         names: list[str], *, collect_receipts: bool = True) -> list[dict[str, Any]]:
    """Batch compatible project scenarios; keep CTest's special journeys intact."""
    sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
    from run_native_test_sessions import SHARED_PROJECT_CASES

    groups: dict[tuple[str, ...], list[dict[str, Any]]] = {}
    separate = []
    for name in names:
        test = next(test for test in catalogue if test["name"] == name)
        command = test.get("command", [])
        if "--case" in command and len(command) > 1:
            index = command.index("--case")
            case = command[index + 1]
            flags = {value for value in command if value.startswith("--")}
            ordinary_properties = all(item["name"] in {"RUN_SERIAL", "TIMEOUT", "WORKING_DIRECTORY"}
                                      for item in test.get("properties", []))
            if (case in SHARED_PROJECT_CASES and Path(command[1]).name == "test_fork_features.py"
                    and ordinary_properties
                    and flags <= {"--executable", "--work-root", "--case", "--ffmpeg", "--ffprobe"}):
                key = tuple(command[:index] + command[index + 2:])
                groups.setdefault(key, []).append(test)
                continue
        separate.append(test)
    results: list[dict[str, Any]] = []
    for command, tests in groups.items():
        cases = [test["command"][test["command"].index("--case") + 1] for test in tests]
        output = run_command([*command, "--cases", *cases], root, timeout=300 * len(tests) + 120)
        results.extend(read_native_acceptance(output, test, build) for test in tests)
    for test in separate:
        output = run_command(["ctest", "--test-dir", str(build), "-C", "Release", "--verbose",
                              "--no-tests=error", "-R", "^" + re.escape(test["name"]) + "$"], root)
        results.append(read_native_acceptance(output, test, build) if collect_receipts else
                       {"name": test["name"], "result": "passed"})
    return results


def run_private_runtime_checks(root: Path, work_root: Path, executable: Path,
                               expression: str, comparison: str) -> dict[str, Any]:
    """Exercise new runner behavior against an unchanged, privately extracted app.

    The caller owns extraction. A marker distinguishes that payload from an
    installed user's editor; the runner owns only its project/profile outputs.
    This route never rebuilds, installs or substitutes portable acceptance.
    """
    executable = executable.resolve(strict=True)
    marker = executable.parent / "_test_runtime_owner"
    if not marker.is_file() or marker.read_text() != "AuTopsy private test runtime 1":
        raise RuntimeError("Direct runtime checks require a marked private app payload.")
    sys.path.insert(0, str(Path(__file__).parent / "piped-work"))
    from run_native_test_sessions import (
        MODULE_REGISTERED_NATIVE_CASES,
        NATIVE_CASES,
        RECORDING_CASES,
    )

    pattern = re.compile(expression)
    cases = [case for case in (*NATIVE_CASES, *MODULE_REGISTERED_NATIVE_CASES, *RECORDING_CASES)
             if case != "fixtures" and pattern.search("fork-" + case)]
    if not cases:
        raise RuntimeError("The private runtime selection matches no native scenarios.")
    driver = root / "scripts/piped-work/test_fork_features.py"
    tools = ["--ffmpeg", str(executable.parent / "ffmpeg.exe"),
             "--ffprobe", str(executable.parent / "ffprobe.exe")]
    command = [sys.executable, str(driver), "--executable", str(executable),
               "--work-root", str(work_root), *tools]
    output = run_command([*command, "--cases", *cases], root, timeout=300 * len(cases) + 120)
    results = [read_native_acceptance(output, {"name": "fork-" + case,
        "command": [*command, "--case", case]}, work_root.parent) for case in cases]
    return {"result": "passed", "scope": "native", "comparison": comparison, "checks": results}


def run_prepackage_checks(root: Path, build: Path, base: str | None, extra: str | None,
                          level: str | None = None) -> dict[str, Any]:
    """Gate packaging on current checks of the same compiled payload.

    Native cases retain their own original acceptance records. Only the headless
    fixture check uses this runner's existing bounded receipt store. These are
    check outcomes, not workflow checkpoints; every call reads current inputs.
    No installer is needed or created, and no installed editor is accessed.
    """
    catalogue = json.loads(run_command(["ctest", "--test-dir", str(build), "-C", "Release",
                                       "--show-only=json-v1"], root))["tests"]
    changed, comparison = changed_paths(root, base, level)
    if level and comparison.startswith("all:") and not extra:
        extra = "."  # First qualification has no earlier acceptance to inherit.
    selected = select_installer_tests(changed, catalogue, extra, comparison.startswith("all:"))
    executable = next(Path(test["command"][test["command"].index("--executable") + 1]).resolve()
                      for test in catalogue if test["name"] == "fork-preview" and "--executable" in test.get("command", []))
    if not executable.is_relative_to(build):
        raise RuntimeError("Prepackage checks must use the compiled payload inside the selected build.")
    payload = native_payload_identity(catalogue)
    group = CheckGroup("installer-media-fixture", (
        "scripts/piped-work/test_fork_features.py", "scripts/piped-work/run_fork_test_harness.py",
        "scripts/piped-work/pipeclient.py"))
    runtime = {"payload": payload, "python": sys.version, "platform": platform.platform()}
    results = []
    with ReceiptStore(build / "_prepackage-results") as store:
        for name in selected:
            test = next(test for test in catalogue if test["name"] == name)
            if native_payload_identity(catalogue) != payload:
                raise RuntimeError("Compiled payload changed before a prepackage check; packaging is blocked.")
            if name == "fork-fixtures":
                identity = check_input_identity(root, group, test, runtime)
                def fixture_inputs_current(test: dict[str, Any] = test, identity: str = identity) -> bool:
                    return (check_input_identity(root, group, test, runtime) == identity
                            and native_payload_identity(catalogue) == payload)
                result = run_named_check(root, build, test, identity, store, fixture_inputs_current)
                if result["result"] == "failed":
                    raise RuntimeError(f"Required prepackage check failed: {result}")
                results.append(result)
        results.extend(run_native_catalogue(root, build, catalogue,
            [name for name in selected if name != "fork-fixtures"]))
    if native_payload_identity(catalogue) != payload:
        raise RuntimeError("Compiled payload changed during prepackage checks; packaging is blocked.")
    return {"result": "passed", "scope": "prepackage", "comparison": comparison, "qualification_level": level,
            "checks": results, "payload": payload,
            "payload_directory": executable.parent.relative_to(build).as_posix()}


def main(argv: list[str] | None = None) -> int:
    """CLI result boundary: link complete diagnostics, keep routine output short."""
    try:
        return run_selected_tests(argv)
    except (RuntimeError, OSError, ValueError) as error:
        failure: dict[str, Any] = {"result": "failed", "operation": "affected-tests", "error": str(error)}
        if isinstance(error, CommandFailure):
            if error.diagnostic_output:
                failure["diagnostic_output"] = error.diagnostic_output
            else:
                # Failure to write diagnostics must not erase the original
                # invocation, status or either child stream.
                failure["command_failure"] = error.details
            if error.diagnostic_error:
                failure["diagnostic_error"] = error.diagnostic_error
        print(json.dumps(failure), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
