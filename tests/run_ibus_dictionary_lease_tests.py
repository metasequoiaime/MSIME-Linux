"""IBus must reject publication contention before seeding or opening databases."""
import fcntl
import os
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="msime-ibus-lease-") as directory:
    root = pathlib.Path(directory)
    lock = root / "dictionary-sessions.lock"
    lock.touch(mode=0o600)
    environment = dict(os.environ, METASEQUOIA_IME_DATA_DIR=directory)
    with lock.open("r+") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        result = subprocess.run([sys.argv[1]], env=environment, capture_output=True, text=True, timeout=5)
        assert result.returncode == 1, result
        assert "publication is in progress" in result.stderr, result.stderr
        assert list(root.iterdir()) == [lock], "IBus seeded files while publication was active"
    lock.chmod(0o666)
    result = subprocess.run([sys.argv[1]], env=environment, capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result
    assert "Unable to acquire dictionary session lease" in result.stderr, result.stderr
    assert list(root.iterdir()) == [lock], "IBus opened dictionaries without a safe lease"
print("IBus refuses busy/unsafe dictionary leases before writing")
