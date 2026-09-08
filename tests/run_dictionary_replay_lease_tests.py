"""Exercise the actual replay executable while another process publishes."""
import fcntl
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="msime-replay-lease-") as name:
    root = pathlib.Path(name)
    lock = root / "dictionary-sessions.lock"
    lock.touch(mode=0o600)
    command = [sys.argv[1], "--data-dir", name]
    with lock.open("r+") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        assert result.returncode == 1, result
        assert "publication is in progress" in result.stderr, result.stderr
        assert set(root.iterdir()) == {lock, root / "dictionary-publication.lock"}, "busy replay wrote dictionary files"
        fcntl.flock(handle, fcntl.LOCK_UN)
    result = subprocess.run(command, capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert "Applied 0 user dictionary operations" in result.stdout, result.stdout
print("Replay refuses concurrent publication and succeeds after release")
