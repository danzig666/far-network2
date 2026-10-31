"""Opens the real network with the stock and the new DLL, each in a private copy of Far.

Measures how long Far doesn't react: after opening the plugin we type a character
and wait for it to appear on the command line.

Usage: compare_real.py <Far install dir> <work dir> [rounds] [stock,new]
"""
import os
import re
import shutil
import sys
import time
from fardriver import Far
from run_tests import ROOT, quit_far


def make_far(src, work, dll):
    if os.path.exists(work):
        shutil.rmtree(work)
    target = os.path.join(work, 'Plugins', 'Network')
    os.makedirs(target)
    for name in os.listdir(src):
        path = os.path.join(src, name)
        if os.path.isfile(path):
            shutil.copy(path, work)
    for name in os.listdir(os.path.join(src, 'Plugins', 'Network')):
        if name.endswith(('.lng', '.hlf')):
            shutil.copy(os.path.join(src, 'Plugins', 'Network', name), target)
    shutil.copy(dll, target)
    with open(os.path.join(work, 'Far.exe.ini'), 'w') as ini:
        ini.write('[General]\nUseSystemProfiles=0\nUserProfileDir=%FARHOME%\\Profile\nUserLocalProfileDir=%FARHOME%\\Profile\n')


def measure(far_dir):
    far = Far(far_dir)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        start = time.monotonic()
        far.press('enter')
        far.type('q')
        # Keep answering the wait box's absence: the character shows up once Far runs again.
        far.wait_for(lambda s: re.search(r'>q\b', s), 300, 'Far to react')
        reacted = time.monotonic() - start
        far.press('esc')  # clears the command line
        time.sleep(0.1)
        settled = None
        try:
            far.wait_for(lambda s: 'Esc]' not in s, 300, 'scan')
            settled = time.monotonic() - start
        except TimeoutError:
            pass
        screen = far.screen()
        title = next((l for l in screen.splitlines() if 'Net:' in l), '')
        title = re.sub(r'[^\x20-\x7e]', '', title.split('Net:')[1])[:60] if title else ''
        quit_far(far)
        far.wait_exit(30)
        return reacted, settled, title
    finally:
        far.kill()


def main():
    src, work = sys.argv[1], sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    labels = sys.argv[4].split(',') if len(sys.argv) > 4 else ['stock', 'new']
    dlls = {'stock': os.path.join(src, 'Plugins', 'Network', 'Network.dll'),
            'new': os.path.join(ROOT, 'dist', 'x64', 'Network.dll')}
    for label, dll in ((label, dlls[label]) for label in labels):
        far_dir = os.path.join(work, label)
        make_far(src, far_dir, dll)
        for round_no in range(rounds):
            reacted, settled, title = measure(far_dir)
            settled_text = f'{settled:.2f}s' if settled is not None else 'n/a'
            print(f'{label:5} open #{round_no + 1}: Far reacted after {reacted:.2f}s, listing done after {settled_text}, title:{title!r}', flush=True)


if __name__ == '__main__':
    main()
