"""Run the packaged verifier from a staged install against locked resources."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

build, libexec, dictionary, helpcodes = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="msime-installed-resources-") as temporary:
    root = Path(temporary)
    environment = dict(os.environ, DESTDIR=str(root / "stage"))
    subprocess.run(["cmake", "--install", str(build), "--prefix", "/usr", "--component", "NativeResources"],
                   env=environment, check=True, capture_output=True, text=True)
    relative = libexec.relative_to("/") if libexec.is_absolute() else Path("usr") / libexec
    tools = root / "stage" / relative / "metasequoia-native-resources"
    lock = tools / "native-resource-lock.json"
    spec = json.loads(lock.read_text())
    checksums = list((root / "stage").rglob("SHA256SUMS.txt"))
    assert len(checksums) == 1 and checksums[0].read_bytes() == (dictionary.parent / "SHA256SUMS.txt").read_bytes()
    source = root / "source"
    source.mkdir()
    for name in spec["dictionary"]["dictionary"]["assets"]:
        shutil.copyfile(dictionary.parent / name, source / name)
    shutil.copytree(helpcodes, source / "helpcodes")
    command = [sys.executable, str(tools / "native_resources.py"), "prepare", str(source), str(root / "resources"), str(lock)]
    first = subprocess.run(command, cwd=root, check=True, capture_output=True, text=True).stdout.strip()
    second = subprocess.run(command, cwd=root, check=True, capture_output=True, text=True).stdout.strip()
    assert first == second and len(Path(first).name) == 64
    assert Path(first).is_dir()
    assert set(path.name for path in tools.glob("*.py")) == {
        "native_resources.py", "product_lock.py", "product_lock_shared.py", "dictionary_product.py"}
print("Installed verifier and generated lock prepared and reused the real resource bundle")
