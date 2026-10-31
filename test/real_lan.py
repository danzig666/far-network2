"""Tries the release DLL on the real LAN, in a private copy of Far, starting without any cache.

Only ever browses into this computer itself.

Usage: real_lan.py <Far install dir> <work dir>
"""
import os
import re
import socket
import sys
import time
from compare_real import make_far
from fardriver import Far
from run_tests import ROOT, quit_far


def computers(screen):
    # Only the left panel, that's where the plugin is.
    return sorted(set(re.findall(r'^.(\\\\[^\s│|]+)', screen, re.M)))


def main():
    src, work = sys.argv[1], sys.argv[2]
    make_far(src, work, os.path.join(ROOT, 'dist', 'x64', 'Network.dll'))
    me = socket.gethostname().upper()

    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        start = time.monotonic()
        far.press('enter')

        far.wait_for(lambda s: computers(s), 30, 'first computer')
        print(f'first computer after {time.monotonic() - start:.2f}s', flush=True)

        time.sleep(6 - min(6, time.monotonic() - start))
        found = computers(far.screen())
        print(f'after {time.monotonic() - start:.1f}s: {len(found)} computers: {found}', flush=True)
        print('still scanning:', 'Esc]' in far.screen(), flush=True)

        far.wait_for(lambda s: 'Esc]' not in s, 120, 'scan to end')
        print(f'scan over after {time.monotonic() - start:.1f}s (legacy browser gave up)', flush=True)
        found = computers(far.screen())
        print(f'final: {len(found)} computers: {found}', flush=True)
        for line in far.screen().splitlines()[2:18]:
            print('   ', line[:62], flush=True)

        far.press('home')
        for _ in range(40):
            status = far.screen().splitlines()[-4]
            if me in status.upper():
                break
            far.press('down')
            time.sleep(0.05)
        else:
            print(f'{me} is not in the list, not browsing anywhere', flush=True)
            quit_far(far)
            far.wait_exit(30)
            return

        entered = time.monotonic()
        far.press('enter')
        far.wait_for(lambda s: f'\\\\{me}\\' in s.upper() or f'NET: \\\\{me}' in s.upper(), 30, 'own computer')
        print(f'entered {me} after {time.monotonic() - entered:.2f}s', flush=True)
        far.wait_for(lambda s: 'Esc]' not in s, 30, 'share listing')
        print(f'shares listed after {time.monotonic() - entered:.2f}s', flush=True)
        for line in far.screen().splitlines()[2:8]:
            print('   ', line[:62], flush=True)

        # Opening a share closes the plugin panel from inside SetDirectory (the upstream gh-631 crash).
        lines = [l[1:28].strip() for l in far.screen().splitlines()]
        share = next((l for l in lines if l.upper().startswith(f'\\\\{me}\\') and not l.endswith('$') and '}' not in l), None)
        if share:
            far.press('home')
            for _ in range(20):
                if share in far.screen().splitlines()[-4]:
                    break
                far.press('down')
                time.sleep(0.05)
            far.press('enter')
            far.wait_for(lambda s: share.upper() in s.splitlines()[0].upper() and 'Net:' not in s.splitlines()[0], 15, 'the share as a file panel')
            print(f'opened {share} as a normal file panel', flush=True)
            far.press('home')
            far.press('enter')
            far.wait_for(lambda s: f'Net: \\\\{me}' in s.splitlines()[0], 15, 'back from the share root')
            print('".." from the share root is back in the plugin', flush=True)
            far.wait_for(lambda s: share in s.splitlines()[-4], 5, 'cursor on the share')
            print('...with the cursor on the share', flush=True)
        else:
            print('no suitable share to open', flush=True)

        far.press('home')
        back = time.monotonic()
        far.press('enter')
        far.wait_for(lambda s: len(computers(s)) >= len(found) and f'\\\\{me}\\' not in s.upper(), 5, 'back in the domain')
        print(f'back up after {time.monotonic() - back:.3f}s, no network involved', flush=True)
        quit_far(far)
        far.wait_exit(30)
    finally:
        far.kill()

    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        start = time.monotonic()
        far.press('enter')
        far.wait_for(lambda s: computers(s), 10, 'cached computers')
        print(f'after restart: list on screen after {time.monotonic() - start:.3f}s, {len(computers(far.screen()))} computers', flush=True)
        far.wait_for(lambda s: 'Esc]' not in s, 60, 'refresh to end')
        print(f'after restart: refresh over after {time.monotonic() - start:.1f}s', flush=True)
        quit_far(far)
        print(f'exit code {far.wait_exit(30) and far.proc.returncode}', flush=True)
    finally:
        far.kill()


if __name__ == '__main__':
    main()
