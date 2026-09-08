"""Native resource freezing exercises the real pinned public validators."""
import concurrent.futures
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import test_product_lock as fixtures

ROOT = fixtures.ROOT

spec = importlib.util.spec_from_file_location("native_resources", ROOT / "scripts/native_resources.py")
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)


class NativeResourcesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        fixture = fixtures.ProductLockTests()
        fixture.setUp()
        fixture.fixture_assets(self.source)
        dictionary_lock = self.root / "dictionary-lock.json"
        dictionary_lock.write_text(json.dumps(fixture.data))
        inventory = ROOT / "vendor/MetasequoiaImeEngine/contracts/assets/assets.json"
        for entry in json.loads(inventory.read_text())["assets"]:
            if "schema" in entry:
                file = self.source / entry["path"]
                file.parent.mkdir(exist_ok=True)
                file.write_text("synthetic helpcode " + entry["schema"])
        self.lock = resources.build_lock(dictionary_lock, self.source / "helpcodes", inventory)
        self.destination = self.root / "resources"

    def prepare(self):
        return resources.prepare(self.source, self.destination, self.lock)

    def test_freezes_reuses_and_never_repairs_corrupted_published_resources(self):
        target = self.prepare()
        self.assertEqual(len(target.name), 64)
        inode = target.stat().st_ino
        (self.source / "msime.db").write_text("source changed after preparation")
        self.assertEqual(self.prepare(), target)
        self.assertEqual(target.stat().st_ino, inode)
        (target / "msime.db").write_text("corrupt cache")
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual((target / "msime.db").read_text(), "corrupt cache")

    def test_digest_and_helpcode_failures_clean_private_stage(self):
        for name in ("msime.db", next(iter(self.lock["helpcodes"]))):
            file = self.source / name
            original = file.read_bytes()
            file.write_text("wrong contents")
            with self.assertRaises(ValueError):
                self.prepare()
            self.assertEqual(list(self.destination.iterdir()), [])
            file.write_bytes(original)

    def test_cancelled_copy_does_not_publish(self):
        calls = 0
        def cancelled():
            nonlocal calls
            calls += 1
            return calls >= 7
        with self.assertRaises(InterruptedError):
            resources.prepare(self.source, self.destination, self.lock, cancelled)
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_symlink_and_path_escape_are_rejected(self):
        file = self.source / "msime.db"
        file.rename(self.source / "original.db")
        file.symlink_to(self.source / "original.db")
        with self.assertRaises(OSError):
            self.prepare()
        self.assertEqual(list(self.destination.iterdir()), [])
        unsafe = copy.deepcopy(self.lock)
        unsafe["helpcodes"] = {"helpcodes/../../escape.txt": "a" * 64}
        with self.assertRaises(ValueError):
            resources.prepare(self.source, self.destination, unsafe)

    def test_existing_empty_target_and_extra_files_are_not_replaced(self):
        self.destination.mkdir()
        target = self.destination / resources.hashlib.sha256(resources.canonical(self.lock)).hexdigest()
        target.mkdir()
        inode = target.stat().st_ino
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(target.stat().st_ino, inode)
        target.rmdir()
        target = self.prepare()
        (target / "custom_translations.txt").write_text("unlocked resource")
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertTrue((target / "custom_translations.txt").exists())

    def test_concurrent_publication_never_overwrites_winner(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            paths = list(pool.map(lambda _: self.prepare(), range(3)))
        self.assertEqual(len(set(paths)), 1)
        self.assertEqual(list(self.destination.iterdir()), [paths[0]])
        resources.verify(paths[0], self.lock, lambda: False)


if __name__ == "__main__":
    unittest.main()
