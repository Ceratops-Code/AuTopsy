"""Build AuTopsy installers and publish completed Windows releases.

Each checkout has one installer scratch directory. Build removes abandoned
scratch before compiling and removes its own scratch after success or failure.
Direct builds publish the package in the checkout's Git artifact slot and return
its build data on stdout. Managed output includes the versioned build manifest
used by the publication adapter.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile
from contextlib import contextmanager
from pathlib import Path

MANIFEST_NAME = "AuTopsy-{version}-build-manifest.json"
OWNER = "AuTopsy Windows video dependencies 1"
_process_job: int | None = None
SDK_INPUTS = {
    "mpv": {
        "url": "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-dev-x86_64-20261021-git-e471496c56.7z",
        "sha256": "efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633",
        "files": ["include/mpv/client.h", "libmpv-2.dll"],
        "kind": "7z",
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


def remove_owned_tree(path: Path, root: Path, *, require_sdk_marker: bool = True) -> None:
    """Remove an exact owned child; SDK storage also requires its owner marker."""
    if not os.path.lexists(path):
        return
    if path.is_symlink() or path.is_junction() or path.resolve().parent != root.resolve():
        raise RuntimeError(f"Refusing cleanup outside its owning directory: {path}")
    if not require_sdk_marker:
        # CMake can clone dependencies with read-only Git object packfiles.
        # Clear that attribute only on regular files in this scratch tree.
        for directory, subdirectories, files in path.walk():
            subdirectories[:] = [name for name in subdirectories if not (directory / name).is_junction()]
            for name in files:
                file = directory / name
                mode = file.lstat().st_mode
                if stat.S_ISREG(mode) and not mode & stat.S_IWRITE:
                    file.chmod(mode | stat.S_IWRITE)
        shutil.rmtree(path)
        return
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
                target: str | None = None, child_failure: Path | None = None) -> str:
    """Retain child failures; a selected GitHub GET may report expected absence."""
    print("Running " + subprocess.list2cmdline(arguments), file=sys.stderr, flush=True)
    try:
        ensure_process_tree_cleanup()
        result = subprocess.run(arguments, cwd=source, text=True, encoding="utf-8", errors="backslashreplace", capture_output=True, check=False,
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
        failure: dict[str, object] = {
            "operation": operation, "target": target or str(source), "argv": arguments,
            "exit": result.returncode, "stdout": result.stdout or "<absent>",
            "stderr": result.stderr or "<absent>",
        }
        if child_failure is not None:
            # The installer cleans its scratch tree before returning. Carry the
            # affected-test and native-test evidence through the existing report.
            scratch = diagnostics.parent.resolve()
            child_reports = []
            for _ in range(2):
                try:
                    child_failure = child_failure.resolve()
                    if not child_failure.is_relative_to(scratch):
                        break
                    child = json.loads(child_failure.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    break
                if not isinstance(child, dict):
                    break
                child_reports.append(child)
                nested = child.get("child_diagnostic_output")
                if not isinstance(nested, str) or Path(nested) == child_failure:
                    break
                child_failure = Path(nested)
            if child_reports:
                failure["child_failures"] = child_reports
        write_json(diagnostics, failure)
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
    except OSError as error:
        raise RuntimeError(f"Dependency download failed for {spec['url']}: {error}") from error
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


def get_native_test_python() -> Path:
    """Use the persistent base runtime for standard-library native test clients.

    uv's --with environment can disappear when the build command exits.
    Persisting its executable in CTest makes later sessions impossible. Native
    behavior clients use only the standard library; package tests retain their
    explicitly declared uv project environment.
    """
    executable = Path(sys.base_prefix) / ("python.exe" if os.name == "nt" else "bin/python3")
    if not executable.is_file():
        raise RuntimeError(f"The persistent native-test Python is missing: {executable}")
    return executable.resolve()


def checked_runtime_files(build: Path, report: dict) -> dict[str, str]:
    """Read only the tested build's runtime; packaging must consume these bytes."""
    runtime = (build / report["payload_directory"]).resolve()
    if not runtime.is_relative_to(build) or runtime == build:
        raise RuntimeError("The tested runtime directory must stay inside its build.")
    return {path.relative_to(runtime).as_posix(): sha256(path) for path in sorted(runtime.rglob("*"))
            if path.is_file() and path.suffix.lower() in (".exe", ".dll")}


