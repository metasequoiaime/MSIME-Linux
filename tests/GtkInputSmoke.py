#!/usr/bin/env python3
"""Type through X11 -> GTK IM module -> IBus -> Engine into real GTK entries."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


def session(root):
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('IBus', '1.0')
    from gi.repository import GLib, Gtk, IBus

    def wait_for(predicate, description, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while GLib.MainContext.default().pending():
                GLib.MainContext.default().iteration(False)
            if predicate():
                return
            time.sleep(0.02)
        raise AssertionError(description)

    def key(*keys):
        subprocess.run(['xdotool', 'key', '--clearmodifiers', '--delay', '80', *keys], check=True)

    def settled():
        deadline = time.monotonic() + 0.4
        while time.monotonic() < deadline:
            while GLib.MainContext.default().pending():
                GLib.MainContext.default().iteration(False)
            time.sleep(0.01)

    subprocess.run(['ibus', 'write-cache'], check=True)
    log = (root / 'ibus.log').open('w')
    daemon = subprocess.Popen(['ibus-daemon', '--replace', '--single', '--panel', 'disable',
                               '--config', 'disable', '--cache', 'auto', '--xim'], stdout=log, stderr=log)
    try:
        IBus.init()
        bus = IBus.Bus()
        wait_for(bus.is_connected, 'IBus did not connect')
        wait_for(lambda: any(e.get_name() == 'metasequoiaime' for e in bus.list_engines()),
                 'Built engine was not registered')
        window = Gtk.Window(title='MSIME GTK CI')
        window.set_default_size(600, 220)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        first, second = Gtk.Entry(), Gtk.Entry()
        box.pack_start(first, True, True, 8)
        box.pack_start(second, True, True, 8)
        window.add(box)
        window.show_all()
        settled()
        wid = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--name', '^MSIME GTK CI$'], text=True).splitlines()[0]
        subprocess.run(['xdotool', 'windowfocus', '--sync', wid], check=True)
        first.grab_focus()
        wait_for(first.has_focus, 'First entry did not gain real focus')
        if not bus.set_global_engine('metasequoiaime'):
            raise AssertionError('Could not activate the product engine')
        settled()
        key('n', 'i', 'h', 'a', 'o', 'space')
        wait_for(lambda: bool(first.get_text()), 'Chinese commit did not reach the first GTK entry')
        original = first.get_text()
        if not re.search('[\u3400-\u9fff]', original):
            raise AssertionError(f'Input bypassed IBus/Engine: {original!r}')
        if second.get_text():
            raise AssertionError('Unfocused entry received input')
        # Leave a composition in the first field, then move the actual GTK focus.
        key('n', 'i')
        settled()
        second.grab_focus()
        wait_for(second.has_focus, 'Second entry did not gain real focus')
        settled()
        after_blur = first.get_text()
        if second.get_text():
            raise AssertionError('Composition leaked into the newly focused entry')
        key('h', 'a', 'o', 'space')
        wait_for(lambda: bool(second.get_text()), 'Chinese commit did not reach the second GTK entry')
        if not re.search('[\u3400-\u9fff]', second.get_text()):
            raise AssertionError(f'Second entry bypassed the engine: {second.get_text()!r}')
        if first.get_text() != after_blur:
            raise AssertionError('A commit for the new focus modified the previous entry')
        print(f'GTK input and focus isolation passed at GDK_SCALE={os.environ["GDK_SCALE"]}')
        window.destroy()
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        log.close()
        print((root / 'ibus.log').read_text(), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', nargs='?', default='build')
    parser.add_argument('--scale', choices=('1', '2'), default='1')
    parser.add_argument('--session', type=Path)
    args = parser.parse_args()
    if args.session:
        session(args.session)
        return
    project = Path(__file__).resolve().parents[1]
    build = Path(args.build).resolve()
    engine = build / 'metasequoia-ime-ibus'
    if not engine.is_file():
        raise RuntimeError(f'Missing built engine: {engine}')
    source = Path(os.environ.get('METASEQUOIA_IME_DATA_DIR', project / 'vendor/MetasequoiaImeDict/out')).resolve()
    with tempfile.TemporaryDirectory(prefix='msime-gtk-') as directory:
        root = Path(directory)
        env = dict(os.environ, GTK_IM_MODULE='ibus', IBUS_USE_PORTAL='0', GIO_USE_VFS='local',
                   GDK_BACKEND='x11', GDK_SCALE=args.scale, GDK_DPI_SCALE='1', NO_AT_BRIDGE='1')
        for name, folder in [('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache'),
                             ('XDG_DATA_HOME', 'user-data'), ('XDG_RUNTIME_DIR', 'run'),
                             ('IBUS_COMPONENT_PATH', 'components')]:
            path = root / folder
            path.mkdir(mode=0o700)
            env[name] = str(path)
        data = root / 'dictionary'
        data.mkdir()
        for name in ('msime.db', 'others.db', 'english.db'):
            if not (source / name).is_file():
                raise RuntimeError(f'Missing product dictionary: {source / name}')
            (data / name).symlink_to(source / name)
        helpcodes = source / 'helpcodes'
        (data / 'helpcodes').symlink_to(helpcodes if helpcodes.is_dir() else project / 'vendor/MetasequoiaImeEngine/helpcode/helpcodes')
        env['METASEQUOIA_IME_DATA_DIR'] = str(data)
        config = root / 'config/metasequoiaime'
        config.mkdir()
        (config / 'config.ini').write_text('[input]\nmode=ime\nscheme=quanpin\npunctuation=english\nfull-width=false\n')
        tree = ET.parse(build / 'metasequoiaime.xml')
        tree.getroot().find('exec').text = f'{engine} --ibus'
        tree.write(root / 'components/metasequoiaime.xml', encoding='unicode')
        subprocess.run(['dbus-run-session', '--', 'xvfb-run', '-a', '-s', '-screen 0 1600x1000x24',
                        sys.executable, str(Path(__file__).resolve()), '--session', str(root)],
                       env=env, check=True, timeout=90)


if __name__ == '__main__':
    main()
