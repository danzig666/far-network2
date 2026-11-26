A drop-in replacement for the `Network.dll` of Far Manager that doesn't freeze Far while it looks for computers.

- **Far stays usable:** scans run in the background and results show up as they arrive.
- **Esc cancels:** it stops a scan, or gets you out of a blocking network call.
- **Computers are found through WS-Discovery as well,** the protocol of Explorer's Network folder. The legacy browser the stock plugin relies on is gone from most networks.
- **Lists are cached,** also across restarts, and refreshed in the background. Ctrl+R rescans.

## New in 3.0.109

- **Finds computers that have network discovery turned off** but share files: a NetBIOS query goes to every address of the local subnets (up to a /22) alongside WS-Discovery.
- **Waits longer for WS-Discovery answers** (4 s, three probes): Windows computers often answer only after 2 s or more, and some were missed.
- **No more half-minute wait on a computer whose firewall blocks file sharing:** Enter says "The network path was not found" within 2 s instead of 40 s, and doesn't start another wait once you've closed the error.

## Install

Download `far-network2-*.zip` (not the `-symbols` one) and extract it. Close Far, then in the extracted folder run:

```
powershell -ExecutionPolicy Bypass -File install.ps1
```

It picks the build that matches your `Far.exe` and keeps the original as `Network.dll.stock`. `install.ps1 -Uninstall` puts the original back. To do it by hand, copy `x64\Network.dll` or `x86\Network.dll` over `Plugins\Network\Network.dll`.

Language files, help and settings stay as they are. Needs Far 3 build 6682 or newer.

## Tested and not tested

Run end to end on Far 3.0.6699 x64.

**Not run yet:**
- the x86 build (it compiles and its exports are correct)
- mapping and disconnecting drives
- typing into the password prompt (it does come up)
- adding and removing favorites
- a network with a working browse server

This is an unofficial build, not from the Far Group, and the DLL isn't code signed.