def native_compile_command(driver: str, build: Path) -> list[str]:
    """Own the native build arguments used by both provenance and execution."""
    return [driver, "--build", str(build), "--config", "Release", "--parallel", "4"]


def compiled_runtime_inputs(source: Path, build: Path, configure: list[str], dependencies: dict,
                            diagnostics: Path) -> dict | None:
    """Read product inputs separately from documentation and native check code.

    Unknown tracked paths remain build inputs. The explicit exclusions cannot
    affect a compiled application; their current code still runs at its normal
    check boundary. Cache settings and tool bytes cover manual build changes.
    """
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return None
    paths = run_command(["git", "ls-files", "-z"], source, diagnostics).split("\0")
    excluded_files = {"README.md", "BUILDING.md", "AGENTS.md", "scripts/run_affected_tests.py",
                      "scripts/test_run_affected_tests.py", "scripts/build/windows/test_build_video_installer.py",
                      "scripts/build/build_video_installer.py"}
    files = {}
    for name in paths:
        if not name or name in excluded_files or name.startswith(("docs/", ".github/", ".ceratops/sdlc/", "scripts/piped-work/")):
            continue
        path = source / name
        files[name] = sha256(path) if path.is_file() else "missing"
    settings = dict(line.split("=", 1) for line in cache.read_text(encoding="utf-8").splitlines()
                    if "=" in line and not line.startswith(("#", "//")))
    # uv regenerates its launcher for each environment. CMake records the real
    # engine it ran in this cache; compare its bytes, not the launcher's embedded
    # temporary Python path or comments naming an installation location.
    native_driver = Path(settings.get("CMAKE_COMMAND:INTERNAL", configure[0]))
    transient_locations = {"CMAKE_COMMAND:INTERNAL", "CMAKE_CPACK_COMMAND:INTERNAL",
                           "CMAKE_CTEST_COMMAND:INTERNAL", "CMAKE_ROOT:INTERNAL"}
    tools = {}
    for key, value in settings.items():
        if key in {"CMAKE_LINKER:FILEPATH", "CMAKE_C_COMPILER:FILEPATH", "CMAKE_CXX_COMPILER:FILEPATH",
                   "CMAKE_MAKE_PROGRAM:FILEPATH", "CMAKE_VS_MSBUILD_COMMAND:INTERNAL"}:
            tool = Path(value)
            tools[value] = sha256(tool) if tool.is_file() else "missing"
            if tool.name.lower() == "link.exe":
                for name in ("cl.exe", "c1.dll", "c1xx.dll", "c2.dll", "mspdbcore.dll"):
                    compiler = tool.with_name(name)
                    tools[str(compiler)] = sha256(compiler) if compiler.is_file() else "missing"
    return {"source_files": files, "configure": configure[1:], "cmake_sha256": sha256(native_driver),
            "build_arguments": native_compile_command(configure[0], build)[1:],
            "cache_settings": {key: value for key, value in settings.items() if key not in transient_locations},
            "compiler_tools": tools, "dependencies": dependencies,
            "compiler_environment": {name: os.environ.get(name) for name in (
                "CL", "_CL_", "CFLAGS", "CXXFLAGS", "LDFLAGS", "INCLUDE", "LIB", "LIBPATH",
                "VCToolsInstallDir", "WindowsSdkDir", "WindowsSDKVersion")}}


