#!/usr/bin/env python3
"""Run repository validation, excluding tests, in the scripts project's uv environment.

SDLC invokes this script with the locked project under scripts.
uv owns dependency synchronization; this entrypoint never installs
dependencies or runs test suites. Failed-check evidence survives for diagnosis
and is removed after success. Test commands belong in SDLC tests operations.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

CHECK_DEFINITIONS = [{'id': 'shellcheck',
  'command': ['shellcheck',
              '--severity=error',
              '--',
              'au4/buildscripts/ci/learn/make_playlists_info_file.sh',
              'au4/buildscripts/ci/learn/push_file_to_s3.sh',
              'au4/buildscripts/ci/linux/ci_all.sh',
              'au4/buildscripts/ci/linux/make_appimage.sh',
              'au4/buildscripts/ci/linux/setup.sh',
              'au4/buildscripts/ci/macos/setup.sh',
              'au4/buildscripts/ci/tools/s3_install.sh',
              'au4/buildscripts/ci/windows/ci_all.sh',
              'au4/buildscripts/packaging/Linux+BSD/mscore.1.preview.sh',
              'au4/buildscripts/packaging/MacOS/make_dmg.sh',
              'linux/AppImage/AppRun.sh',
              'linux/AppImage/apprun-hooks/00-preserve-environment.sh',
              'linux/AppImage/apprun-hooks/install-url-handler.sh',
              'linux/AppImage/apprun-hooks/setup-gtk2-theme.sh',
              'linux/audacity.sh',
              'linux/check_dependencies.sh',
              'linux/create_appimage.sh',
              'linux/packages/arch/dependencies.sh',
              'linux/packages/arch/entrypoint.sh',
              'linux/packages/build_package.sh',
              'linux/packages/build_packages.sh',
              'linux/packages/fedora34/dependencies.sh',
              'linux/packages/fedora34/entrypoint.sh',
              'linux/packages/prepare_offline_dependencies.sh',
              'linux/packages/ubuntu-20.04/dependencies.sh',
              'linux/packages/ubuntu-20.04/entrypoint.sh',
              'linux/ubuntu-focal/dependencies.sh',
              'linux/ubuntu-focal/entrypoint.sh',
              'locale/diagnostics.sh',
              'locale/no_id.sh',
              'locale/update_po_files.sh',
              'modules/import-export/mod-ffmpeg/lib-ffmpeg-support/generator/generate_headers.sh',
              'scripts/build/macOS/build_universal.sh',
              'scripts/build/macOS/hdiutil_wrapper.sh'],
  'cwd': '.',
  'exclusive': False},
 {'id': 'npm-markdown-lint',
  'command': ['{npm}', '--prefix', 'scripts', 'run', 'lint:markdown'],
  'cwd': '.',
  'exclusive': False},
 {'id': 'ruff',
  'command': ['{python}',
              '-m',
              'ruff',
              'check',
              '.',
              '--config',
              'scripts/pyproject.toml'],
  'cwd': '.',
  'exclusive': False},
 {'id': 'mypy',
  'command': ['{python}',
              '-m',
              'mypy',
              '--config-file',
              'scripts/pyproject.toml'],
  'cwd': '.',
  'exclusive': False},
 {'id': 'actionlint',
  'command': ['{python}', 'scripts/run-actionlint.py'],
  'cwd': '.',
  'exclusive': False}]
COMMAND_NOT_FOUND_EXIT_CODE = 127


def command(definition: dict[str, object], temporary_root: pathlib.Path) -> list[str]:
    """Resolve portable executable tokens without shell parsing."""

    npm = "npm.cmd" if sys.platform == "win32" else "npm"
    pnpm = "pnpm.cmd" if sys.platform == "win32" else "pnpm"
    pwsh = "pwsh.exe" if sys.platform == "win32" else "pwsh"
    values = {
        "{python}": sys.executable,
        "{npm}": npm,
        "{pnpm}": pnpm,
        "{pwsh}": pwsh,
        "{temp}": str(temporary_root),
    }
    raw_command = definition["command"]
    if not isinstance(raw_command, list):
        raise TypeError("check command must be a list")
    resolved: list[str] = []
    for raw in raw_command:
        value = str(raw)
        for token, replacement in values.items():
            value = value.replace(token, replacement)
        resolved.append(value)
    return resolved


def prepare_temporary_directories(
    argv: list[str], temporary_root: pathlib.Path
) -> None:
    """Create only contract-declared working directories inside the owned root."""

    resolved_root = temporary_root.resolve()
    for index, value in enumerate(argv[:-1]):
        if value != "--temp-root":
            continue
        path = pathlib.Path(argv[index + 1]).resolve()
        try:
            path.relative_to(resolved_root)
        except ValueError:
            continue
        path.mkdir(parents=True, exist_ok=True)


def cleanup_evidence(
    evidence_file: pathlib.Path,
    *,
    prune_default_parent: bool,
) -> None:
    """Remove stale failure evidence after success and only its owned directory."""

    evidence_file.unlink(missing_ok=True)
    evidence_file.with_name(f".{evidence_file.name}.tmp").unlink(missing_ok=True)
    if prune_default_parent:
        try:
            evidence_file.parent.rmdir()
        except FileNotFoundError:
            pass
        except OSError:
            if not evidence_file.parent.is_dir() or any(
                evidence_file.parent.iterdir()
            ):
                return
            raise


def child_evidence(argv: list[str], temporary_root: pathlib.Path) -> list[str]:
    """Retain declared child-validator evidence before temporary cleanup."""

    paths: list[pathlib.Path] = []
    for index, value in enumerate(argv):
        if value == "--evidence-file" and index + 1 < len(argv):
            paths.append(pathlib.Path(argv[index + 1]))
        elif value.startswith("--evidence-file="):
            paths.append(pathlib.Path(value.partition("=")[2]))
    retained: list[str] = []
    temporary_root = temporary_root.resolve()
    for path in paths:
        resolved = path.resolve()
        try:
            resolved.relative_to(temporary_root)
        except ValueError:
            continue
        if resolved.is_symlink() or not resolved.is_file():
            continue
        retained.extend(
            (
                f"child_evidence: {resolved.relative_to(temporary_root).as_posix()}",
                resolved.read_text(encoding="utf-8", errors="replace"),
            )
        )
    return retained


def main() -> int:
    """Run contract-selected checks in order and emit one bounded result."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-file", type=pathlib.Path)
    args = parser.parse_args()
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    evidence_file = (
        args.evidence_file.expanduser().resolve()
        if args.evidence_file
        else repo_root / ".build" / "deploy-validation" / "repository-validation.log"
    )
    with tempfile.TemporaryDirectory(prefix="repository-validation-") as temporary:
        temporary_root = pathlib.Path(temporary)
        child_environment = os.environ.copy()
        child_environment["PYTHONPYCACHEPREFIX"] = str(temporary_root / "python-cache")
        for definition in CHECK_DEFINITIONS:
            argv = command(definition, temporary_root)
            prepare_temporary_directories(argv, temporary_root)
            raw_cwd = definition["cwd"]
            if not isinstance(raw_cwd, str):
                raise TypeError("check cwd must be a string")
            cwd = repo_root.joinpath(*pathlib.PurePosixPath(raw_cwd).parts)
            try:
                result = subprocess.run(
                    argv,
                    cwd=cwd,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    check=False,
                    env=child_environment,
                )
            except OSError as exc:
                result = subprocess.CompletedProcess(
                    argv,
                    COMMAND_NOT_FOUND_EXIT_CODE,
                    "",
                    f"{type(exc).__name__}: {exc}",
                )
            if result.returncode == 0:
                continue
            evidence_file.parent.mkdir(parents=True, exist_ok=True)
            partial = evidence_file.with_name(f".{evidence_file.name}.tmp")
            retained_child_evidence = child_evidence(argv, temporary_root)
            partial.write_text(
                "\n".join(
                    (
                        f"check: {definition['id']}",
                        f"exit_code: {result.returncode}",
                        f"cwd: {cwd}",
                        "command: " + json.dumps(argv, separators=(",", ":")),
                        "stdout:",
                        result.stdout or "",
                        "stderr:",
                        result.stderr or "",
                        *retained_child_evidence,
                    )
                )
                + "\n",
                encoding="utf-8",
                newline="\n",
            )
            partial.replace(evidence_file)
            print(
                json.dumps(
                    {
                        "check": definition["id"],
                        "exit_code": result.returncode,
                        "evidence_file": str(evidence_file),
                    },
                    separators=(",", ":"),
                )
            )
            return result.returncode if result.returncode > 0 else 1
    try:
        cleanup_evidence(
            evidence_file,
            prune_default_parent=args.evidence_file is None,
        )
    except OSError as exc:
        print(
            json.dumps(
                {
                    "check": "evidence-cleanup",
                    "exit_code": 1,
                    "evidence_file": str(evidence_file),
                    "cleanup_error": f"{type(exc).__name__}: {exc}",
                },
                separators=(",", ":"),
            )
        )
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
