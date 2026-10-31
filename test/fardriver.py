"""Runs Far in its own hidden console, types into it and reads its screen."""
import ctypes
import os
import subprocess
import time
from ctypes import wintypes as w

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

CREATE_NEW_CONSOLE = 0x10
STARTF_USESHOWWINDOW = 1
SW_HIDE = 0
GENERIC_RW = 0xC0000000
FILE_SHARE_RW = 3
OPEN_EXISTING = 3


class COORD(ctypes.Structure):
    _fields_ = [('X', w.SHORT), ('Y', w.SHORT)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [('Left', w.SHORT), ('Top', w.SHORT), ('Right', w.SHORT), ('Bottom', w.SHORT)]


class CHAR_INFO(ctypes.Structure):
    _fields_ = [('Char', w.WCHAR), ('Attributes', w.WORD)]


class CSBI(ctypes.Structure):
    _fields_ = [('dwSize', COORD), ('dwCursorPosition', COORD), ('wAttributes', w.WORD),
                ('srWindow', SMALL_RECT), ('dwMaximumWindowSize', COORD)]


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [('bKeyDown', w.BOOL), ('wRepeatCount', w.WORD), ('wVirtualKeyCode', w.WORD),
                ('wVirtualScanCode', w.WORD), ('uChar', w.WCHAR), ('dwControlKeyState', w.DWORD)]


class _EV(ctypes.Union):
    _fields_ = [('KeyEvent', KEY_EVENT_RECORD), ('pad', ctypes.c_byte * 16)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [('EventType', w.WORD), ('Event', _EV)]


k32.CreateFileW.restype = w.HANDLE
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k32.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, ctypes.POINTER(CSBI)]
k32.ReadConsoleOutputW.argtypes = [w.HANDLE, ctypes.POINTER(CHAR_INFO), COORD, COORD, ctypes.POINTER(SMALL_RECT)]
k32.WriteConsoleInputW.argtypes = [w.HANDLE, ctypes.POINTER(INPUT_RECORD), w.DWORD, ctypes.POINTER(w.DWORD)]
k32.SetConsoleScreenBufferSize.argtypes = [w.HANDLE, COORD]
k32.SetConsoleWindowInfo.argtypes = [w.HANDLE, w.BOOL, ctypes.POINTER(SMALL_RECT)]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.AttachConsole.argtypes = [w.DWORD]

VK = {'esc': 0x1B, 'enter': 0x0D, 'tab': 0x09, 'down': 0x28, 'up': 0x26, 'home': 0x24,
      'f4': 0x73, 'f5': 0x74, 'f10': 0x79, 'f11': 0x7A, 'pgup': 0x21, 'pgdn': 0x22}
CTRL = 0x0008
SHIFT = 0x0010


class Far:
    def __init__(self, far_dir, env=None, args=()):
        self.env = dict(os.environ)
        self.env.update(env or {})
        si = subprocess.STARTUPINFO()
        si.dwFlags = STARTF_USESHOWWINDOW
        si.wShowWindow = SW_HIDE
        self.proc = subprocess.Popen([os.path.join(far_dir, 'Far.exe'), *args], cwd=far_dir, env=self.env,
                                     creationflags=CREATE_NEW_CONSOLE, startupinfo=si)
        time.sleep(0.5)
        self._attach()
        self.wait_for(lambda s: 'bin' in s or '..' in s or 'Far.exe' in s.replace(' ', ''), 15, 'Far to start')

    def _attach(self):
        k32.FreeConsole()
        for _ in range(50):
            if k32.AttachConsole(self.proc.pid):
                break
            time.sleep(0.1)
        else:
            raise OSError(ctypes.get_last_error(), 'AttachConsole')
        self.hin = k32.CreateFileW('CONIN$', GENERIC_RW, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)
        self.hout = k32.CreateFileW('CONOUT$', GENERIC_RW, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)

    def _refresh_out(self):
        # Far may have switched the active screen buffer.
        k32.CloseHandle(self.hout)
        self.hout = k32.CreateFileW('CONOUT$', GENERIC_RW, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)

    def screen(self):
        self._refresh_out()
        info = CSBI()
        if not k32.GetConsoleScreenBufferInfo(self.hout, ctypes.byref(info)):
            return ''
        win = info.srWindow
        cols, rows = win.Right - win.Left + 1, win.Bottom - win.Top + 1
        buf = (CHAR_INFO * (cols * rows))()
        region = SMALL_RECT(win.Left, win.Top, win.Right, win.Bottom)
        if not k32.ReadConsoleOutputW(self.hout, buf, COORD(cols, rows), COORD(0, 0), ctypes.byref(region)):
            return ''
        return '\n'.join(''.join(buf[y * cols + x].Char for x in range(cols)).rstrip() for y in range(rows))

    def _key(self, vk, ch='\0', state=0):
        recs = (INPUT_RECORD * 2)()
        for i, down in enumerate((1, 0)):
            recs[i].EventType = 1
            k = recs[i].Event.KeyEvent
            k.bKeyDown, k.wRepeatCount, k.wVirtualKeyCode, k.uChar, k.dwControlKeyState = down, 1, vk, ch, state
        written = w.DWORD()
        if not k32.WriteConsoleInputW(self.hin, recs, 2, ctypes.byref(written)):
            raise OSError(ctypes.get_last_error(), 'WriteConsoleInput')

    def press(self, name, ctrl=False, shift=False):
        state = (CTRL if ctrl else 0) | (SHIFT if shift else 0)
        if len(name) == 1:
            ch = name.upper()
            self._key(ord(ch), '\x12' if ch == 'R' and ctrl else ('\0' if ctrl else name), state)
        else:
            self._key(VK[name], '\x1b' if name == 'esc' else ('\r' if name == 'enter' else '\0'), state)

    def type(self, text):
        for c in text:
            self._key(ord(c.upper()) if c.isalnum() else 0, c, SHIFT if c.isupper() else 0)

    def wait_for(self, pred, timeout, what=''):
        """Returns the seconds it took; raises with a screen dump on timeout."""
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            s = self.screen()
            if s and pred(s):
                return time.monotonic() - start
            if self.proc.poll() is not None:
                raise RuntimeError(f'Far exited ({self.proc.returncode:#x}) while waiting for {what}')
            time.sleep(0.02)
        raise TimeoutError(f'{what}: not within {timeout}s. Screen:\n{self.screen()}')

    def wait_exit(self, timeout):
        start = time.monotonic()
        self.proc.wait(timeout)
        return time.monotonic() - start

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
        k32.FreeConsole()
