"""Select named portable checks from one input map, through the SDLC test action.

The native desktop harness owns its own executable acceptance. This runner reports
native impact separately and never substitutes portable passes for desktop tests.
Result files are immutable check receipts, not saved execution progress: every call
reads current source/build inputs and chooses only missing acceptance. The owner
keeps three receipts per named check at startup and after publication. Pending
files are removed under an exclusive, live-process lock; unrelated files are left
alone. Local and CI callers use this same repository-owned entry point.
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
    "src/video/CMakeLists.txt", "scripts/run_affected_tests.py", "sdlc/sdlc.yml",
    "scripts/pyproject.toml", "scripts/uv.lock", "tests/Catch2Main.cpp",
)
TIMELINE_INPUTS = ("src/video/EditVideoTimeline.h", "src/clip-groups/EditClipMarkers.h")
GROUPS = (
    CheckGroup("video-playback", TIMELINE_INPUTS + (
        "src/video/SyncVideoPlayback.h", "src/video/ChooseVideoExport.h",
        "src/video/RelinkVideoMedia.h", "src/video/TestVideoPlayback.cpp"), "video-playback-test"),
    CheckGroup("video-edits", TIMELINE_INPUTS + (
        "src/video/PlanVideoEdits.*", "src/video/TestVideoEdits.cpp"), "video-edits-test"),
    CheckGroup("video-composition", TIMELINE_INPUTS + (
        "src/video/PlanVideoEdits.*", "src/video/EvaluateVideoTimeline.*",
        "src/video/TestVideoComposition.cpp"), "video-composition-test"),
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
    CheckGroup("affected-test-selection", ("scripts/test_run_affected_tests.py",)),
)
# Native bodies are outside portable acceptance. These hints keep their required
# next scope visible without launching a GUI or rebuilding an accepted installer.
NATIVE_INPUTS = (
    "src/video/EditVideoTimeline.cpp", "src/video/ShowVideoPreview.cpp",
    "src/video/CreateVideoTrack.h", "src/video/CreateVideoTrack.cpp",
    "src/video/ShowVideoTrack.h", "src/video/ShowVideoTrack.cpp",
    "src/video/PrepareVideoPreview.h", "src/video/PrepareVideoPreview.cpp",
    "src/video/ExportVideoMix.h", "src/video/ExportVideoMix.cpp",
    "src/video/ChooseVideoExport.cpp", "src/video/RelinkVideoMedia.cpp",
    "src/clip-groups/LinkMediaClips.h", "src/clip-groups/LinkMediaClips.cpp",
    "src/clip-groups/EditClipMarkers.cpp", "src/clip-groups/GroupAudioClips.h",
    "src/clip-groups/GroupAudioClips.cpp",
    "src/menus/*.cpp", "src/commands/GetInfoCommand.cpp",
    "libraries/lib-project-history/*", "libraries/lib-viewport/*", "CMakeLists.txt", "CMakePresets.json",
)
OWNED_INPUTS = ("src/video/*", "src/clip-groups/*", "scripts/*.py", "scripts/build/windows/*")
DOCUMENT_SUFFIXES = {".md", ".rst"}


class CommandFailure(RuntimeError):
    """Keep invocation, runtime, exit status and both child streams together."""

    def __init__(self, command: list[str], result: subprocess.CompletedProcess[str]):
        self.details: dict[str, Any] = {"command": command, "runtime": shutil.which(command[0]) or command[0], "exit_status": result.returncode,
                        "stdout": result.stdout or "<absent>", "stderr": result.stderr or "<absent>"}
        super().__init__(json.dumps(self.details))


def run_command(command: list[str], root: Path) -> str:
    try:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=False, timeout=600)
    except OSError as error:
        raise CommandFailure(command, subprocess.CompletedProcess(command, -1, "", str(error))) from error
    except subprocess.TimeoutExpired as error:
        def stream_text(value: str | bytes | None) -> str:
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        raise CommandFailure(command, subprocess.CompletedProcess(
            command, -1, stream_text(error.stdout), stream_text(error.stderr) + "\nTimed out after 600 seconds.")) from error
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


def changed_paths(root: Path, base: str | None) -> tuple[list[str], str]:
    """Include staged/unstaged changes, renames, deletions and untracked inputs."""
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
        except BaseException:
            self.__exit__()
            raise
        return self

    def __exit__(self, *_: Any) -> None:
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
            records = sorted(group.glob("_result-*.json"), key=lambda p: p.stat().st_mtime_ns, reverse=True)
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
        record = {"id": uuid.uuid4().hex, "name": name, "input_identity": identity,
                  "result": result, "finished_ns": time.time_ns(), "evidence": evidence}
        folder = self.root / digest(name)
        folder.mkdir(exist_ok=True)
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
        return {"name": test["name"], "result": "failed", "receipt": record["id"], "failure": evidence}
    record = store.publish(test["name"], identity, "passed", {"command": command})
    return {"name": test["name"], "result": "passed", "receipt": record["id"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(".build/_core"))
    parser.add_argument("--base", default=os.environ.get("AUTOPSY_TEST_BASE"))
    parser.add_argument("--all", action="store_true", help="Explicitly select every portable check.")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if not build.is_relative_to(root / ".build"):
        raise RuntimeError("Portable build/results must stay inside this worktree's .build directory.")
    for command in ("git", "cmake", "ctest"):
        if not shutil.which(command):
            raise RuntimeError(f"Required test tool is missing: {command}")
    changed, comparison = changed_paths(root, args.base)
    groups, native = select_groups(changed, args.all or comparison.startswith("all:"))
    if bool(os.environ.get("AUTOPSY_VIDEO_BUILD_DIR")) != bool(os.environ.get("AUTOPSY_VIDEO_TEST_NAME")):
        raise RuntimeError("A native test session needs both AUTOPSY_VIDEO_BUILD_DIR and AUTOPSY_VIDEO_TEST_NAME.")
    native_session = bool(os.environ.get("AUTOPSY_VIDEO_BUILD_DIR") and os.environ.get("AUTOPSY_VIDEO_TEST_NAME"))
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
                          "comparison": comparison, "checks": results,
                          "native_inputs_requiring_separate_acceptance": native}))
        return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as error:
        print(json.dumps({"result": "failed", "operation": "affected-tests", "error": str(error)}), file=sys.stderr)
        raise SystemExit(1) from error
