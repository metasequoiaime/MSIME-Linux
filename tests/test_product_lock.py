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
        with self.assertRaises(ValueError):
            lock.validate(self.data)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture_assets(directory)
            for name in lock._product.DESKTOP_FILES:
                (directory / name).write_bytes(b'MSJPDT1\0fixture' if name == 'dict_japanese.dat' else b'fixture')
            product = {'manifest_version': 1, 'format_version': 1, 'profile': 'desktop',
                       'engine_compatibility': {'dictionary_format': 1, 'japanese_model_magic': 'MSJPDT1'},
                       'files': {name: {'sha256': lock.sha256(directory / name), 'size': (directory / name).stat().st_size}
                                 for name in lock._product.DESKTOP_FILES}}
            manifest_path = directory / lock.PRODUCT_MANIFEST
            manifest_path.write_text(json.dumps(product))
            for name in (*lock.ASSETS, lock.PRODUCT_MANIFEST):
                self.data['dictionary']['assets'][name] = lock.sha256(directory / name)
            lock.validate(self.data)
            lock.verify_assets(directory, self.data)
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

    def test_mutable_dictionary_tags_are_rejected(self):
        for tag in ("latest", "main", "../dict-test", "dict-test\n", ""):
            with self.subTest(tag=tag):
                changed = copy.deepcopy(self.data)
                changed["dictionary"]["tag"] = tag
                with self.assertRaises(ValueError):
                    lock.validate(changed)

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
