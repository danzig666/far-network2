"""Times ".." from the folder the plugin opens in, on the real LAN.

Usage: parent_timing.py <Far install dir> <work dir>
"""
import os
import re
import sys
import time
from compare_real import make_far
from fardriver import Far
from run_tests import ROOT, quit_far


def main():
    src, work = sys.argv[1], sys.argv[2]
    make_far(src, work, os.path.join(ROOT, 'dist', 'x64', 'Network.dll'))
    far = Far(work)
    try:
        far.press('f11')
        far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
        far.press('enter')
        far.wait_for(lambda s: 'Net:' in s.splitlines()[0] and re.search(r'^.\\\\', s, re.M), 30, 'opening folder')
        before = re.sub(r'[^\x20-\x7e]', '', far.screen().splitlines()[0])[:45]
        far.press('home')
        start = time.monotonic()
        far.press('enter')
        box = False
        while time.monotonic() - start < 120:
            screen = far.screen()
            box = box or 'Please wait' in screen
            if 'Please wait' not in screen and re.sub(r'[^\x20-\x7e]', '', screen.splitlines()[0])[:45] != before:
                break
            time.sleep(0.01)
        after = re.sub(r'[^\x20-\x7e]', '', far.screen().splitlines()[0])[:45]
        print(f'".." took {time.monotonic() - start:.2f}s, wait box seen: {box}', flush=True)
        print(f'  from: {before}\n  to:   {after}', flush=True)
        quit_far(far)
        far.wait_exit(30)
    finally:
        far.kill()


if __name__ == '__main__':
    main()
