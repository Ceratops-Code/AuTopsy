"""Build or publish the Windows video installer without installing it.

The selected build directory owns _video-dependencies (one current SDK copy per
component), pending extraction/downloads, and package/_build-manifest.json.
Fresh calls inspect SDK identities, remove only owned abandoned staging, and
let CMake rebuild unmet targets. Publication consumes only a completed installer
and its manifest, inspecting the selected GitHub tag/assets before each effect.
It never replaces a conflicting tag or asset and publishes only after uploads
have matching service-reported SHA-256 identities.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from contextlib import contextmanager
from pathlib import Path

OWNER = "AuTopsy Windows video dependencies 1"
_process_job: int | None = None
SDK_INPUTS = {
    "mpv": {
        "url": "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-dev-x86_64-20261021-git-e471496c56.7z",
        "sha256": "efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633",
        "files": ["include/mpv/client.h", "libmpv-2.dll"],
        "kind": "7z",
    },
    "ffmpeg": {
        "url": "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-20-13-08/ffmpeg-n8.1.3-18-g46d899d7a0-win64-gpl-shared-8.1.zip",
        "sha256": "36a621ed90a46a2c68af52418be101e8e74327d2784bb35b6cf3874d59b00067",
        "files": [
            "ffmpeg.exe", "ffprobe.exe", "avformat-62.dll", "avcodec-62.dll",
            "avutil-60.dll", "swresample-6.dll", "swscale-9.dll",
            "avfilter-11.dll", "avdevice-62.dll", "LICENSE.txt",
        ],
        "kind": "zip",
    },
}


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, data: dict) -> None:
    """Publish one current record; a fresh call discards its private sibling."""
    encoded = json.dumps(data, indent=2, sort_keys=True) + "\n"
    pending = path.with_name("_" + path.name + ".pending")
    pending.unlink(missing_ok=True)
    if path.is_file() and path.read_text(encoding="utf-8") == encoded:
        return
    pending.write_text(encoded, encoding="utf-8")
    pending.replace(path)


def remove_owned_tree(path: Path, root: Path) -> None:
    """Check resolved boundaries and ownership before recursive Windows deletion."""
    if not path.exists():
        return
    if path.is_symlink() or path.resolve().parent != root.resolve():
        raise RuntimeError(f"Refusing cleanup outside dependency root: {path}")
    marker = path / "_owner"
    children = list(path.iterdir())
    if not marker.exists() and not children:
        path.rmdir()
        return
    identity = marker.read_text(encoding="utf-8") if marker.is_file() else None
    # A stop while creating the marker can leave only an empty/prefix marker.
    # No SDK files are written until the complete owner marker is closed.
    if children == [marker] and identity is not None and OWNER.startswith(identity):
        marker.unlink()
        path.rmdir()
        return
    if identity != OWNER:
        raise RuntimeError(f"Dependency directory has another owner: {path}")
    # Keep ownership until the last effect so interruption during recursive
    # cleanup still leaves a directory the next normal call can safely finish.
    for child in children:
        if child == marker:
            continue
        if child.is_dir() and not child.is_symlink():
            if not child.resolve().is_relative_to(path.resolve()):
                raise RuntimeError(f"Refusing cleanup outside owned SDK: {child}")
            shutil.rmtree(child)
        else:
            child.unlink()
    marker.unlink()
    path.rmdir()


def run_command(arguments: list[str], source: Path, diagnostics: Path, *,
                operation: str = "Windows installer build", missing_ok: bool = False,
                target: str | None = None) -> str:
    """Retain child failures; a selected GitHub GET may report expected absence."""
    print("Running " + subprocess.list2cmdline(arguments), flush=True)
    try:
        ensure_process_tree_cleanup()
        result = subprocess.run(arguments, cwd=source, text=True, capture_output=True, check=False,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    except (OSError, RuntimeError) as error:
        record = {"operation": operation, "target": target or str(source),
                  "argv": arguments, "exit": None, "stdout": "<absent>", "stderr": str(error)}
        write_json(diagnostics, record)
        raise RuntimeError(f"Cannot start build command; details: {diagnostics}") from error
    if result.returncode:
        if missing_ok:
            if arguments[:2] == ["gh", "api"] and "HTTP 404" in result.stderr:
                return ""
            if arguments[:3] == ["gh", "release", "view"] and result.stderr.strip() == "release not found":
                return ""
        write_json(diagnostics, {
            "operation": operation, "target": target or str(source), "argv": arguments,
            "exit": result.returncode, "stdout": result.stdout or "<absent>",
            "stderr": result.stderr or "<absent>",
        })
        raise RuntimeError(f"Build command exited {result.returncode}; details: {diagnostics}")
    return result.stdout


def ensure_process_tree_cleanup() -> None:
    """Keep build children inside a Windows job until this worker exits.

    subprocess alone cannot stop MSBuild or an extractor after an abrupt worker
    exit. The unnamed, non-inheritable job handle is deliberately detached from
    Python finalization: the kernel closes it at process exit, preserving the
    worker's exit status while terminating its remaining descendants.
    """
    global _process_job
    if sys.platform != "win32" or _process_job is not None:
        return
    try:
        import win32api
        import win32job

        job = win32job.CreateJobObject(None, "")
        limits = win32job.QueryInformationJobObject(job, win32job.JobObjectExtendedLimitInformation)
        limits["BasicLimitInformation"]["LimitFlags"] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        win32job.SetInformationJobObject(job, win32job.JobObjectExtendedLimitInformation, limits)
        win32job.AssignProcessToJobObject(job, win32api.GetCurrentProcess())
        _process_job = job.Detach()
    except Exception as error:
        raise RuntimeError(f"Cannot contain the Windows build process tree: {error}") from error


def download_archive(spec: dict, archive: Path) -> None:
    pending = archive.with_name("_" + archive.name + ".pending")
    pending.unlink(missing_ok=True)
    if archive.is_file() and sha256(archive) == spec["sha256"]:
        return
    request = urllib.request.Request(spec["url"], headers={"User-Agent": "AuTopsy-build"})
    try:
        with urllib.request.urlopen(request, timeout=60) as incoming, pending.open("wb") as output:
            shutil.copyfileobj(incoming, output)
        if sha256(pending) != spec["sha256"]:
            raise RuntimeError(f"Dependency download has unexpected contents: {spec['url']}")
        pending.replace(archive)
    finally:
        pending.unlink(missing_ok=True)


def prepare_sdk(name: str, spec: dict, root: Path, source: Path, diagnostics: Path) -> Path:
    """Current SDK identity is artifact metadata, never saved execution progress."""
    current, staging = root / ("_" + name), root / ("_" + name + ".pending")
    unpack = root / ("_" + name + ".unpack")
    archive = root / ("_" + name + "." + spec["kind"])
    archive.with_name("_" + archive.name + ".pending").unlink(missing_ok=True)
    for abandoned in (staging, unpack):
        remove_owned_tree(abandoned, root)
    if current.exists() and (
        current.resolve() != current.absolute() or
        not (current / "_owner").is_file() or
        (current / "_owner").read_text(encoding="utf-8") != OWNER
    ):
        raise RuntimeError(f"Dependency directory has another owner: {current}")
    manifest = current / "_identity.json"
    if manifest.is_file():
        try:
            identity = json.loads(manifest.read_text(encoding="utf-8"))
        except ValueError:
            identity = {}
        files = identity.get("files", {}) if isinstance(identity, dict) else {}
        if isinstance(files, dict) and set(files) == set(spec["files"]) and identity.get("archive_sha256") == spec["sha256"] and all(
            (current / filename).is_file() and sha256(current / filename) == digest
            for filename, digest in files.items()
        ):
            return current
    download_archive(spec, archive)
    for directory in (staging, unpack):
        directory.mkdir()
        (directory / "_owner").write_text(OWNER, encoding="utf-8")
    try:
        if spec["kind"] == "zip":
            with zipfile.ZipFile(archive) as package:
                for member in package.infolist():
                    target = (unpack / member.filename).resolve()
                    if not target.is_relative_to(unpack.resolve()):
                        raise RuntimeError("Dependency archive contains an unsafe path.")
                package.extractall(unpack)
        else:
            cmake = shutil.which("cmake")
            if not cmake:
                raise RuntimeError("CMake is missing from PATH.")
            run_command([cmake, "-E", "tar", "xf", str(archive)], unpack, diagnostics)
        for filename in spec["files"]:
            # The archive layout belongs only to this SDK adapter.
            suffix = "/" + filename.lower()
            matches = [p for p in unpack.rglob(Path(filename).name)
                       if p.is_file() and ("/" + p.relative_to(unpack).as_posix().lower()).endswith(suffix)]
            if len(matches) != 1:
                raise RuntimeError(f"Expected one dependency file {filename}, found {len(matches)}.")
            target = staging / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(matches[0], target)
        write_json(staging / "_identity.json", {
            "archive_sha256": spec["sha256"],
            "files": {name: sha256(staging / name) for name in spec["files"]},
        })
        remove_owned_tree(current, root)
        staging.replace(current)
        return current
    finally:
        remove_owned_tree(unpack, root)
        remove_owned_tree(staging, root)


def _build_installer(source: Path, build: Path, inno: Path) -> dict:
    """A normal rerun derives all remaining work from current source and outputs."""
    source, build, inno = source.resolve(), build.resolve(), inno.resolve()
    if source == build or source.is_relative_to(build) or not (source / "CMakeLists.txt").is_file():
        raise RuntimeError("Choose a separate build directory and an AuTopsy source checkout.")
    if not inno.is_file():
        raise RuntimeError("Inno Setup 6 is missing; specify --inno-compiler.")
    build.mkdir(parents=True, exist_ok=True)
    dependency_root = build / "_video-dependencies"
    dependency_root.mkdir(exist_ok=True)
    diagnostics = build / "_build_command_failure.json"
    for pending in (build / "__build_command_failure.json.pending",
                    build / "package/__build-manifest.json.pending"):
        pending.unlink(missing_ok=True)
    revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    dirty = bool(run_command(["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics).strip())
    if os.environ.get("GITHUB_ACTIONS") == "true" and (dirty or revision != os.environ.get("GITHUB_SHA")):
        raise RuntimeError("Hosted installer source differs from the workflow's checked-out revision.")
    dependency_inputs = {name: {"url": value["url"], "sha256": value["sha256"]}
                         for name, value in SDK_INPUTS.items()}
    checks = [{"name": "fork-fixtures", "result": "passed", "source_revision": revision}]
    manifest = build / "package/_build-manifest.json"
    if manifest.is_file() and not dirty:
        try:
            accepted = json.loads(manifest.read_text(encoding="utf-8"))
            completed = build / "package" / accepted["filename"]
            if (accepted.get("schema") == "autopsy-windows-installer.v1"
                    and accepted.get("source_revision") == revision
                    and accepted.get("source_dirty") is False
                    and accepted.get("dependency_inputs") == dependency_inputs
                    and accepted.get("checks") == checks
                    and completed.parent.resolve() == (build / "package").resolve()
                    and completed.is_file() and sha256(completed) == accepted.get("sha256")):
                diagnostics.unlink(missing_ok=True)
                return accepted
        except (ValueError, KeyError, TypeError):
            # An absent or incomplete receipt cannot authorize reuse. The normal
            # build recreates it only after the media check and packaging pass.
            pass
    mpv = prepare_sdk("mpv", SDK_INPUTS["mpv"], dependency_root, source, diagnostics)
    ffmpeg = prepare_sdk("ffmpeg", SDK_INPUTS["ffmpeg"], dependency_root, source, diagnostics)
    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake is missing from PATH.")
    configure = [
        cmake, "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_CONFIGURATION_TYPES=Release",
        "-Daudacity_has_tests=ON", "-Daudacity_has_video_preview=ON", "-Daudacity_has_networking=OFF",
        "-Daudacity_has_asio_support=OFF", "-Daudacity_bundle_gplv3=ON",
        "-Daudacity_video_mpv_sdk=" + str(mpv), "-Daudacity_video_ffmpeg_runtime=" + str(ffmpeg),
        "-DINNO_SETUP_COMPILER=" + str(inno),
    ]
    run_command(configure, source, diagnostics)
    run_command([cmake, "--build", str(build), "--config", "Release", "--parallel", "4"], source, diagnostics)
    ctest = shutil.which("ctest")
    if not ctest:
        raise RuntimeError("CTest is missing from the current build environment.")
    # Source-only CI has no native installer yet. Its portable model and builder
    # tests run separately; this real encoded-media check gates every new package.
    run_command([ctest, "--test-dir", str(build), "-C", "Release", "--output-on-failure",
                 "-R", "^fork-fixtures$", "--no-tests=error"], source, diagnostics)
    run_command([cmake, "--build", str(build), "--config", "Release", "--target", "innosetup"], source, diagnostics)
    descriptor = json.loads((build / "package/_installer-x64.json").read_text(encoding="utf-8"))
    installer = build / "package" / descriptor["filename"]
    if installer.parent.resolve() != (build / "package").resolve() or not installer.is_file():
        raise RuntimeError("The packager did not produce its declared installer.")
    if sha256(installer) != descriptor["sha256"]:
        raise RuntimeError("The packaged installer differs from its declared identity.")
    final_revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    final_dirty = bool(run_command(["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics).strip())
    if final_revision != revision or final_dirty != dirty:
        raise RuntimeError("Installer source changed during the build; no accepted manifest was written.")
    result = {
        "schema": "autopsy-windows-installer.v1", "source_revision": revision, "source_dirty": dirty,
        "filename": descriptor["filename"], "sha256": descriptor["sha256"], "signed": False,
        "dependency_inputs": dependency_inputs, "checks": checks,
    }
    write_json(build / "package/_build-manifest.json", result)
    diagnostics.unlink(missing_ok=True)
    return result


@contextmanager
def build_lock(build: Path):
    """A live worker owns dependency cleanup; process exit releases the lock."""
    build.mkdir(parents=True, exist_ok=True)
    with (build / "_video_build.lock").open("a+b") as stream:
        stream.seek(0, 2)
        if not stream.tell():
            stream.write(OWNER.encode("ascii"))
            stream.flush()
        stream.seek(0)
        if stream.read() != OWNER.encode("ascii"):
            raise RuntimeError("The installer lock file has another owner.")
        stream.seek(0)
        if sys.platform == "win32":
            import msvcrt

            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            stream.seek(0)
            if sys.platform == "win32":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def build_installer(source: Path, build: Path, inno: Path) -> dict:
    source, build = source.resolve(), build.resolve()
    if source == build or source.is_relative_to(build):
        raise RuntimeError("Choose a separate build directory.")
    with build_lock(build):
        return _build_installer(source, build, inno)


def test_existing_installer_build(source: Path, build: Path, test_name: str) -> dict:
    """Run the selected CTest case without compiling, installing, or opening AuTopsy.

    The default release check is the neutral media fixture. Explicit desktop
    selections remain available for a separate interactive test session.
    """
    source, build = source.resolve(), build.resolve()
    if not test_name.strip():
        raise ValueError("Choose a nonempty CTest selection.")
    if not (build / "CTestTestfile.cmake").is_file():
        raise RuntimeError(f"Configure the native installer build before testing it: {build}")
    ctest = shutil.which("ctest")
    if not ctest:
        raise RuntimeError("CTest is missing from the current build environment.")
    diagnostics = build / "_test_command_failure.json"
    with build_lock(build):
        diagnostics.with_name("_" + diagnostics.name + ".pending").unlink(missing_ok=True)
        run_command([ctest, "--test-dir", str(build), "-C", "Release",
                     "--output-on-failure", "-R", test_name, "--no-tests=error"], source, diagnostics)
        diagnostics.unlink(missing_ok=True)
    return {"status": "passed", "build_directory": str(build), "tests": test_name}


def publish_installer(source: Path, build: Path, repository: str, tag: str, notes: Path) -> dict:
    """Converge one unsigned public preview on its already accepted package.

    GitHub state, never saved progress, determines missing effects. The tag binds
    the original build revision; later test/documentation commits do not rebuild
    an unchanged accepted installer. Existing conflicting releases/assets fail
    closed. The build lock protects the two local upload inputs while they are
    read. One current child-failure report belongs to this publication operation.
    """
    source, build, notes = source.resolve(), build.resolve(), notes.resolve()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Choose an explicit GitHub owner/repository.")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+-autopsy\.[0-9]+", tag):
        raise ValueError("Choose an AuTopsy preview tag, for example v3.7.10-autopsy.2.")
    body = notes.read_text(encoding="utf-8")
    if not body.strip():
        raise ValueError("Preview release notes must not be empty.")
    diagnostics = build / "_publish_command_failure.json"
    with build_lock(build):
        diagnostics.with_name("_" + diagnostics.name + ".pending").unlink(missing_ok=True)
        manifest = build / "package/_build-manifest.json"
        accepted = json.loads(manifest.read_text(encoding="utf-8"))
        revision = accepted.get("source_revision", "")
        filename = accepted.get("filename", "")
        installer = manifest.parent / filename
        if (accepted.get("schema") != "autopsy-windows-installer.v1"
                or accepted.get("source_dirty") is not False or accepted.get("signed") is not False
                or not re.fullmatch(r"[0-9a-f]{40}", revision)
                or not filename.startswith("autopsy-win-") or not filename.endswith("-x64.exe")
                or installer.parent.resolve() != manifest.parent.resolve()
                or not installer.is_file() or sha256(installer) != accepted.get("sha256")
                or not any(item == {"name": "fork-fixtures", "result": "passed", "source_revision": revision}
                           for item in accepted.get("checks", []))):
            raise RuntimeError("Preview publication requires a clean accepted installer and matching media-check receipt.")

        def execute(arguments, *, missing=False):
            return run_command(arguments, source, diagnostics,
                               operation="Windows installer publication", missing_ok=missing,
                               target=repository + "@" + tag)

        def read(endpoint, *, missing=False):
            value = execute(["gh", "api", endpoint], missing=missing)
            return json.loads(value) if value else None

        def current_release():
            # The REST tag endpoint only returns published releases. gh owns
            # draft lookup; read the selected release's full asset identities
            # through its exact repository-bounded REST endpoint afterward.
            value = execute(["gh", "release", "view", tag, "--repo", repository,
                             "--json", "apiUrl"], missing=True)
            if not value:
                return None
            url = json.loads(value)["apiUrl"]
            prefix = f"https://api.github.com/repos/{repository}/releases/"
            if not url.lower().startswith(prefix.lower()) or not url[len(prefix):].isdigit():
                raise RuntimeError("GitHub returned a release endpoint outside the selected repository.")
            return read(f"repos/{repository}/releases/" + url[len(prefix):])

        def require_tag(*, optional=False):
            ref = read(f"repos/{repository}/git/ref/tags/{tag}", missing=True)
            if ref is None:
                if optional:
                    return
                raise RuntimeError("The published preview tag is missing.")
            target = ref["object"]
            for _ in range(8):
                if target["type"] != "tag":
                    break
                target = read(f"repos/{repository}/git/tags/{target['sha']}")["object"]
            if target.get("type") != "commit" or target.get("sha") != revision:
                raise RuntimeError("The preview tag already points to a different source revision.")

        execute(["git", "merge-base", "--is-ancestor", revision, "HEAD"])
        require_tag(optional=True)
        title = "AuTopsy " + tag
        release = current_release()
        if release is None:
            execute(["gh", "release", "create", tag, "--repo", repository, "--target", revision,
                     "--draft", "--prerelease", "--title", title, "--notes-file", str(notes)])
        expected = {installer.name: sha256(installer), manifest.name: sha256(manifest)}
        for path in (installer, manifest):
            release = current_release()
            require_tag(optional=True)
            assets = {asset["name"]: asset for asset in release["assets"]}
            if release.get("target_commitish") != revision:
                raise RuntimeError("The preview release targets another source revision.")
            if set(assets) - set(expected):
                raise RuntimeError("The selected preview contains unexpected assets; leave them for owner review.")
            if path.name in assets:
                if assets[path.name].get("digest") != "sha256:" + expected[path.name]:
                    raise RuntimeError("An existing preview asset differs from the accepted package.")
            else:
                if not release["draft"]:
                    raise RuntimeError("A public preview is incomplete; choose a new release tag.")
                execute(["gh", "release", "upload", tag, str(path), "--repo", repository])
        release = current_release()
        require_tag(optional=True)
        if ({asset["name"]: asset.get("digest") for asset in release["assets"]}
                != {name: "sha256:" + digest for name, digest in expected.items()}):
            raise RuntimeError("GitHub has not confirmed both accepted preview assets.")
        if release["draft"]:
            execute(["gh", "release", "edit", tag, "--repo", repository, "--draft=false",
                     "--prerelease", "--title", title, "--notes-file", str(notes)])
        release = current_release()
        require_tag()
        if release["draft"] or not release["prerelease"] or release["body"] != body or release["name"] != title:
            raise RuntimeError("The public preview does not match the requested release metadata.")
        diagnostics.unlink(missing_ok=True)
        return {"schema": "autopsy-preview-publication.v1", "status": "published",
                "url": release["html_url"], "tag": tag, "source_revision": revision,
                "assets": expected, "signed": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--build-dir", type=Path, default=Path(
        os.environ.get("AUTOPSY_VIDEO_BUILD_DIR", ".build/_windows-installer")))
    parser.add_argument("--test-existing-build", action="store_true")
    parser.add_argument("--publish-installer", action="store_true")
    parser.add_argument("--repository", default="Ceratops-Code/AuTopsy")
    parser.add_argument("--release-tag", default=os.environ.get("AUTOPSY_RELEASE_TAG"))
    parser.add_argument("--release-notes", type=Path, default=os.environ.get("AUTOPSY_RELEASE_NOTES"))
    parser.add_argument("--test-name", default=os.environ.get("AUTOPSY_VIDEO_TEST_NAME", "^fork-fixtures$"))
    parser.add_argument("--inno-compiler", type=Path, default=Path(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    ) / "Inno Setup 6/ISCC.exe")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The Windows installer requires a Windows build host.")
    if args.publish_installer:
        if args.test_existing_build or not args.release_tag or not args.release_notes:
            parser.error("Publication requires a release tag and notes, with no test-only mode.")
        try:
            result = publish_installer(args.source_dir, args.build_dir, args.repository,
                                       args.release_tag, args.release_notes)
        except Exception as error:
            raise RuntimeError(f"Preview publication {args.repository}@{args.release_tag} failed: {error}") from error
        print(json.dumps(result))
        return
    if args.test_existing_build:
        print(json.dumps(test_existing_installer_build(args.source_dir, args.build_dir, args.test_name)))
        return
    result = build_installer(args.source_dir, args.build_dir, args.inno_compiler)
    print(json.dumps(result, separators=(",", ":")))
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write("installer=" + str((args.build_dir / "package" / result["filename"]).resolve()) + "\n")
            stream.write("manifest=" + str((args.build_dir / "package/_build-manifest.json").resolve()) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f"Windows installer build failed: {error}", file=sys.stderr)
        sys.exit(1)
