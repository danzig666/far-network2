"""End-to-end tests of the test build of Network.dll, in a private copy of Far.

The test build (build.cmd test) replaces the network with a fake one whose every
call takes FARNET_TEST_DELAY_MS, so the outcome doesn't depend on the LAN.

Usage: run_tests.py <Far install dir> <work dir>
"""
import os
import re
import shutil
import sys
import time
from fardriver import Far

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESPONSIVE = 1.0  # seconds; Far must react to a key within this while a scan runs
results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  (' + detail + ')' if detail else ''}", flush=True)


def fakes(screen):
    return len(set(re.findall(r'FAKE\d{3}', screen)))


def make_far(src, work):
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(os.path.join(work, 'Plugins', 'Network'))
    for name in os.listdir(src):
        path = os.path.join(src, name)
        if os.path.isfile(path):
            shutil.copy(path, work)
    net = os.path.join(src, 'Plugins', 'Network')
    for name in os.listdir(net):
        if name.endswith(('.lng', '.hlf')):
            shutil.copy(os.path.join(net, name), os.path.join(work, 'Plugins', 'Network'))
    shutil.copy(os.path.join(ROOT, 'build', 'testdll', 'Network.dll'), os.path.join(work, 'Plugins', 'Network'))
    with open(os.path.join(work, 'Far.exe.ini'), 'w') as ini:
        ini.write('[General]\nUseSystemProfiles=0\nUserProfileDir=%FARHOME%\\Profile\nUserLocalProfileDir=%FARHOME%\\Profile\n')


def quit_far(far):
    far.press('f10')
    # Not just "Quit": that is on the key bar all the time, and a dialog drops keys typed ahead of it.
    far.wait_for(lambda s: 'Do you want to quit' in s, 5, 'quit prompt')
    far.press('enter')


def open_network(far):
    far.press('f11')
    far.wait_for(lambda s: 'Network browser' in s, 5, 'plugins menu')
    far.press('enter')


def test_incremental_and_responsive(far_dir):
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '30', 'FARNET_TEST_DELAY_MS': '200'})
    try:
        open_network(far)
        # Resolving the local network goes through a wait box first.
        t = far.wait_for(lambda s: 'Scanning the network... Esc]' in s, 5, 'scan to show in title')
        check('scan shows in the panel title', True, f'{t:.2f}s')

        far.wait_for(lambda s: fakes(s) >= 1, 5, 'first server')
        first = fakes(far.screen())
        far.wait_for(lambda s: fakes(s) >= first + 3, 5, 'more servers')
        check('servers appear incrementally', 'Esc]' in far.screen(), f'{first} then {fakes(far.screen())} while still scanning')

        # Not something short: Far's autocomplete popup takes the first Esc, with or without this plugin.
        far.type('zzqx')
        t = far.wait_for(lambda s: re.search(r'>zzqx\b', s), 5, 'typed text')
        check('Far accepts typing during a scan', t < RESPONSIVE, f'{t:.3f}s')
        far.press('esc')  # with text in the command line Esc belongs to it
        far.wait_for(lambda s: 'zzqx' not in s, 3, 'command line to clear')
        check('Esc clears the command line before it cancels', 'Esc]' in far.screen())

        far.press('tab')
        far.press('tab')
        far.press('esc')
        t = far.wait_for(lambda s: 'Esc]' not in s, 3, 'scan to stop')
        check('Esc cancels the scan', t < RESPONSIVE, f'{t:.3f}s')
        stopped = fakes(far.screen())
        time.sleep(1.5)
        check('late results are rejected', fakes(far.screen()) == stopped and 'Esc]' not in far.screen(), f'stayed at {stopped}')

        far.press('r', ctrl=True)
        t = far.wait_for(lambda s: 'Esc]' in s, 3, 'rescan')
        check('Ctrl+R rescans after a cancel', True, f'{t:.3f}s')
        far.wait_for(lambda s: 'Esc]' not in s, 15, 'rescan to finish')
        check('rescan runs to the end', True)
        quit_far(far)
        check('clean exit', far.wait_exit(10) < 10 and far.proc.returncode == 0, f'code {far.proc.returncode}')
    finally:
        far.kill()


