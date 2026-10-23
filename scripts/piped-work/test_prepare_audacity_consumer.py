"""Portable consumer ownership, archive safety and fresh-call interruption cases.

CTest supplies the temporary root. Network fixtures are synthetic ZIPs, with no
installed app or user media. The native AUP3 case separately exercises real stock
packages through the same cache owner and exact package pins used in CI.
"""
from __future__ import annotations

import hashlib
import io
import os
import stat
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest.mock import patch

import prepare_audacity_consumer as consumer
import run_audacity_consumer as sessions


def package_zip(entries: dict[str, bytes] | None = None) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in (entries or {"Audacity/audacity.exe": b"neutral executable",
                                     "Audacity/modules/mod-script-pipe.dll": b"neutral module",
                                     "Audacity/LICENSE.txt": b"neutral license"}).items():
            archive.writestr(name, data)
    return output.getvalue()


class ConsumerCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        parent = os.environ.get("AUTOPSY_TEST_TEMP_ROOT")
        if not parent:
            self.fail("The owning CTest entry must supply AUTOPSY_TEST_TEMP_ROOT.")
        self.temporary = tempfile.TemporaryDirectory(prefix="_consumer-tests-", dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "_cache"
        self.archive = package_zip()
        self.package = consumer.AudacityPackage("3.7.8", hashlib.sha256(self.archive).hexdigest())
        self.packages = (self.package,)

    def response(self, *_args, **_kwargs):
        return io.BytesIO(self.archive)

    def prepare(self):
        with consumer.AudacityConsumerCache(self.cache, self.packages) as cache:
            return cache.prepare_consumer(self.package)

    def pending(self) -> list[Path]:
        return list(self.cache.glob("_pending-*"))

    def test_official_pins_are_distinct_bounded_and_have_no_moving_url(self) -> None:
        self.assertEqual([item.version for item in consumer.PINNED_CONSUMERS], ["3.7.8", "3.7.9"])
        for package in consumer.PINNED_CONSUMERS:
            self.assertRegex(package.sha256, r"^[0-9a-f]{64}$")
            self.assertEqual(package.url, f"https://github.com/audacity/audacity/releases/download/"
                             f"Audacity-{package.version}/audacity-win-{package.version}-64bit.zip")
        with self.assertRaises(ValueError):
            consumer.AudacityConsumerCache(self.cache, self.packages * 3)
        with self.assertRaises(ValueError):
            consumer.AudacityConsumerCache(self.cache, (consumer.AudacityPackage("../escape", "a" * 64),))

    def test_repeat_reuses_accepted_package_and_preserves_original_receipt(self) -> None:
        with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response) as download:
            first = self.prepare()
            receipt = self.cache / self.package.cache_name / "_receipt.json"
            original, stamp = receipt.read_bytes(), receipt.stat().st_mtime_ns
            second = self.prepare()
        self.assertEqual(first, second)
        self.assertEqual(download.call_count, 1)
        self.assertEqual(receipt.read_bytes(), original)
        self.assertEqual(receipt.stat().st_mtime_ns, stamp)
        self.assertEqual(first["executable"].read_bytes(), b"neutral executable")
        self.assertFalse(list(self.cache.rglob("*.zip")))
        self.assertFalse(self.pending())

    def test_changed_or_missing_payload_is_replaced_from_current_package(self) -> None:
        for damage in ("changed", "missing", "extra", "receipt"):
            with self.subTest(damage=damage):
                with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response) as download:
                    first = self.prepare()
                    target = self.cache / self.package.cache_name
                    if damage == "changed":
                        first["executable"].write_bytes(b"changed bytes")
                    elif damage == "missing":
                        first["executable"].unlink()
                    elif damage == "extra":
                        (first["executable"].parent / "extra.txt").write_text("unexpected")
                    else:
                        (target / "_receipt.json").write_text("incomplete")
                    second = self.prepare()
                    self.assertEqual(second["executable"].read_bytes(), b"neutral executable")
                    self.assertEqual(download.call_count, 2)
                    self.assertFalse((second["executable"].parent / "extra.txt").exists())
                consumer.remove_cache_tree(target)

    def test_checksum_download_failure_and_size_limit_publish_nothing(self) -> None:
        for failure in ("digest", "network", "size"):
            with self.subTest(failure=failure):
                def download(*_args, selected=failure, **_kwargs):
                    if selected == "network":
                        raise urllib.error.URLError("test network unavailable")
                    return io.BytesIO(b"wrong" if selected == "digest" else self.archive)
                limit = len(self.archive) - 1 if failure == "size" else consumer.MAX_ARCHIVE_BYTES
                with patch.object(consumer.urllib.request, "urlopen", side_effect=download), \
                        patch.object(consumer, "MAX_ARCHIVE_BYTES", limit):
                    with self.assertRaisesRegex(RuntimeError, "download portable consumer.*3.7.8"):
                        self.prepare()
                self.assertFalse((self.cache / self.package.cache_name).exists())
                self.assertFalse(self.pending())
                with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response):
                    self.assertTrue(self.prepare()["executable"].is_file())
                consumer.remove_cache_tree(self.cache / self.package.cache_name)

    def test_interruptions_before_and_after_each_package_effect_converge(self) -> None:
        # A package is visible only after atomic publication. An interruption
        # after that effect must reuse the completed bytes on a fresh call.
        for operation in ("download_archive", "extract_archive", "publish", "prune"):
            for when in ("before", "after"):
                with self.subTest(operation=operation, when=when):
                    real = (os.replace if operation == "publish" else
                            consumer.AudacityConsumerCache.prune_owned_outputs if operation == "prune"
                            else getattr(consumer, operation))

                    def interrupted(*args, selected=operation, boundary=when, original=real, **kwargs):
                        package_effect = selected != "publish" or Path(args[1]).name == self.package.cache_name
                        # Entry pruning has no package yet. Interrupt after the
                        # completed package exists, at the success-time pruning.
                        if selected == "prune":
                            package_effect = (self.cache / self.package.cache_name).exists()
                        if package_effect and boundary == "before":
                            raise InterruptedError("before " + selected)
                        result = original(*args, **kwargs)
                        if package_effect and boundary == "after":
                            raise InterruptedError("after " + selected)
                        return result

                    owner = consumer.AudacityConsumerCache if operation == "prune" else consumer.os if operation == "publish" else consumer
                    attribute = "prune_owned_outputs" if operation == "prune" else "replace" if operation == "publish" else operation
                    with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response) as download:
                        with patch.object(owner, attribute, new=interrupted):
                            with self.assertRaisesRegex(RuntimeError, "failed for Audacity"):
                                self.prepare()
                        before = download.call_count
                        result = self.prepare()
                        already_published = operation == "prune" or (operation == "publish" and when == "after")
                        self.assertEqual(download.call_count, before + (0 if already_published else 1))
                        self.assertTrue(result["executable"].is_file())
                        self.assertEqual(result, self.prepare())
                    self.assertFalse(self.pending())
                    consumer.remove_cache_tree(self.cache / self.package.cache_name)

    def test_owner_publication_interruptions_leave_a_fresh_entry_path(self) -> None:
        for boundary in ("before", "after"):
            cache = self.root / ("_owner-" + boundary)
            real = os.replace
            def interrupted(source, target, selected=boundary, original=real):
                if Path(target).name == "_owner" and selected == "before":
                    raise InterruptedError("before owner publication")
                original(source, target)
                if Path(target).name == "_owner" and selected == "after":
                    raise InterruptedError("after owner publication")
            with patch.object(consumer.os, "replace", new=interrupted):
                with self.assertRaises(InterruptedError):
                    with consumer.AudacityConsumerCache(cache, self.packages):
                        self.fail("Interrupted owner entered")
            with consumer.AudacityConsumerCache(cache, self.packages):
                self.assertEqual((cache / "_owner").read_text(), consumer.OWNER)
                self.assertFalse((cache / "_owner.pending").exists())

    def test_fresh_entry_prunes_abandoned_staging_and_superseded_owned_packages(self) -> None:
        with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response):
            old = self.prepare()["executable"]
        orphan = self.cache / ("_pending-" + "a" * 32)
        orphan.mkdir()
        (orphan / "_archive.zip").write_bytes(b"interrupted download")
        unrelated = self.cache / "keep.txt"
        unrelated.write_text("not producer output")
        replacement = consumer.AudacityPackage("3.7.9", self.package.sha256)
        with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response):
            with consumer.AudacityConsumerCache(self.cache, (replacement,)) as cache:
                self.assertFalse(old.exists())
                self.assertFalse(orphan.exists())
                self.assertTrue(cache.prepare_consumer(replacement)["executable"].is_file())
        self.assertEqual(unrelated.read_text(), "not producer output")
        self.assertEqual(len(list(self.cache.glob("_audacity-*"))), 1)

    def test_prune_interruptions_do_not_repeat_finished_downloads(self) -> None:
        with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response):
            self.prepare()
        for when in ("before", "after"):
            orphan = self.cache / ("_pending-" + "b" * 32)
            orphan.mkdir()
            real = consumer.remove_cache_tree
            def interrupted(path, selected=when, original=real, abandoned=orphan):
                if path == abandoned and selected == "before":
                    raise InterruptedError("before prune")
                original(path)
                if path == abandoned and selected == "after":
                    raise InterruptedError("after prune")
            with patch.object(consumer, "remove_cache_tree", new=interrupted):
                with self.assertRaises(InterruptedError):
                    with consumer.AudacityConsumerCache(self.cache, self.packages):
                        self.fail("Interrupted pruning entered")
            with patch.object(consumer.urllib.request, "urlopen", side_effect=AssertionError("unneeded download")):
                self.assertTrue(self.prepare()["executable"].is_file())
            self.assertFalse(orphan.exists())

    def test_unmarked_cache_and_live_lease_block_without_changing_unrelated_files(self) -> None:
        self.cache.mkdir()
        unrelated = self.cache / "keep.txt"
        unrelated.write_text("untouched")
        with self.assertRaisesRegex(RuntimeError, "unmarked"):
            self.prepare()
        self.assertEqual(list(self.cache.iterdir()), [unrelated])
        unrelated.unlink()
        with consumer.AudacityConsumerCache(self.cache, self.packages) as first:
            with self.assertRaisesRegex(RuntimeError, "Another portable consumer session"):
                with consumer.AudacityConsumerCache(self.cache, self.packages):
                    self.fail("Competing owner acquired lease")
            with self.assertRaisesRegex(RuntimeError, "declared package"):
                first.prepare_consumer(consumer.AudacityPackage("3.7.9", "b" * 64))
        with consumer.AudacityConsumerCache(self.cache, self.packages):
            self.assertEqual((self.cache / "_cache.lock").stat().st_size, 1)

    def test_killed_worker_releases_lease_and_fresh_entry_removes_orphans(self) -> None:
        program = ("import sys,time; from pathlib import Path; from prepare_audacity_consumer import AudacityConsumerCache; "
                   "cache=AudacityConsumerCache(Path(sys.argv[1])); cache.__enter__(); "
                   "(cache.root/('_pending-'+'c'*32)).mkdir(); print('locked',flush=True); time.sleep(30)")
        child = subprocess.Popen([sys.executable, "-c", program, str(self.cache)],
                                 cwd=Path(consumer.__file__).parent, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        try:
            assert child.stdout is not None
            self.assertEqual(child.stdout.readline().strip(), "locked")
            child.terminate()
            child.communicate(timeout=10)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=10)
        with consumer.AudacityConsumerCache(self.cache, self.packages):
            self.assertFalse(self.pending())

    def test_archive_paths_links_collisions_and_missing_module_are_rejected(self) -> None:
        for name in ("../escape", "/escape", "C:/escape", "a\\escape", "a/../escape", "./escape",
                     "a//escape", "a/CON.txt", "a/data:stream", "a/b. "):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    consumer.archive_path(name)
        cases = [package_zip({"../escape": b"x"}), package_zip({"a": b"x", "a/b": b"y"}),
                 package_zip({"a.exe": b"x", "A.exe": b"y"}), package_zip({"audacity.exe": b"x"})]
        linked = io.BytesIO()
        with zipfile.ZipFile(linked, "w") as archive:
            item = zipfile.ZipInfo("link")
            item.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(item, "../escape")
        cases.append(linked.getvalue())
        for index, data in enumerate(cases):
            with self.subTest(archive=index):
                archive_file = self.root / f"_unsafe-{index}.zip"
                archive_file.write_bytes(data)
                with self.assertRaises(ValueError):
                    consumer.extract_archive(archive_file, self.root / f"_payload-{index}")
        self.assertFalse((self.root.parent / "escape").exists())

    def test_payload_and_member_count_limits_block_before_extraction(self) -> None:
        archive = self.root / "_limit.zip"
        archive.write_bytes(self.archive)
        for limit in ("MAX_PAYLOAD_BYTES", "MAX_MEMBERS"):
            with patch.object(consumer, limit, 1):
                output = self.root / ("_" + limit)
                with self.assertRaisesRegex(ValueError, "payload limits"):
                    consumer.extract_archive(archive, output)
                self.assertFalse(output.exists())

    def test_links_in_cache_are_not_followed_or_deleted(self) -> None:
        # Junction/reparse detection is exercised even on hosts without symlink
        # creation privileges. The actual external directory stays unchanged.
        with patch.object(consumer.urllib.request, "urlopen", side_effect=self.response):
            result = self.prepare()
        external = self.root / "_outside"
        external.mkdir()
        preserved = external / "keep.txt"
        preserved.write_text("untouched")
        target = self.cache / self.package.cache_name
        original = consumer.is_link
        with patch.object(consumer, "is_link", side_effect=lambda path: path == target or original(path)):
            with self.assertRaisesRegex(RuntimeError, "linked consumer package"):
                self.prepare()
        self.assertEqual(preserved.read_text(), "untouched")
        self.assertTrue(result["executable"].is_file())

    def test_native_matrix_uses_both_pins_and_keeps_fixture_descriptor_on_success_or_failure(self) -> None:
        case = self.root / "_case"
        case.mkdir()
        expected = case / "_compatibility-expected.json"
        for fail_at in (None, 0, 1):
            expected.write_text("[]")
            calls: list[Path] = []
            def check(root, executable, display, ffmpeg, observed=calls, failure=fail_at):
                self.assertTrue(expected.exists())
                observed.append(executable)
                if len(observed) - 1 == failure:
                    raise RuntimeError("consumer rejected project")
                return {"projects": ["neutral.aup3"]}
            fake_result = {"executable": self.root / "audacity.exe", "version": "3.7.8",
                           "archive_sha256": "a" * 64, "url": "official"}
            with patch.object(sessions, "AudacityConsumerCache") as cache, \
                    patch.object(sessions, "check_stock_audacity_projects", side_effect=check):
                cache.return_value.__enter__.return_value.prepare_consumer.return_value = fake_result
                if fail_at is None:
                    result = sessions.check_portable_audacity_projects(case, {}, "ffmpeg")
                    self.assertEqual(len(result), 2)
                    self.assertEqual(len(calls), 2)
                    self.assertEqual(result[0]["package"]["archive_sha256"], "a" * 64)
                else:
                    with self.assertRaisesRegex(RuntimeError, "consumer rejected"):
                        sessions.check_portable_audacity_projects(case, {}, "ffmpeg")
                prepared = cache.return_value.__enter__.return_value.prepare_consumer.call_args_list
                self.assertEqual([item.args[0] for item in prepared], list(consumer.PINNED_CONSUMERS))
            self.assertEqual(expected.read_text(), "[]")


if __name__ == "__main__":
    unittest.main()
