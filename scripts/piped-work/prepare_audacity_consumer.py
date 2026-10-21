"""Own pinned, unmodified portable Audacity consumers for AUP3 interoperability.

The native test supplies a dedicated cache directory. One OS lock covers cache
publication, pruning and consumer use; a killed worker releases it automatically.
Only two currently declared packages survive startup/success. Abandoned staging
is removed on entry, archives after extraction, and no logs/checkpoints are kept.
Unmarked directories and links are never adopted or followed. Cached receipts
bind accepted package bytes to their payload; reuse checks integrity only.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import sys
import urllib.request
import uuid
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import IO, Any

OWNER = "AuTopsy portable Audacity consumers 1"
MAX_ARCHIVE_BYTES = 96 * 1024 * 1024
MAX_PAYLOAD_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 12000


@dataclass(frozen=True)
class AudacityPackage:
    version: str
    sha256: str

    @property
    def url(self) -> str:
        return (f"https://github.com/audacity/audacity/releases/download/Audacity-{self.version}/"
                f"audacity-win-{self.version}-64bit.zip")

    @property
    def cache_name(self) -> str:
        return f"_audacity-{self.version}-{self.sha256}"


# Official release assets list these SHA-256 digests. Keep a baseline and the
# chosen upstream-update consumer, never a moving 'latest' URL. Update both the
# supported-release documentation and the interoperability scope when changing.
PINNED_CONSUMERS = (
    AudacityPackage("3.7.8", "900620f6e9bb6a9f6d1c0a1a10b58eaf480c0c8a4bfce134f89e80eddc83979f"),
    AudacityPackage("3.7.9", "6ace99628c02ae345690183950bf76ee7aeacdaa1a3693e98a6f971642cd2da7"),
)


def file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def is_link(path: Path) -> bool:
    """Junctions are links too; Windows directory symlink checks alone miss them."""
    return path.is_symlink() or bool(getattr(path.lstat(), "st_file_attributes", 0) & 0x400)


def remove_cache_tree(path: Path) -> None:
    if is_link(path):
        raise RuntimeError(f"Refusing linked cache entry: {path}")
    shutil.rmtree(path)


def download_archive(package: AudacityPackage, target: Path) -> None:
    """Fetch a bounded official ZIP, rejecting partial/mismatched bytes before use."""
    request = urllib.request.Request(package.url, headers={"User-Agent": "AuTopsy-project-compatibility"})
    digest = hashlib.sha256()
    count = 0
    with urllib.request.urlopen(request, timeout=45) as response, target.open("xb") as output:
        while block := response.read(1024 * 1024):
            count += len(block)
            if count > MAX_ARCHIVE_BYTES:
                raise ValueError("Portable Audacity archive exceeds the download limit.")
            digest.update(block)
            output.write(block)
        output.flush()
        os.fsync(output.fileno())
    if digest.hexdigest() != package.sha256:
        raise ValueError(f"Portable Audacity SHA-256 mismatch for {package.url}.")


def archive_path(name: str) -> PurePosixPath:
    """Reject ambiguous Windows paths before any payload file is written."""
    path = PurePosixPath(name)
    if not name or path.is_absolute() or "\\" in name or not path.parts:
        raise ValueError(f"Unsafe archive path: {name!r}")
    for part in name.rstrip("/").split("/"):
        reserved = part.split(".", 1)[0].upper()
        if (part in ("", ".", "..") or part.endswith((".", " "))
                or any(char in part for char in '<>:"|?*')
                or any(ord(char) < 32 for char in part)
                or reserved in {"CON", "PRN", "AUX", "NUL"}
                or re.fullmatch(r"(?:COM|LPT)[1-9]", reserved)):
            raise ValueError(f"Unsafe archive path: {name!r}")
    return path


def extract_archive(archive: Path, payload: Path) -> tuple[str, dict[str, dict[str, Any]]]:
    """Preflight the whole archive; extract only bounded ordinary files/directories."""
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        if not members or len(members) > MAX_MEMBERS or sum(item.file_size for item in members) > MAX_PAYLOAD_BYTES:
            raise ValueError("Portable Audacity archive exceeds payload limits.")
        seen: dict[str, bool] = {}
        for item in members:
            path = archive_path(item.filename)
            kind = stat.S_IFMT(item.external_attr >> 16)
            if (kind not in (0, stat.S_IFREG, stat.S_IFDIR) or item.flag_bits & 1
                    or item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                raise ValueError(f"Unsupported archive member: {item.filename}")
            key = path.as_posix().casefold()
            if key in seen:
                raise ValueError(f"Duplicate archive member: {item.filename}")
            seen[key] = item.is_dir()
        for name in seen:
            path = PurePosixPath(name)
            if any(parent.as_posix() in seen and not seen[parent.as_posix()] for parent in path.parents):
                raise ValueError(f"Archive file/directory collision: {name}")
        payload.mkdir()
        files: dict[str, dict[str, Any]] = {}
        for item in members:
            relative = archive_path(item.filename).as_posix()
            destination = payload.joinpath(*PurePosixPath(relative).parts)
            if item.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.open(item) as input_file, destination.open("xb") as output:
                shutil.copyfileobj(input_file, output, 1024 * 1024)
            files[relative] = {"bytes": destination.stat().st_size, "sha256": file_sha256(destination)}
    executables = [name for name in files if PurePosixPath(name).name.lower() == "audacity.exe"]
    if len(executables) != 1:
        raise ValueError("The portable package must contain exactly one audacity.exe.")
    executable = PurePosixPath(executables[0])
    module = (executable.parent / "modules/mod-script-pipe.dll").as_posix()
    if module not in files:
        raise ValueError("The portable package has no stock scripting module.")
    return executable.as_posix(), files


class AudacityConsumerCache:
    """Hold the package cache lease until all consumers have been privately copied.

    The owner may keep the lease during the whole native matrix. Another worker
    fails clearly instead of pruning or modifying a package currently in use.
    Fresh calls derive needed effects from current files, never saved progress.
    """

    def __init__(self, root: Path, packages: tuple[AudacityPackage, ...] = PINNED_CONSUMERS):
        self.root = root.absolute()
        self.packages = packages
        self.stream: IO[bytes] | None = None
        if not 1 <= len(packages) <= 2 or len({item.cache_name for item in packages}) != len(packages):
            raise ValueError("Declare one or two distinct portable Audacity consumers.")
        if any(not re.fullmatch(r"\d+\.\d+\.\d+", item.version)
               or not re.fullmatch(r"[0-9a-f]{64}", item.sha256) for item in packages):
            raise ValueError("Consumer versions and SHA-256 digests must be explicit.")

    def __enter__(self) -> AudacityConsumerCache:
        for ancestor in (self.root, *self.root.parents):
            if ancestor.exists() and is_link(ancestor):
                raise RuntimeError(f"Refusing linked cache root: {ancestor}")
        self.root.mkdir(parents=True, exist_ok=True)
        marker = self.root / "_owner"
        if marker.exists():
            if is_link(marker) or marker.read_text(encoding="utf-8") != OWNER:
                raise RuntimeError(f"Portable consumer cache has another owner: {self.root}")
        elif any(path.name not in ("_cache.lock", "_owner.pending") for path in self.root.iterdir()):
            raise RuntimeError(f"Refusing unmarked nonempty consumer cache: {self.root}")
        lock = self.root / "_cache.lock"
        if lock.exists() and is_link(lock):
            raise RuntimeError(f"Refusing linked consumer lock: {lock}")
        self.stream = lock.open("a+b")
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
            raise RuntimeError(f"Another portable consumer session owns {self.root}: {error}") from error
        try:
            # The owner signature is written atomically below while locked.
            # A crash before replacement leaves a known staging name, not a
            # partially written owner marker that would require manual repair.
            pending_owner = self.root / "_owner.pending"
            pending_owner.unlink(missing_ok=True)
            if not marker.exists():
                pending_owner.write_text(OWNER, encoding="utf-8")
                os.replace(pending_owner, marker)
            self.prune_owned_outputs()
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
        # Keep this lock inode. Unlinking it would permit two simultaneous owners.

    def prune_owned_outputs(self) -> None:
        if not self.stream:
            raise RuntimeError("Cache cleanup requires its live lease.")
        active = {package.cache_name for package in self.packages}
        for path in self.root.iterdir():
            if re.fullmatch(r"_pending-[0-9a-f]{32}", path.name):
                remove_cache_tree(path)
            elif re.fullmatch(r"_audacity-\d+\.\d+\.\d+-[0-9a-f]{64}", path.name) and path.name not in active:
                if is_link(path):
                    raise RuntimeError(f"Refusing linked package: {path}")
                receipt = path / "_receipt.json"
                if receipt.is_file() and not is_link(receipt):
                    record = json.loads(receipt.read_text(encoding="utf-8"))
                    if record.get("owner") == OWNER:
                        remove_cache_tree(path)

    def accepted_payload(self, target: Path, package: AudacityPackage) -> dict[str, Any] | None:
        if not target.exists():
            return None
        if is_link(target):
            raise RuntimeError(f"Refusing linked consumer package: {target}")
        try:
            receipt = target / "_receipt.json"
            if is_link(receipt):
                raise RuntimeError(f"Refusing linked package receipt: {receipt}")
            record = json.loads(receipt.read_text(encoding="utf-8"))
            if record.get("owner") != OWNER or record.get("package") != asdict(package):
                return None
            payload = target / "_payload"
            actual: dict[str, dict[str, Any]] = {}
            if is_link(payload):
                raise RuntimeError(f"Refusing linked package payload: {payload}")
            for path in payload.rglob("*"):
                if is_link(path):
                    raise RuntimeError(f"Refusing linked package member: {path}")
                if path.is_file():
                    actual[path.relative_to(payload).as_posix()] = {"bytes": path.stat().st_size, "sha256": file_sha256(path)}
            if actual == record["files"] and record["executable"] in actual:
                archive_path(record["executable"])
                return record
        except (OSError, ValueError, KeyError, TypeError):
            return None
        return None

    def prepare_consumer(self, package: AudacityPackage) -> dict[str, Any]:
        """Return a complete consumer and its exact package identity, or no partial output."""
        if not self.stream or package not in self.packages:
            raise RuntimeError("Consumer preparation requires its declared package and live lease.")
        target = self.root / package.cache_name
        operation = "inspect cached consumer"
        pending = self.root / ("_pending-" + uuid.uuid4().hex)
        try:
            record = self.accepted_payload(target, package)
            if record is None:
                if target.exists():
                    remove_cache_tree(target)
                pending.mkdir()
                archive = pending / "_archive.zip"
                operation = "download portable consumer"
                download_archive(package, archive)
                operation = "extract portable consumer"
                executable, files = extract_archive(archive, pending / "_payload")
                archive.unlink()
                record = {"owner": OWNER, "package": asdict(package), "url": package.url,
                          "executable": executable, "files": files}
                with (pending / "_receipt.json").open("x", encoding="utf-8") as stream:
                    json.dump(record, stream, sort_keys=True)
                    stream.flush()
                    os.fsync(stream.fileno())
                operation = "publish portable consumer"
                os.replace(pending, target)
            self.prune_owned_outputs()
            return {"executable": target / "_payload" / record["executable"], "version": package.version,
                    "archive_sha256": package.sha256, "url": package.url}
        except Exception as error:
            raise RuntimeError(f"{operation} failed for Audacity {package.version} at {target}: {error}") from error
        finally:
            if pending.exists():
                remove_cache_tree(pending)