def test_navigation_and_cache(far_dir):
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '5', 'FARNET_TEST_DELAY_MS': '100'})
    try:
        open_network(far)
        far.wait_for(lambda s: fakes(s) == 5 and 'Esc]' not in s, 10, 'domain listing')
        far.press('home')
        far.press('down')
        far.press('down')
        far.press('enter')
        far.wait_for(lambda s: 'docs' in s and 'media' in s, 5, 'shares of a server')
        check('entering a server lists its shares', True)
        far.press('home')
        far.press('enter')
        t = far.wait_for(lambda s: fakes(s) == 5, 3, 'back at the domain')
        check('going back shows the cached list at once', t < 0.5, f'{t:.3f}s')
        quit_far(far)
        far.wait_exit(10)
    finally:
        far.kill()

    # Nothing about the network has to be asked for to show the last known state.
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '5', 'FARNET_TEST_DELAY_MS': '3000'})
    try:
        open_network(far)
        t = far.wait_for(lambda s: fakes(s) == 5, 2, 'restored listing')
        check('listing survives a restart', t < 1, f'{t:.3f}s although the network now takes 3s per call')
        check('...and is being refreshed', 'Esc]' in far.screen())
    finally:
        far.kill()


def test_hung_network(far_dir):
    # A provider that doesn't answer for ten minutes.
    env = {'FARNET_TEST_SERVERS': '5', 'FARNET_TEST_DELAY_MS': '600000'}
    far = Far(far_dir, env)
    try:
        open_network(far)
        far.wait_for(lambda s: fakes(s) == 5, 3, 'restored listing')
        far.press('home')
        far.press('down')
        far.press('down')
        far.press('down')
        far.press('down')
        far.press('enter')
        # Nothing fresh about this one: this has to ask the network, and that hangs.
        t = far.wait_for(lambda s: 'Please wait' in s and 'Esc: Cancel' in s, 3, 'wait box')
        check('a blocking call shows a wait box', True, f'{t:.3f}s')
        far.press('esc')
        t = far.wait_for(lambda s: 'Please wait' not in s, 3, 'wait box to go')
        check('Esc gets out of a hung call', t < RESPONSIVE, f'{t:.3f}s')
        check('...and stays in the same folder', fakes(far.screen()) == 5)

        far.type('zzqx')
        t = far.wait_for(lambda s: re.search(r'>zzqx\b', s), 3, 'typed text')
        check('Far accepts typing while a scan hangs', t < RESPONSIVE, f'{t:.3f}s')
        far.press('esc')
        far.wait_for(lambda s: 'zzqx' not in s, 3, 'command line to clear')

        # The folder the plugin opens in was reached without going through its parent.
        far.press('home')
        far.press('enter')
        t = far.wait_for(lambda s: 'Microsoft Windows Network' in s.splitlines()[0], 3, 'the network above the domain')
        check('".." from the opening folder needs no network', 'Please wait' not in far.screen() and t < RESPONSIVE, f'{t:.3f}s')

        # The workers are still stuck in "network calls". Quitting must not wait for them or crash.
        quit_far(far)
        t = far.wait_exit(10)
        check('quitting with hung workers is quick and clean', t < 5 and far.proc.returncode == 0, f'{t:.2f}s, code {far.proc.returncode}')
    finally:
        far.kill()


