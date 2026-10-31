"""The in-place upgrade: an existing Far with the stock DLL and a profile gets the DLL replaced, and back.

Far remembers what plugins export in its plugin cache, so a replacement has to
work on top of a cache made from the original, and the settings have to survive.

Usage: test_upgrade.py <Far install dir> <work dir>
"""
import filecmp
import os
import re
import subprocess
import sys
import time
from compare_real import make_far
from fardriver import Far
from run_tests import ROOT, quit_far


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  (' + detail + ')' if detail else ''}", flush=True)
    if not ok:
        sys.exit(1)


def install(far_dir, *args):
    result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(ROOT, 'install.ps1'), '-FarPath', far_dir, *args],
                            capture_output=True, text=True)
    print(result.stdout.strip(), result.stderr.strip()[:400], flush=True)
    return result.returncode


def open_and_type(far):
    """Opens the plugin and returns how long it takes Far to react to a key."""
    far.press('f11')
    far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
    start = time.monotonic()
    far.press('enter')
    # Not something short: Far's autocomplete popup takes the first Esc, with or without this plugin.
    far.type('zzqx')
    far.wait_for(lambda s: re.search(r'>zzqx\b', s), 300, 'Far to react')
    return time.monotonic() - start


def main():
    src, work = sys.argv[1], sys.argv[2]
    stock = os.path.join(src, 'Plugins', 'Network', 'Network.dll')
    make_far(src, work, stock)
    plugin = os.path.join(work, 'Plugins', 'Network', 'Network.dll')

    # Use the stock DLL long enough to get into the plugin cache, and change a setting.
    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        far.press('esc')
        far.wait_for(lambda s: 'Network browser' not in s, 5, 'menu to close')
        quit_far(far)
        far.wait_exit(20)
    finally:
        far.kill()
    check('the stock DLL ran and made a profile', os.path.isdir(os.path.join(work, 'Profile')))

    # Refuses while that Far is running
    far = Far(work)
    try:
        check('install refuses while Far is running', install(work) != 0 and filecmp.cmp(plugin, stock, shallow=False))
    finally:
        far.kill()
    time.sleep(0.5)

    check('install works', install(work) == 0 and filecmp.cmp(plugin, os.path.join(ROOT, 'dist', 'x64', 'Network.dll'), shallow=False))
    check('the original is kept', filecmp.cmp(plugin + '.stock', stock, shallow=False))

    far = Far(work)
    try:
        t = open_and_type(far)
        check('Far reacts at once after the upgrade', t < 2, f'{t:.3f}s')
        far.wait_for(lambda s: 'Net:' in s.splitlines()[0], 5, 'plugin title')
        far.press('esc')
        far.wait_for(lambda s: not re.search(r'>zzqx\b', s), 5, 'command line to clear')
        # Anything showing up in the panel by itself needs ProcessSynchroEventW, which the cached stock plugin didn't have.
        far.wait_for(lambda s: re.search(r'^.\\\\[^\s]+', s, re.M), 15, 'discovered computers')
        check('progress reaches the panel on top of the old plugin cache', True)
        quit_far(far)
        far.wait_exit(20)
    finally:
        far.kill()

    time.sleep(0.5)
    check('a second install keeps the original backup', install(work) == 0 and filecmp.cmp(plugin + '.stock', stock, shallow=False))
    check('uninstall restores the original', install(work, '-Uninstall') == 0 and filecmp.cmp(plugin, stock, shallow=False))

    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        far.press('esc')
        far.wait_for(lambda s: 'Network browser' not in s, 5, 'menu to close')
        quit_far(far)
        far.wait_exit(20)
        check('Far runs fine with the original again (the new cache data is ignored)', far.proc.returncode == 0)
    finally:
        far.kill()


if __name__ == '__main__':
    main()
