"""Bounded, owned fixture bytes shared by native scenarios.

Each purpose keeps its current immutable output and two predecessors. Pending
generation belongs to this cache and is removed at entry and after interruption.
Scenarios receive copies, so corrupting or relinking an input cannot poison the
next scenario. Model filenames retain the application's decoder contract.
The native runner's process/profile mutex encloses every cache operation.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import uuid
from collections.abc import Callable
from pathlib import Path
from typing import Any

SIGNATURE = "AuTopsy native fixture cache 1"
MAXIMUM_FILE_BYTES = 512 * 1024 * 1024
MAXIMUM_GROUP_BYTES = 1536 * 1024 * 1024


def file_digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class TestMediaCache:
    """Materialize a recipe once, with atomic publication and bounded retention."""

    def __init__(self, root: Path):
        if root.is_symlink() or root.absolute() != root.resolve():
            raise RuntimeError("The fixture cache cannot use a redirected root.")
        self.root = root.resolve()
        owner = self.root / "_owner"
        if self.root.exists():
            if not owner.is_file() or owner.read_text() != SIGNATURE:
                raise RuntimeError("The fixture cache has another owner: " + str(self.root))
        else:
            self.root.mkdir(parents=True)
            owner.write_text(SIGNATURE)
        for path in self.root.glob("_pending-*"):
            marker = path / "_owner"
            if not marker.is_file() or marker.read_text() != SIGNATURE:
                raise RuntimeError("An incomplete fixture has another owner: " + str(path))
            self.remove(path)
        purposes = {self.read(path)["purpose"] for path in self.root.glob("_ready-*")}
        for purpose in purposes:
            self.prune(purpose)

    def remove(self, path: Path) -> None:
        if path.is_symlink() or path.resolve().parent != self.root:
            raise RuntimeError("Fixture cleanup escaped its owned root: " + str(path))
        owner = path / "_owner"
        if owner.is_symlink() or not owner.is_file() or owner.read_text() != SIGNATURE:
            raise RuntimeError("Fixture cleanup refuses another owner's group: " + str(path))
        shutil.rmtree(path)

    def read(self, path: Path) -> dict[str, Any]:
        if path.is_symlink() or path.resolve().parent != self.root:
            raise RuntimeError("A fixture group is outside its owned root.")
        owner = path / "_owner"
        if owner.is_symlink() or not owner.is_file() or owner.read_text() != SIGNATURE:
            raise RuntimeError("A completed fixture group has another owner.")
        data = json.loads((path / "_complete.json").read_text(encoding="utf-8"))
        if (data.get("schema") != "autopsy-test-media.v1"
                or not isinstance(data.get("purpose"), str)
                or not isinstance(data.get("inputs"), dict)
                or not isinstance(data.get("files"), dict)
                or not 1 <= len(data["files"]) <= 16
                or any(not isinstance(name, str) or name in (".", "..", "_owner", "_complete.json")
                       or "/" in name or "\\" in name or ":" in name or not name
                       or not isinstance(value, str) or len(value) != 64
                       for name, value in data["files"].items())):
            raise RuntimeError("A fixture group has an unknown format: " + str(path))
        identity = hashlib.sha256(json.dumps({"purpose": data["purpose"], "inputs": data["inputs"]},
                                             sort_keys=True).encode()).hexdigest()
        if path.name != "_ready-" + identity:
            raise RuntimeError("A fixture group's input identity disagrees with its path.")
        return data

    def files(self, path: Path) -> dict[str, str]:
        result = {}
        size = 0
        for file in sorted(path.iterdir()):
            if file.name in ("_owner", "_complete.json"):
                continue
            if file.is_symlink() or not file.is_file():
                raise RuntimeError("A generated fixture must contain regular files only.")
            count = file.stat().st_size
            size += count
            if not count or count > MAXIMUM_FILE_BYTES or size > MAXIMUM_GROUP_BYTES:
                raise RuntimeError("Generated fixture bytes are outside the cache's bounds.")
            result[file.name] = file_digest(file)
        if not result or len(result) > 16:
            raise RuntimeError("A fixture recipe must produce between one and sixteen files.")
        return result

    def current(self, path: Path) -> bool:
        return self.files(path) == self.read(path)["files"]

    def prune(self, purpose: str, current: Path | None = None) -> None:
        groups = sorted((path for path in self.root.glob("_ready-*")
                         if self.read(path)["purpose"] == purpose),
                        key=lambda path: path.stat().st_mtime_ns, reverse=True)
        keep = ([current] if current is not None else [])
        keep.extend(path for path in groups if path != current)
        for path in keep[3:]:
            self.remove(path)

    def materialize(self, purpose: str, inputs: dict[str, Any],
                    produce: Callable[[Path], object]) -> Path:
        identity = hashlib.sha256(json.dumps({"purpose": purpose, "inputs": inputs},
                                             sort_keys=True).encode()).hexdigest()
        target = self.root / ("_ready-" + identity)
        if target.exists():
            if not self.current(target):
                raise RuntimeError("Cached fixture bytes changed: " + str(target))
            self.prune(purpose, target)
            return target
        pending = self.root / ("_pending-" + uuid.uuid4().hex)
        pending.mkdir()
        (pending / "_owner").write_text(SIGNATURE)
        try:
            produce(pending)
            files = self.files(pending)
            (pending / "_complete.json").write_text(json.dumps({
                "schema": "autopsy-test-media.v1", "purpose": purpose,
                "inputs": inputs, "files": files}, sort_keys=True), encoding="utf-8")
            pending.replace(target)
        finally:
            if pending.exists():
                self.remove(pending)
        self.prune(purpose, target)
        return target

    def latest(self, purpose: str, required: dict[str, Any]) -> Path | None:
        candidates = sorted(self.root.glob("_ready-*"),
                            key=lambda path: path.stat().st_mtime_ns, reverse=True)
        for path in candidates:
            data = self.read(path)
            if data["purpose"] == purpose and all(data["inputs"].get(k) == v for k, v in required.items()):
                if not self.current(path):
                    raise RuntimeError("Cached fixture bytes changed: " + str(path))
                return path
        return None

    def copy_to(self, group: Path, destination: Path) -> None:
        if not self.current(group):
            raise RuntimeError("Cached fixture bytes changed before copying.")
        if destination.is_symlink() or destination.absolute() != destination.resolve():
            raise RuntimeError("A fixture destination cannot be redirected.")
        destination.mkdir(parents=True, exist_ok=True)
        for name, expected in self.read(group)["files"].items():
            target = destination / name
            if target.exists():
                if not target.is_symlink() and target.is_file() and file_digest(target) == expected:
                    continue
                raise RuntimeError("A fixture copy would replace existing output: " + str(target))
            pending = destination / ("_copy-" + name + ".pending")
            if pending.is_symlink():
                raise RuntimeError("A pending fixture copy cannot be redirected.")
            pending.unlink(missing_ok=True)
            try:
                shutil.copy2(group / name, pending)
                if file_digest(pending) != expected:
                    raise RuntimeError("Fixture bytes changed during copying.")
                pending.replace(target)
            finally:
                pending.unlink(missing_ok=True)


def preserve_caption_models(cache: TestMediaCache, profile: Path, executable: Path) -> None:
    """Keep completed weights after the private app stops; never keep job files."""
    worker = executable.parent / "autopsy-whisper.exe"
    source = profile / "_captions"
    if not worker.is_file() or not source.is_dir():
        return
    models = sorted((path for path in source.glob("*.bin")
                     if path.is_file() and not path.is_symlink() and path.stat().st_size),
                    key=lambda path: path.stat().st_mtime_ns, reverse=True)[:3]
    if not models:
        return
    inputs = {"worker": file_digest(worker), "models": {path.name: file_digest(path) for path in models}}

    def produce(target: Path) -> None:
        for path in models:
            shutil.copy2(path, target / path.name)

    cache.materialize("caption-models", inputs, produce)


def seed_caption_models(cache: TestMediaCache, profile: Path, executable: Path) -> None:
    """The application still checks every model against its pinned digest."""
    worker = executable.parent / "autopsy-whisper.exe"
    if worker.is_file():
        group = cache.latest("caption-models", {"worker": file_digest(worker)})
        if group is not None:
            cache.copy_to(group, profile / "_captions")