def reuse_or_compile_runtime(source: Path, build: Path, configure: list[str], dependencies: dict,
                             revision: str, diagnostics: Path) -> dict | None:
    """Keep the current compiled output when product inputs and bytes match.

    The build root owns one current provenance record and its atomic pending
    sibling. It records a completed artifact, never execution steps or a pass.
    An absent/incomplete record cannot skip compilation; native acceptance is
    separately owned by the test runner and still gates every new installer.
    """
    record = build / "_compiled-runtime.json"
    pending = record.with_name("_" + record.name + ".pending")
    pending.unlink(missing_ok=True)
    inputs = compiled_runtime_inputs(source, build, configure, dependencies, diagnostics)
    if inputs is not None and record.is_file():
        try:
            previous = json.loads(record.read_text(encoding="utf-8"))
            if (previous.get("schema") == "autopsy-compiled-runtime.v1" and previous.get("inputs") == inputs
                    and previous.get("runtime") and previous["runtime"] == checked_runtime_files(build, {"payload_directory": "Release"})):
                return previous
        except (ValueError, KeyError, TypeError):
            pass
    run_command(native_compile_command(configure[0], build), source, diagnostics)
    if inputs is None:
        # A partial configure has no reusable product identity. The required
        # native payload/CTest checks still reject missing or incomplete output.
        return None
    if compiled_runtime_inputs(source, build, configure, dependencies, diagnostics) != inputs:
        raise RuntimeError("Product inputs changed during compilation; no runtime provenance was recorded.")
    runtime = checked_runtime_files(build, {"payload_directory": "Release"})
    if "AuTopsy.exe" not in runtime:
        raise RuntimeError("The native compiler produced no AuTopsy executable.")
    completed = {"schema": "autopsy-compiled-runtime.v1", "source_revision": revision,
                 "inputs": inputs, "runtime": runtime}
    try:
        write_json(record, completed)
    finally:
        pending.unlink(missing_ok=True)
    return completed


def read_product_version(source: Path) -> str:
    """One product identity is embedded in the app, installer and public tag."""
    version = json.loads((source / "resources/autopsy-release.json").read_text(encoding="utf-8")).get("version", "")
    if not isinstance(version, str) or not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-autopsy\.[1-9][0-9]*)?", version):
        raise ValueError("The AuTopsy product release version is missing or malformed.")
    return version


