"""Exercise GTK restore with locked resources and an isolated session bus."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

if sys.argv[1] == "--session":
    binary, tools, source, native, engine = map(Path, sys.argv[2:])
    daemon = subprocess.Popen(["ibus-daemon", "--single", "--panel", "disable", "--config", "disable"])
    process = None
    try:
        for _ in range(100):
            if daemon.poll() is not None:
                raise RuntimeError("IBus daemon exited before startup")
            probe = subprocess.run(["ibus", "list-engine"], capture_output=True, text=True)
            if probe.returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("IBus address unavailable")
        process = subprocess.Popen([str(engine)])
        subprocess.run([str(binary), str(tools), str(source), str(native), "live"], check=True)
        if process.poll() is not None:
            raise RuntimeError("Native input service exited during restore")
    finally:
        for child in (process, daemon):
            if child is not None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
    sys.exit(0)

binary, dictionary, helpcodes, tools = map(Path, sys.argv[1:5])
engine = Path(sys.argv[5]) if len(sys.argv) == 6 else None
with tempfile.TemporaryDirectory(prefix="msime-native-window-") as temporary:
    root = Path(temporary)
    source = root / "source"
    source.mkdir()
    for name in ("msime.db", "english.db", "others.db", "SHA256SUMS.txt", "dictionary-manifest.json",
                 "dict_japanese.dat", "mozc_dictionary_oss_README.txt"):
        shutil.copyfile(dictionary.parent / name, source / name)
    shutil.copytree(helpcodes, source / "helpcodes")
    native = root / "native"
    native.mkdir()
    command = [str(binary), str(tools), str(source), str(native)]
    environment = os.environ.copy()
    if engine:
        for name in ("msime.db", "english.db", "others.db"):
            shutil.copyfile(source / name, native / name)
        shutil.copytree(source / "helpcodes", native / "helpcodes")
        environment.update(METASEQUOIA_IME_DATA_DIR=str(native), IBUS_USE_PORTAL="0", GIO_USE_VFS="local")
        for key in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"):
            directory = root / key.lower()
            directory.mkdir(mode=0o700)
            environment[key] = str(directory)
        command = [sys.executable, str(Path(__file__).resolve()), "--session", *command, str(engine)]
    subprocess.run(["dbus-run-session", "--", "xvfb-run", "-a", *command], env=environment, check=True)
