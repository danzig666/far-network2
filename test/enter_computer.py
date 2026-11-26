"""Opens the network with the release DLL in a private copy of Far and presses Enter on one computer.

Reports which computers were found, and how long Enter took to show shares, a password prompt
or an error. Nothing is typed into a password prompt: it is closed with Esc.

Usage: enter_computer.py <Far install dir> <work dir> <computer name> [<DLL>]
"""
import os
import re
import sys
import time
from compare_real import make_far
from fardriver import Far
from run_tests import ROOT, quit_far


def computers(screen):
    return sorted(set(re.findall(r'^.(\\\\[^\s│|]+)', screen, re.M)))


def outcome(screen, name):
    up = screen.upper()
    if 'PASSWORD' in up or 'USER NAME' in up:
        return 'password prompt'
    if re.search(r'0x[0-9a-f]{8}', screen, re.I) or 'ERROR' in up:
        return 'error'
    if f'\\\\{name}\\' in up and 'ESC]' not in up:
        return 'shares'
    return None


def main():
    src, work, name = sys.argv[1], sys.argv[2], sys.argv[3].upper()
    dll = sys.argv[4] if len(sys.argv) > 4 else os.path.join(ROOT, 'dist', 'x64', 'Network.dll')
    make_far(src, work, dll)

    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        start = time.monotonic()
        far.press('enter')
        far.wait_for(lambda s: computers(s), 30, 'first computer')
        print(f'first computer after {time.monotonic() - start:.2f}s', flush=True)
        far.wait_for(lambda s: name in ' '.join(computers(s)).upper(), 30, name)
        print(f'{name} after {time.monotonic() - start:.2f}s', flush=True)
        try:
            far.wait_for(lambda s: 'Esc]' not in s, 20, 'scan to end')
        except TimeoutError:
            print('scan still running after 20s', flush=True)
        print(f'found after {time.monotonic() - start:.1f}s: {computers(far.screen())}', flush=True)

        far.press('home')
        for _ in range(40):
            if f'\\\\{name}' in far.screen().splitlines()[-4].upper():
                break
            far.press('down')
            time.sleep(0.05)
        else:
            raise RuntimeError(f'could not put the cursor on {name}')

        entered = time.monotonic()
        far.press('enter')
        far.wait_for(lambda s: outcome(s, name), 180, 'outcome of Enter')
        screen = far.screen()
        print(f'Enter on {name}: {outcome(screen, name)} after {time.monotonic() - entered:.2f}s', flush=True)
        for line in screen.splitlines():
            if line.strip():
                print('   ', line[:100], flush=True)
        closed = time.monotonic()
        far.press('esc')
        # Closing the answer mustn't start another round of asking the network.
        far.wait_for(lambda s: 'Please wait' not in s and 'Error' not in s and 'Esc]' not in s, 180, 'panel after Esc')
        print(f'panel back {time.monotonic() - closed:.2f}s after Esc', flush=True)
        time.sleep(0.5)
        if 'Please wait' in far.screen():
            print('a wait box came up again', flush=True)
        far.press('esc')
        quit_far(far)
        far.wait_exit(30)
    finally:
        far.kill()


if __name__ == '__main__':
    main()