def build_installer(source: Path, build: Path, inno: Path, native_tests: str | None = None, *,
                    artifact_directory: Path | None = None, artifact_version: str | None = None) -> dict:
    """Compile and check the runtime, then stage exactly the completed installer."""
    source, build, inno = source.resolve(), build.resolve(), inno.resolve()
    if source == build or source.is_relative_to(build) or not (source / "CMakeLists.txt").is_file():
        raise RuntimeError("Choose a separate build directory and an AuTopsy source checkout.")
    if not inno.is_file():
        raise RuntimeError("Inno Setup 6 is missing; specify --inno-compiler.")
    build.mkdir(parents=True, exist_ok=True)
    dependency_root = build / "_video-dependencies"
    dependency_root.mkdir(exist_ok=True)
    diagnostics = build / "_build_command_failure.json"
    product_version = read_product_version(source)
    version = artifact_version or product_version.replace("-autopsy.", "b")
    revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    dirty = bool(run_command(["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics).strip())
    source_changes = run_command(["git", "diff", "--binary", "HEAD", "--"], source, diagnostics)
    if os.environ.get("GITHUB_ACTIONS") == "true" and (dirty or revision != os.environ.get("GITHUB_SHA")):
        raise RuntimeError("Hosted installer source differs from the workflow's checked-out revision.")
    dependency_inputs = {name: {"url": value["url"], "sha256": value["sha256"]}
                         for name, value in SDK_INPUTS.items()}
    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake is missing from PATH.")
    mpv = dependency_root / "_mpv"
    configure = [
        cmake, "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_CONFIGURATION_TYPES=Release",
        "-Daudacity_has_tests=ON", "-Daudacity_has_video_preview=ON", "-Daudacity_has_networking=OFF",
        "-Daudacity_has_asio_support=OFF", "-Daudacity_bundle_gplv3=ON",
        "-Daudacity_video_mpv_sdk=" + str(mpv),
        "-DINNO_SETUP_COMPILER=" + str(inno),
        "-DPython3_EXECUTABLE:FILEPATH=" + str(get_native_test_python()),
    ]
    checks = [{"name": "fork-fixtures", "result": "passed", "source_revision": revision}]
    package = build / "_artifact"
    prepare_sdk("mpv", SDK_INPUTS["mpv"], dependency_root, source, diagnostics)
    run_command(configure, source, diagnostics)
    compiled = reuse_or_compile_runtime(source, build, configure, dependency_inputs, revision, diagnostics)
    ctest = shutil.which("ctest")
    if not ctest:
        raise RuntimeError("CTest is missing from the current build environment.")
    # The selector and native harness already own check impact and acceptance.
    # Run them on the compiled host, before producing any installer. Inno's
    # target stages prebuilt files; it has no dependency on the application target.
    command = [sys.executable, str(source / "scripts/run_affected_tests.py"), "--before-package", str(build),
               "--qualification-level", "beta"]
    if native_tests:
        command.extend(["--native-tests", native_tests])
    output = run_command(command, source, diagnostics,
                         child_failure=build / "_prepackage-results" / "_command-failure.json")
    report = json.loads(output.strip().splitlines()[-1])
    if (report.get("result") != "passed" or report.get("scope") != "prepackage"
            or not {"fork-fixtures", "fork-preview", "fork-project-compatibility"}.issubset(
                row["name"] for row in report.get("checks", []))
            or any(row.get("result") not in ("passed", "reused") for row in report.get("checks", []))):
        raise RuntimeError("Packaging requires completed native and media-check acceptance.")
    if checked_runtime_files(build, report) != report.get("payload"):
        raise RuntimeError("The runtime differs from the checked payload; packaging is blocked.")
    current_revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    current_changes = run_command(["git", "diff", "--binary", "HEAD", "--"], source, diagnostics)
    if current_revision != revision or current_changes != source_changes:
        raise RuntimeError("Installer source changed during checks; packaging is blocked.")
    package_command = [cmake, "-E", "env", f"AUTOPSY_INSTALLER_OUTPUT={package}",
                       cmake, "--build", str(build), "--config", "Release", "--target", "innosetup"]
    run_command(package_command, source, diagnostics)
    if checked_runtime_files(build, report) != report["payload"]:
        raise RuntimeError("Packaging changed the checked runtime; no accepted manifest was written.")
    descriptor = json.loads((build / "package/_installer-x64.json").read_text(encoding="utf-8"))
    installer = package / descriptor["filename"]
    if installer.parent.resolve() != package.resolve() or not installer.is_file():
        raise RuntimeError("The packager did not produce its declared installer.")
    if not installer.name.startswith("autopsy-win-" + product_version + "-"):
        raise RuntimeError("The installer filename differs from the embedded product version.")
    if sha256(installer) != descriptor["sha256"]:
        raise RuntimeError("The packaged installer differs from its declared identity.")
    final_revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    final_dirty = bool(run_command(["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics).strip())
    final_changes = run_command(["git", "diff", "--binary", "HEAD", "--"], source, diagnostics)
    if final_revision != revision or final_dirty != dirty or final_changes != source_changes:
        raise RuntimeError("Installer source changed during the build; no accepted manifest was written.")
    filename = f"autopsy-win-{version}-x64.exe"
    if installer.name != filename:
        installer.rename(package / filename)
    result = {
        "schema": "autopsy-windows-installer.v1", "source_revision": revision, "source_dirty": dirty,
        "product_version": product_version, "artifact_version": version,
        "filename": filename, "sha256": descriptor["sha256"], "signed": False,
        "dependency_inputs": dependency_inputs, "checks": checks, "prepackage": report,
        "compiled_source_revision": compiled["source_revision"] if compiled else revision,
    }
    if artifact_directory is not None:
        destination = artifact_directory.resolve(strict=True)
        (package / filename).rename(destination / filename)
        write_json(destination / MANIFEST_NAME.format(version=version), result)
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


def test_existing_installer_build(source: Path, build: Path, test_name: str) -> dict:
    """Run selected checks without rebuilding or installing the accepted product.

    Refresh CTest's runtime binding from current authoritative inputs before the
    session. Explicit desktop cases open only an owned private test process;
    the default fixture case opens no application.
    """
    source, build = source.resolve(), build.resolve()
    if not test_name.strip():
        raise ValueError("Choose a nonempty CTest selection.")
    if not (build / "CTestTestfile.cmake").is_file():
        raise RuntimeError(f"Configure the native installer build before testing it: {build}")
    ctest, cmake = shutil.which("ctest"), shutil.which("cmake")
    if not ctest or not cmake:
        raise RuntimeError("CMake and CTest are required in the current build environment.")
    diagnostics = build / "_test_command_failure.json"
    with build_lock(build):
        diagnostics.with_name("_" + diagnostics.name + ".pending").unlink(missing_ok=True)
        run_command([cmake, "-S", str(source), "-B", str(build),
                     "-DPython3_EXECUTABLE:FILEPATH=" + str(get_native_test_python())],
                    source, diagnostics, operation="native test configuration", target=str(build))
        run_command([sys.executable, str(source / "scripts/run_affected_tests.py"),
                     "--native-build", str(build), "--native-tests", test_name], source, diagnostics)
        diagnostics.unlink(missing_ok=True)
    return {"status": "passed", "build_directory": str(build), "tests": test_name}


def publish_installer(source: Path, build: Path, repository: str, tag: str, notes: Path, *,
                      accepted_installer: Path | None = None, accepted_manifest: Path | None = None) -> dict:
    """Converge one unsigned public preview on its already accepted package.

    GitHub state, never saved progress, determines missing effects. The tag binds
    the original build revision; later test/documentation commits do not rebuild
    an unchanged accepted installer. Existing conflicting releases/assets fail
    closed. The build lock protects the two local upload inputs while they are
    read. One current child-failure report belongs to this publication operation.
    Release notes may use LF or CRLF line endings; all other text must match.
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
        package = accepted_installer.parent if accepted_installer is not None else build / "package"
        manifest = accepted_manifest if accepted_manifest is not None else package / "_build-manifest.json"
        accepted = json.loads(manifest.read_text(encoding="utf-8-sig"))
        revision = accepted.get("source_revision", "")
        filename = accepted.get("filename", "")
        installer = package / filename
        if (accepted.get("schema") != "autopsy-windows-installer.v1"
                or accepted.get("source_dirty") is not False or accepted.get("signed") is not False
                or not re.fullmatch(r"[0-9a-f]{40}", revision)
                or not filename.startswith("autopsy-win-") or not filename.endswith("-x64.exe")
                or installer.parent.resolve() != package.resolve()
                or (accepted_installer is not None and installer != accepted_installer)
                or not installer.is_file() or sha256(installer) != accepted.get("sha256")
                or not any(item == {"name": "fork-fixtures", "result": "passed", "source_revision": revision}
                           for item in accepted.get("checks", []))):
            raise RuntimeError("Preview publication requires a clean accepted installer and matching media-check receipt.")

        if tag != "v" + str(accepted.get("product_version", "")):
            raise RuntimeError("The release tag differs from this installer's embedded product version.")

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
        if (release["draft"] or not release["prerelease"]
                or release["body"].replace("\r\n", "\n") != body or release["name"] != title):
            raise RuntimeError("The public preview does not match the requested release metadata.")
        diagnostics.unlink(missing_ok=True)
        return {"schema": "autopsy-preview-publication.v1", "status": "published",
                "url": release["html_url"], "tag": tag, "source_revision": revision,
                "assets": expected, "signed": False}


def get_installer_build_directory() -> Path:
    """Select package storage without activating the native test-session contract."""
    return Path(os.environ.get("AUTOPSY_INSTALLER_BUILD_DIR", "build/_windows-installer"))


def build_ubuntu_installer(source: Path, build: Path, version: str, artifact_directory: Path | None) -> dict:
    """Build a DEB and exercise the installed editor on its private hosted desktop."""
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_OS") != "Linux":
        raise RuntimeError("Run the Ubuntu build and installation checks on the GitHub Ubuntu runner.")
    build.mkdir(parents=True, exist_ok=True)
    diagnostics = build / "_build_command_failure.json"
    revision = run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip()
    dirty = run_command(["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics)
    if dirty or revision != os.environ.get("GITHUB_SHA"):
        raise RuntimeError("Hosted installer source differs from the workflow's checked-out revision.")
    if run_command(["dpkg", "--print-architecture"], source, diagnostics).strip() != "amd64":
        raise RuntimeError("The Ubuntu installer requires an amd64 runner.")
    deb_version = re.sub(r"(?<=\d)(a|b|rc)(?=\d+$)", r"~\1", version)
    package = build / "_artifact"
    filename = f"autopsy-editor_{deb_version}_amd64.deb"
    pending = package / filename
    run_command(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=/usr", "-DCMAKE_INSTALL_LIBDIR=lib",
                 "-Daudacity_has_tests=ON", "-Daudacity_has_video_preview=ON", "-Daudacity_has_networking=OFF",
                 "-Daudacity_use_ffmpeg=loaded", "-Daudacity_bundle_gplv3=ON", "-Dautopsy_package_ubuntu=ON",
                 "-Dautopsy_debian_version=" + deb_version, "-DPython3_EXECUTABLE=" + sys.executable],
                source, diagnostics, operation="Ubuntu installer configuration")
    run_command(["cmake", "--build", str(build), "--parallel", "2"], source, diagnostics,
                operation="Ubuntu editor build")
    run_command(["cpack", "--config", str(build / "CPackConfig.cmake"), "-B", str(package)],
                source, diagnostics, operation="Ubuntu installer packaging")
    if not pending.is_file() or pending.stat().st_size == 0:
        raise RuntimeError("CPack did not produce the expected Ubuntu installer.")
    run_command(["sudo", "apt-get", "-y", "install", str(pending)], source, diagnostics,
                operation="Installing the Ubuntu candidate")
    checks = []
    try:
        media = Path("/usr/lib/autopsy-editor")
        behavior_runner = str(source / "scripts/piped-work/test_fork_features.py")
        catalogue = run_command([sys.executable, behavior_runner, "--list-native-cases", "--native-platform", "ubuntu"],
                                source, diagnostics, operation="Selecting installed Ubuntu media checks").strip()
        if not catalogue:
            raise RuntimeError("The installed Ubuntu app requires named native media checks.")
        for case in catalogue.split(";"):
            output = run_command(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", sys.executable,
                                  behavior_runner, "--case", case,
                                  "--executable", "/usr/bin/autopsy-editor", "--work-root", str(build / "_fork-tests"),
                                  "--ffmpeg", str(media / "ffmpeg"), "--ffprobe", str(media / "ffprobe")],
                                 source, diagnostics, operation=f"Installed Ubuntu media check {case}")
            checks.append({"name": case, "result": "passed", "output": output})
    finally:
        run_command(["sudo", "dpkg", "--remove", "autopsy-editor"], source, diagnostics,
                    operation="Removing the installed Ubuntu candidate")
    if run_command(["git", "rev-parse", "HEAD"], source, diagnostics).strip() != revision or run_command(
            ["git", "status", "--porcelain", "--untracked-files=no"], source, diagnostics):
        raise RuntimeError("Ubuntu installer source changed during the build.")
    digest = sha256(pending)
    # CPack owns its staging tree inside scratch. Publish only the checked DEB.
    for child in package.iterdir():
        if child == pending:
            continue
        if child.is_dir():
            remove_owned_tree(child, package, require_sdk_marker=False)
        else:
            child.unlink()
    if artifact_directory is not None:
        os.link(pending, artifact_directory / filename)
    return {"schema": "autopsy-ubuntu-installer.v1", "source_revision": revision,
            "product_version": read_product_version(source), "artifact_version": version,
            "filename": filename, "sha256": digest, "checks": checks}


def main() -> None:
    # Explicit input wins; otherwise inspect standard installation locations.
    # Tool discovery never installs software or edits the user's PATH.
    inno_default = os.environ.get("AUTOPSY_INNO_COMPILER")
    if not inno_default:
        candidates = [shutil.which("ISCC.exe")]
        for variable, suffix in (("LOCALAPPDATA", "Programs/Inno Setup 6/ISCC.exe"),
                                 ("ProgramFiles(x86)", "Inno Setup 6/ISCC.exe"),
                                 ("ProgramFiles", "Inno Setup 6/ISCC.exe")):
            if os.environ.get(variable):
                candidates.append(str(Path(os.environ[variable]) / suffix))
        inno_default = next((value for value in candidates if value and Path(value).is_file()),
                            str(Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
                                / "Inno Setup 6/ISCC.exe"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path, help="Existing build for tests or preview publication.")
    parser.add_argument("--version", help="Artifact version selected by the SDLC build.")
    parser.add_argument("--receipt-file", type=Path, help="Write the completed installer result to this file.")
    parser.add_argument("--artifact-directory", type=Path, help="Place the installer and its version manifest in this allocated directory.")
    parser.add_argument("--accepted-artifacts", help="Use these accepted package files for installation or publication.")
    parser.add_argument("--install-accepted", action="store_true")
    parser.add_argument("--test-existing-build", action="store_true")
    parser.add_argument("--publish-installer", action="store_true")
    parser.add_argument("--repository", default="Ceratops-Code/AuTopsy")
    parser.add_argument("--release-tag", default=os.environ.get("AUTOPSY_RELEASE_TAG"))
    parser.add_argument("--release-notes", type=Path, default=os.environ.get("AUTOPSY_RELEASE_NOTES"))
    parser.add_argument("--test-name", default=os.environ.get("AUTOPSY_VIDEO_TEST_NAME"),
                        help="Extra prepackage native scope; in test-only mode, defaults to fork-fixtures.")
    parser.add_argument("--inno-compiler", type=Path, default=Path(inno_default))
    args = parser.parse_args()
    if sum((args.test_existing_build, args.publish_installer, args.install_accepted)) > 1:
        parser.error("Choose only one installer operation.")
    if bool(args.artifact_directory) != bool(args.receipt_file):
        parser.error("--artifact-directory and --receipt-file are used together for SDLC builds")
    if args.version and not args.artifact_directory:
        parser.error("--version is supplied by the SDLC build")
    if args.receipt_file and (args.test_existing_build or args.publish_installer or args.install_accepted):
        parser.error("--receipt-file applies only when building the installer")
    windows = sys.platform == "win32"
    ubuntu = sys.platform.startswith("linux")
    if not windows and not ubuntu:
        parser.error("Use a Windows or Ubuntu build host.")
    if not windows and (args.test_existing_build or args.publish_installer or args.install_accepted or args.test_name):
        parser.error("These separate installer operations require Windows.")
    accepted_installer = None
    accepted_manifest = None
    if args.accepted_artifacts is not None:
        if not (args.publish_installer or args.install_accepted):
            parser.error("--accepted-artifacts requires installation or publication")
        mapping = json.loads(args.accepted_artifacts)
        paths = mapping.get("deliverables.packages.windows-installer") if isinstance(mapping, dict) else None
        if not isinstance(paths, list) or len(paths) != 2 or not all(isinstance(path, str) for path in paths):
            parser.error("The accepted AuTopsy package must contain its installer and build manifest.")
        files = [Path(path) for path in paths]
        installers = [path for path in files if path.match("autopsy-win-*-x64.exe")]
        if len(installers) != 1 or not all(path.is_absolute() and path.is_file() for path in files):
            parser.error("The accepted package is missing the AuTopsy Windows installer.")
        accepted_installer = installers[0]
        manifest_paths = [path for path in files if path != accepted_installer]
        manifest_path = manifest_paths[0]
        if (manifest_path.parent != accepted_installer.parent
                or (manifest_path.name != "_build-manifest.json"
                    and not re.fullmatch(r"AuTopsy-[A-Za-z0-9.+-]+-build-manifest\.json", manifest_path.name))):
            parser.error("The build manifest must belong to the accepted installer directory.")
        accepted_manifest = manifest_path
    if args.install_accepted:
        if accepted_installer is None:
            parser.error("Installation requires the accepted package.")
        command = [str(accepted_installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"]
        installed = subprocess.run(command, cwd=args.source_dir, capture_output=True, text=True, errors="replace")
        if installed.returncode:
            raise RuntimeError(f"Installing {command!r} exited {installed.returncode}; "
                               f"stdout: {installed.stdout or '<absent>'}; stderr: {installed.stderr or '<absent>'}")
        print("OK")
        return
    if args.publish_installer:
        if args.test_existing_build or not args.release_tag or not args.release_notes:
            parser.error("Publication requires a release tag and notes, with no test-only mode.")
        try:
            result = publish_installer(args.source_dir, args.build_dir or get_installer_build_directory(),
                                       args.repository, args.release_tag, args.release_notes,
                                       accepted_installer=accepted_installer, accepted_manifest=accepted_manifest)
        except Exception as error:
            raise RuntimeError(f"Preview publication {args.repository}@{args.release_tag} failed: {error}") from error
        print(json.dumps(result))
        return
    if args.test_existing_build:
        print(json.dumps(test_existing_installer_build(args.source_dir, args.build_dir or get_installer_build_directory(),
                                                      args.test_name or "^fork-fixtures$")))
        return
    if args.build_dir is not None:
        parser.error("The installer build selects scratch from the Git checkout.")
    source = args.source_dir.resolve()
    git_paths = []
    for arguments in (("--path-format=absolute", "--git-common-dir"), ("--absolute-git-dir",), ("--abbrev-ref", "HEAD")):
        command = ["git", "rev-parse", *arguments]
        probe = subprocess.run(command, cwd=source, capture_output=True, text=True, errors="replace", check=False)
        if probe.returncode:
            raise RuntimeError(f"Resolving the installer checkout with {command!r} exited {probe.returncode}; "
                               f"stdout: {probe.stdout or '<absent>'}; stderr: {probe.stderr or '<absent>'}")
        git_paths.append(probe.stdout.strip())
    common, registration = Path(git_paths[0]), Path(git_paths[1])
    checkout = "primary" if registration == common else registration.name
    primary = common.parent
    package_type = "windows-installer" if windows else "ubuntu-installer"
    scratch = primary.parent / "tmp" / primary.name / checkout / "_installer-build" / package_type
    version = args.version or read_product_version(source).replace("-autopsy.", "b")
    if not re.fullmatch(r"(?:0|[1-9]\d*)(?:\.(?:0|[1-9]\d*)){2}(?:(?:a|b|rc)\d+)?", version):
        raise RuntimeError(f"Invalid installer artifact version: {version}")
    if args.artifact_directory:
        package = args.artifact_directory.resolve(strict=True)
    else:
        store = common / "ceratops" / "artifacts"
        pool = store / "beta-stable" if git_paths[2] == "release/local" else store / "worktrees" / checkout
        package = pool / "members" / "packages" / package_type / version / "native"
        if package.resolve() != package or package.is_symlink() or package.is_junction():
            raise RuntimeError(f"Unsafe installer destination: {package}")
    if scratch.resolve() != scratch or scratch.is_symlink() or scratch.is_junction():
        raise RuntimeError(f"Unsafe installer scratch: {scratch}")
    remove_owned_tree(scratch, scratch.parent, require_sdk_marker=False)
    try:
        if args.artifact_directory is None and os.path.lexists(package):
            raise RuntimeError(f"Installer output already exists: {package}")
        if windows:
            result = build_installer(source, scratch, args.inno_compiler, args.test_name,
                                     artifact_directory=args.artifact_directory, artifact_version=version)
        else:
            result = build_ubuntu_installer(source, scratch, version, args.artifact_directory)
        if args.receipt_file:
            artifacts = [{"type": package_type, "path": result["filename"]}]
            if windows:
                artifacts.append({"type": "build-manifest", "path": MANIFEST_NAME.format(version=version)})
            report = {"schema": "ceratops-builder-output", "status": "passed",
                      "installationArtifactPath": result["filename"],
                      "artifacts": artifacts}
            args.receipt_file.write_text(json.dumps(report) + "\n", encoding="utf-8")
        else:
            package.parent.mkdir(parents=True, exist_ok=True)
            if os.path.lexists(package):
                raise RuntimeError(f"Installer output already exists: {package}")
            try:
                (scratch / "_artifact").rename(package)
            except FileExistsError as error:
                raise RuntimeError(f"Installer output already exists: {package}") from error
    except Exception:
        diagnostics = scratch / "_build_command_failure.json"
        if diagnostics.is_file():
            print(diagnostics.read_text(encoding="utf-8"), file=sys.stderr, end="")
        raise
    finally:
        remove_owned_tree(scratch, scratch.parent, require_sdk_marker=False)
    print(json.dumps({**result, "artifact_directory": str(package)}, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        print(f"AuTopsy installer operation failed: {error}", file=sys.stderr)
        sys.exit(1)