def test_hung_first_open(far_dir, work):
    # No cache at all, and the very first question hangs.
    shutil.rmtree(os.path.join(work, 'Profile'), ignore_errors=True)
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '5', 'FARNET_TEST_DELAY_MS': '600000'})
    try:
        open_network(far)
        t = far.wait_for(lambda s: 'Please wait' in s, 3, 'wait box')
        check('first open on a hung network shows a wait box', True, f'{t:.3f}s')
        far.press('esc')
        t = far.wait_for(lambda s: 'Please wait' not in s, 3, 'wait box to go')
        check('Esc gets out of it', t < RESPONSIVE, f'{t:.3f}s')
        far.wait_for(lambda s: 'Esc]' in s, 3, 'root scan')
        far.press('esc')
        far.wait_for(lambda s: 'Esc]' not in s, 3, 'root scan to stop')
        check('the panel opens at the root, root scan cancelled', 'Favorites' in far.screen() or 'Far Favorites' in far.screen())

        # Favorites don't need the network, however hung it is.
        far.press('home')
        for _ in range(5):
            if 'Favorites' in far.screen().splitlines()[-4]:
                break
            far.press('down')
            time.sleep(0.05)
        far.press('enter')
        t = far.wait_for(lambda s: 'Net: Favorites' in s.splitlines()[0], 3, 'favorites folder')
        check('favorites open on a hung network', t < RESPONSIVE, f'{t:.3f}s')
        far.press('home')
        far.press('enter')
        t = far.wait_for(lambda s: 'Net: Favorites' not in s.splitlines()[0], 3, 'back from favorites')
        check('...and back', t < RESPONSIVE, f'{t:.3f}s')
        quit_far(far)
        t = far.wait_exit(10)
        check('quit is clean', far.proc.returncode == 0, f'{t:.2f}s, code {far.proc.returncode}')
    finally:
        far.kill()


def test_slow_legacy_browser(far_dir, work):
    # Discovery answers at once, and the legacy browser takes its time but does find something.
    shutil.rmtree(os.path.join(work, 'Profile'), ignore_errors=True)
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '3', 'FARNET_TEST_DELAY_MS': '1500', 'FARNET_TEST_DISCOVERY': '1'})
    try:
        open_network(far)
        far.wait_for(lambda s: 'DISC000' in s, 10, 'discovered computer')
        t = far.wait_for(lambda s: 'Esc]' not in s, 1, 'scanning status to go')
        check('scanning ends with discovery', fakes(far.screen()) < 3, f'{t:.3f}s, {fakes(far.screen())} of 3 from the legacy browser so far')
        t = far.wait_for(lambda s: fakes(s) == 3, 15, 'legacy results')
        check('the legacy results are added without a scanning status', 'DISC000' in far.screen() and 'Esc]' not in far.screen(), f'{t:.2f}s later')
        time.sleep(2)
        check('the list stays complete once the legacy browser is done', fakes(far.screen()) == 3 and 'DISC000' in far.screen())
        quit_far(far)
        far.wait_exit(10)
    finally:
        far.kill()

    # Quitting before the legacy browser is done must not cost us the list.
    far = Far(far_dir, {'FARNET_TEST_SERVERS': '3', 'FARNET_TEST_DELAY_MS': '600000', 'FARNET_TEST_DISCOVERY': '1'})
    try:
        open_network(far)
        far.wait_for(lambda s: 'DISC000' in s and fakes(s) == 3 and 'Esc]' not in s, 5, 'cached list and discovery')
        quit_far(far)
        far.wait_exit(10)
        check('quitting while the legacy browser hangs is clean', far.proc.returncode == 0, f'code {far.proc.returncode}')
    finally:
        far.kill()

    far = Far(far_dir, {'FARNET_TEST_SERVERS': '3', 'FARNET_TEST_DELAY_MS': '600000', 'FARNET_TEST_DISCOVERY': '1'})
    try:
        open_network(far)
        t = far.wait_for(lambda s: 'DISC000' in s, 2, 'saved list')
        check('...and keeps the list', fakes(far.screen()) == 3, f'{t:.3f}s')
    finally:
        far.kill()


def main():
    src, work = sys.argv[1], sys.argv[2]
    make_far(src, work)
    for test in (lambda: test_incremental_and_responsive(work), lambda: test_navigation_and_cache(work),
                 lambda: test_hung_network(work), lambda: test_hung_first_open(work, work),
                 lambda: test_slow_legacy_browser(work, work)):
        try:
            test()
        except Exception as e:
            check(getattr(test, '__name__', 'test'), False, f'{type(e).__name__}: {e}')
    failed = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
