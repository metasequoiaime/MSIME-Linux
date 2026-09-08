"""Freeze native resources using the pinned public dictionary validators."""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile

import product_lock


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def validate(spec):
    if set(spec) != {"version", "dictionary", "helpcodes"} or spec["version"] != 1:
        raise ValueError("Invalid native resource lock")
    product_lock.validate(spec["dictionary"])
    helpcodes = spec["helpcodes"]
    if not isinstance(helpcodes, dict) or not helpcodes:
        raise ValueError("Missing helpcode lock")
    for name, digest in helpcodes.items():
        path = PurePosixPath(name)
        if (len(path.parts) < 2 or path.parts[0] != "helpcodes" or str(path) != name or
                ".." in path.parts or "\\" in name or not name.endswith(".txt") or
                not isinstance(digest, str) or not product_lock.DIGEST.fullmatch(digest)):
            raise ValueError("Invalid helpcode lock entry")
    return spec


def build_lock(dictionary_lock, helpcode_root, inventory):
    helpcode_root = Path(helpcode_root)
    contract = json.loads(Path(inventory).read_text())
    required = {entry["path"] for entry in contract["assets"] if "schema" in entry}
    files = {}
    for file in sorted(helpcode_root.rglob("*.txt")):
        if file.is_symlink() or not file.is_file():
            raise ValueError("Invalid helpcode source")
        name = "helpcodes/" + file.relative_to(helpcode_root).as_posix()
        files[name] = product_lock.sha256(file)
    if not required or not required <= files.keys():
        raise ValueError("Incomplete Engine helpcode inventory")
    return validate({"version": 1, "dictionary": product_lock.load(Path(dictionary_lock)), "helpcodes": files})


def regular_file(path, private=False):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        info = os.fstat(descriptor)
        if (not stat.S_ISREG(info.st_mode) or info.st_size > 512 * 1024 * 1024 or
                (private and (info.st_uid != os.geteuid() or info.st_nlink != 1 or info.st_mode & 0o022))):
            raise ValueError("Invalid resource file")
        return os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise


def safe_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(descriptor)
        if info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise ValueError("Unsafe native resource directory")
    finally:
        os.close(descriptor)


def verify(directory, spec, cancelled):
    # Validate layout before the shared validators open the frozen files.
    names = list(spec["dictionary"]["dictionary"]["assets"]) + list(spec["helpcodes"])
    expected_directories = {str(parent) for name in names for parent in PurePosixPath(name).parents} - {"."}
    actual_files = set()
    for entry in directory.rglob("*"):
        relative = entry.relative_to(directory).as_posix()
        if entry.is_symlink():
            raise ValueError("Symlink in frozen resources")
        if entry.is_dir():
            if relative not in expected_directories:
                raise ValueError("Unexpected frozen resource directory")
        else:
            actual_files.add(relative)
    if actual_files != set(names):
        raise ValueError("Frozen resource inventory differs from lock")
    for name in names:
        if cancelled():
            raise InterruptedError("Native resource preparation cancelled")
        for parent in PurePosixPath(name).parents:
            safe_directory(directory / parent)
        with regular_file(directory / name, private=True):
            pass
    product_lock.verify_assets(directory, spec["dictionary"])
    for name, digest in spec["helpcodes"].items():
        if cancelled():
            raise InterruptedError("Native resource preparation cancelled")
        if product_lock.sha256(directory / name) != digest:
            raise ValueError("Helpcode does not match the resource lock")


def rename_new(source, target):
    # Linux renameat2 provides directory publication without replacing even an
    # empty existing directory. There is deliberately no weaker fallback.
    function = ctypes.CDLL(None, use_errno=True).renameat2
    function.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    function.restype = ctypes.c_int
    if function(-100, os.fsencode(source), -100, os.fsencode(target), 1) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(target))


def prepare(source, resources_root, spec, cancelled=lambda: False):
    spec = validate(spec)
    source, resources_root = Path(source), Path(resources_root)
    if not source.is_absolute() or not resources_root.is_absolute():
        raise ValueError("Absolute resource directories required")
    resources_root.mkdir(parents=True, exist_ok=True)
    safe_directory(resources_root)
    content_id = hashlib.sha256(canonical(spec)).hexdigest()
    target = resources_root / content_id
    if target.exists() or target.is_symlink():
        safe_directory(target)
        verify(target, spec, cancelled)
        return target
    incoming = Path(tempfile.mkdtemp(prefix=".incoming-", dir=resources_root))
    try:
        total = 0
        for name in list(spec["dictionary"]["dictionary"]["assets"]) + list(spec["helpcodes"]):
            if cancelled():
                raise InterruptedError("Native resource preparation cancelled")
            for parent in PurePosixPath(name).parents:
                # Source can be a root-owned packaged directory. Reject symlinks
                # without requiring ownership by the interactive user.
                fd = os.open(source / parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
                os.close(fd)
            destination = incoming / name
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with regular_file(source / name) as reader:
                fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
                with os.fdopen(fd, "wb") as writer:
                    while True:
                        if cancelled():
                            raise InterruptedError("Native resource preparation cancelled")
                        chunk = reader.read(1024 * 1024)
                        if not chunk:
                            break
                        total += len(chunk)
                        if total > 1024 * 1024 * 1024:
                            raise ValueError("Native resource bundle too large")
                        writer.write(chunk)
                    writer.flush()
                    os.fsync(writer.fileno())
        verify(incoming, spec, cancelled)
        if cancelled():
            raise InterruptedError("Native resource preparation cancelled")
        # Persist the nested directory entries before publishing their root.
        directories = [path for path in incoming.rglob("*") if path.is_dir()] + [incoming]
        for path in sorted(directories, key=lambda value: len(value.parts), reverse=True):
            fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        if cancelled():
            raise InterruptedError("Native resource preparation cancelled")
        try:
            rename_new(incoming, target)
        except OSError as error:
            if error.errno != errno.EEXIST:
                raise
            safe_directory(target)
            verify(target, spec, cancelled)
        fd = os.open(resources_root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        return target
    finally:
        shutil.rmtree(incoming, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build-lock")
    build.add_argument("dictionary_lock", type=Path)
    build.add_argument("helpcodes", type=Path)
    build.add_argument("inventory", type=Path)
    build.add_argument("output", type=Path)
    install = commands.add_parser("prepare")
    install.add_argument("source", type=Path)
    install.add_argument("resources_root", type=Path)
    install.add_argument("lock", type=Path)
    args = parser.parse_args()
    if args.command == "build-lock":
        args.output.write_bytes(canonical(build_lock(args.dictionary_lock, args.helpcodes, args.inventory)) + b"\n")
    else:
        print(prepare(args.source, args.resources_root, json.loads(args.lock.read_text())))


if __name__ == "__main__":
    main()
