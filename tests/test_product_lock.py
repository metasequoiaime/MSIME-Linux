"""Rejection cases for the product lock. Run with `python3 -m unittest discover -s tests -p 'test_product_lock.py'`."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("product_lock", ROOT / "scripts/product_lock.py")
lock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lock)


class ProductLockTests(unittest.TestCase):
    def setUp(self):
        self.data = lock.load(ROOT / "product-lock.json")

    def test_modern_dictionary_requires_a_compatible_locked_manifest(self):
        self.data['dictionary']['tag'] = 'dict-2026.09.06'
        self.data['dictionary']['repository'] = lock.DICTIONARY_REPOSITORY
        self.data['dictionary']['assets'].pop(lock.PRODUCT_MANIFEST, None)
        with self.assertRaises(ValueError):
            lock.validate(self.data)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture_assets(directory)
            for name in lock._product.DESKTOP_FILES:
                (directory / name).write_bytes(b'MSJPDT1\0fixture' if name == 'dict_japanese.dat' else b'fixture')
            product = {'manifest_version': 1, 'format_version': 1, 'profile': 'desktop',
                       'source': {'repository': self.data['dictionary']['repository'], 'commit': self.data['dictionary']['source_commit'], 'dirty': False},
                       'engine_compatibility': {'dictionary_format': 1, 'japanese_model_magic': 'MSJPDT1'},
                       'files': {name: {'sha256': lock.sha256(directory / name), 'size': (directory / name).stat().st_size}
                                 for name in lock._product.DESKTOP_FILES}}
            manifest_path = directory / lock.PRODUCT_MANIFEST
            manifest_path.write_text(json.dumps(product))
            for name in (*lock.ASSETS, lock.PRODUCT_MANIFEST):
                self.data['dictionary']['assets'][name] = lock.sha256(directory / name)
            lock.validate(self.data)
            lock.verify_assets(directory, self.data)
            original_commit = product['source']['commit']
            product['source']['commit'] = 'f' * 40
            manifest_path.write_text(json.dumps(product))
            self.data['dictionary']['assets'][lock.PRODUCT_MANIFEST] = lock.sha256(manifest_path)
            with self.assertRaises(ValueError):
                lock.verify_assets(directory, self.data)
            product['source']['commit'] = original_commit
            # Even a deliberately updated digest cannot declare an unsupported format compatible.
            product['format_version'] = 2
            manifest_path.write_text(json.dumps(product))
            self.data['dictionary']['assets'][lock.PRODUCT_MANIFEST] = lock.sha256(manifest_path)
            with self.assertRaises(ValueError):
                lock.verify_assets(directory, self.data)

    def fixture_assets(self, directory):
        for name in lock.ASSETS:
            value = (name + " fixture").encode()
            (directory / name).write_bytes(value)
            self.data["dictionary"]["assets"][name] = hashlib.sha256(value).hexdigest()

        if lock.PRODUCT_MANIFEST in self.data['dictionary']['assets']:
            for name in lock._product.DESKTOP_FILES:
                path = directory / name
                if not path.exists():
                    path.write_bytes(b'fixture')
                if name == 'dict_japanese.dat':
                    path.write_bytes(b'MSJPDT1\0fixture')
            product = {'manifest_version': 1, 'format_version': 1, 'profile': 'desktop',
                       'source': {'repository': self.data['dictionary']['repository'],
                                  'commit': self.data['dictionary']['source_commit'], 'dirty': False},
                       'engine_compatibility': {'dictionary_format': 1, 'japanese_model_magic': 'MSJPDT1'},
                       'files': {name: {'sha256': lock.sha256(directory / name), 'size': (directory / name).stat().st_size}
                                 for name in lock._product.DESKTOP_FILES}}
            (directory / lock.PRODUCT_MANIFEST).write_text(json.dumps(product))
            for name in self.data['dictionary']['assets']:
                self.data['dictionary']['assets'][name] = lock.sha256(directory / name)

    def test_mutable_dictionary_tags_are_rejected(self):
        for tag in ("latest", "main", "../dict-test", "dict-test\n", ""):
            with self.subTest(tag=tag):
                changed = copy.deepcopy(self.data)
                changed["dictionary"]["tag"] = tag
                with self.assertRaises(ValueError):
                    lock.validate(changed)

    def test_a_mutable_dictionary_source_commit_is_rejected(self):
        # The commit the tag resolved to is the whole provenance claim. It used to be read from the
        # dict gitlink, which recorded 55bd649 while the shipped bytes came from 0c7368c.
        for commit in ("", "main", "0c7368c", "a" * 39, "a" * 40 + "\n", "A" * 40):
            with self.subTest(commit=commit):
                changed = copy.deepcopy(self.data)
                changed["dictionary"]["source_commit"] = commit
                with self.assertRaises(ValueError):
                    lock.validate(changed)

    def test_the_dictionary_source_is_not_a_gitlink(self):
        # Re-adding it as a submodule would restore a second, unsynchronised home for the pin.
        self.assertNotIn("dict", lock.SUBMODULES)
        gitmodules = (ROOT / ".gitmodules").read_text()
        self.assertNotIn("vendor/MetasequoiaImeDict", gitmodules)

    def tagged_repository(self, directory, tag, annotated):
        """A real repository, because the bug this guards lives in the ls-remote invocation.

        Handing the parser a transcript proves only that the parser works. The first version of
        this asked ls-remote for refs/tags/<tag> alone, which never emits the peeled ref, so the
        commit could not be found no matter how correct the parsing was.
        """
        run = lambda *args: subprocess.run(["git", "-C", str(directory), *args], check=True,
                                           capture_output=True, text=True)
        run("init", "--quiet", "--initial-branch", "main")
        run("-c", "user.email=t@example.com", "-c", "user.name=t", "commit", "--quiet",
            "--allow-empty", "-m", "release")
        if annotated:
            run("-c", "user.email=t@example.com", "-c", "user.name=t", "tag", "-a", tag, "-m", tag)
        else:
            run("tag", tag)
        commit = run("rev-parse", f"{tag}^{{commit}}").stdout.strip()
        return f"file://{directory}", commit

    def test_a_tag_resolves_to_its_commit_whether_or_not_it_is_annotated(self):
        for annotated in (True, False):
            with self.subTest(annotated=annotated), tempfile.TemporaryDirectory() as temporary:
                tag = "dict-2026.01.01"
                url, commit = self.tagged_repository(Path(temporary), tag, annotated)
                with mock.patch.object(lock, "DICTIONARY_URL", url):
                    resolved = lock.resolve_tag_commit(tag)
                self.assertEqual(resolved, commit)
                # An annotated tag's own object is 40 hex digits too, so a wrong answer here would
                # pass every downstream check and lock something that is not in the history.
                if annotated:
                    tag_object = subprocess.check_output(
                        ["git", "-C", temporary, "rev-parse", tag], text=True).strip()
                    self.assertNotEqual(resolved, tag_object)

    def test_tag_resolution_refuses_anything_but_an_explicit_release(self):
        for tag in ("latest", "main", "../dict-test", ""):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                lock.resolve_tag_commit(tag)

    def test_a_tag_that_resolves_to_nothing_is_not_locked(self):
        with mock.patch.object(lock.subprocess, "check_output", return_value=""):
            with self.assertRaises(ValueError):
                lock.resolve_tag_commit(self.data["dictionary"]["tag"])

    def test_every_shipped_asset_must_be_locked(self):
        for name in lock.ASSETS:
            with self.subTest(name=name):
                changed = copy.deepcopy(self.data)
                del changed["dictionary"]["assets"][name]
                with self.assertRaises(ValueError):
                    lock.validate(changed)

    def test_assets_cannot_escape_the_output_directory(self):
        self.data["dictionary"]["assets"]["../notice.txt"] = "a" * 64
        with self.assertRaises(ValueError):
            lock.validate(self.data)

    def test_truncated_digests_are_rejected(self):
        for digest in ("", "abc123", "a" * 63, "a" * 64 + "\n", "A" * 64):
            with self.subTest(digest=digest):
                changed = copy.deepcopy(self.data)
                changed["dictionary"]["assets"]["msime.db"] = digest
                with self.assertRaises(ValueError):
                    lock.validate(changed)

    def test_mutating_both_the_database_and_the_upstream_checksums_still_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture_assets(directory)
            lock.verify_assets(directory, self.data)
            (directory / "msime.db").write_bytes(b"replacement database")
            (directory / "SHA256SUMS.txt").write_text(lock.sha256(directory / "msime.db") + "  msime.db\n")
            with self.assertRaises(ValueError):
                lock.verify_assets(directory, self.data)

    def test_a_missing_asset_is_not_silently_skipped(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture_assets(directory)
            (directory / "english.db").unlink()
            with self.assertRaises(ValueError):
                lock.verify_assets(directory, self.data)

    def test_downloads_only_come_from_an_explicit_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            for tag in ("latest", "../../etc", "main"):
                with self.subTest(tag=tag), self.assertRaises(ValueError):
                    lock.download_assets(tag, Path(temporary))

    def test_a_failed_download_never_overwrites_previous_usable_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            output.mkdir()
            self.fixture_assets(output)
            before = {path.name: path.read_bytes() for path in output.iterdir()}

            def corrupt_download(tag, destination):
                for name, value in before.items():
                    (destination / name).write_bytes(value)
                (destination / "others.db").write_bytes(b"truncated")

            with mock.patch.object(lock, "download_assets", side_effect=corrupt_download):
                with tempfile.TemporaryDirectory(dir=output) as staging:
                    incoming = Path(staging)
                    lock.download_assets(self.data["dictionary"]["tag"], incoming)
                    with self.assertRaises(ValueError):
                        lock.verify_assets(incoming, self.data)
            self.assertEqual(before, {path.name: path.read_bytes() for path in output.iterdir()})

    def test_the_manifest_records_the_gitlinks_this_checkout_actually_carries(self):
        commit = "1" * 40
        record = lock.manifest(ROOT, commit, ROOT / "product-lock.json", self.data)
        self.assertEqual(record["source"]["commit"], commit)
        self.assertEqual(record["dictionary"], self.data["dictionary"])
        self.assertEqual(record["lock_sha256"], lock.sha256(ROOT / "product-lock.json"))
        self.assertEqual(set(record["submodules"]), set(lock.SUBMODULES))
        for name, (repository, path) in lock.SUBMODULES.items():
            entry = record["submodules"][name]
            self.assertEqual(entry["repository"], repository)
            self.assertEqual(entry["path"], path)
            expected = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", f"HEAD:{path}"], text=True)
            self.assertEqual(entry["commit"], expected.strip())

    def test_a_mutable_source_commit_is_rejected(self):
        for commit in ("main", "HEAD", "abc123", "a" * 40 + "\n"):
            with self.subTest(commit=commit), self.assertRaises(ValueError):
                lock.manifest(ROOT, commit, ROOT / "product-lock.json", self.data)

    def test_a_path_that_stopped_being_a_gitlink_is_rejected(self):
        with mock.patch.object(lock, "git", return_value="100644 blob " + "0" * 40 + "\tvendor/x"):
            with self.assertRaises(ValueError):
                lock.gitlinks(ROOT)

    def test_the_manifest_command_writes_what_it_promises(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "product-manifest.json"
            commit = "1" * 40
            subprocess.run(["python3", str(ROOT / "scripts/product_lock.py"), "manifest",
                            "--source-commit", commit, "--output", str(output)], check=True)
            record = json.loads(output.read_text())
            self.assertEqual(record["source"]["repository"], lock.REPOSITORY)
            self.assertEqual(record["source"]["commit"], commit)
            self.assertEqual(record["dictionary"], self.data["dictionary"])
            self.assertEqual(record["lock_sha256"], lock.sha256(ROOT / "product-lock.json"))


if __name__ == "__main__":
    unittest.main()
