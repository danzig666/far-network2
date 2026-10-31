# Far Manager Network plugin: fast and cancellable

A drop-in replacement for the `Network.dll` that comes with Far Manager, based on
the plugin source at `builds/3.0.6699`. It targets the Far 3 plugin API (needs
build 6682 or newer) and has been run on Far 3.0.6699 x64. The x86 build compiles
but hasn't been run yet. This is an unofficial build, not from the Far Group.

## What's different

| | Stock | This one |
|---|---|---|
| Opening a slow network | Far is frozen until Windows answers | Far stays usable, the scan runs in the background |
| Cancelling | Not possible | **Esc** stops the scan, or gets out of any blocking network call |
| Finding computers | Only the legacy browser (`WNetEnumResource`), which is gone from most networks | WS-Discovery as well, the same protocol as Explorer's Network folder |
| Results | All at once at the end | As they arrive |
| Coming back to a folder | Scans again | Shows the known list at once and refreshes it in the background |
| After a restart | Starts from nothing | Shows the last known list at once and refreshes it |

Measured on a LAN with no browse server, opening the network from cold:

| | Stock | This one |
|---|---|---|
| Far reacts to a key | 97-99 s | 0.02-0.08 s |
| First computer on screen | never: the legacy browser finds nothing there | about 0.2-1 s |
| List on screen after a restart | never, same reason | 0.03 s |

## Install

Download `far-network2-*.zip` from
[Releases](https://github.com/danzig666/far-network2/releases/latest) and extract it.
Close the Far you're replacing it in, then in the extracted folder run:

```bash
powershell -ExecutionPolicy Bypass -File install.ps1
```

If more than one Far is found, or to pick a portable one, add `-FarPath "D:\Tools\Far"`.
The script picks the x64 or x86 build to match `Far.exe`, asks for administrator
rights if the folder needs them, and keeps the original as `Network.dll.stock`.

To go back:

```bash
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

**By hand:** close Far, then copy `x64\Network.dll` (or `x86\`) over
`Plugins\Network\Network.dll`. That DLL is all it takes. It needs no runtime
installed, and the existing language files, help and settings keep working.

## Using it

- A panel title like `Net: WORKGROUP [Scanning the network... Esc]` means the scan
  is still running. You can keep working, or even leave the folder.
- **Esc** stops the scan and keeps what was found so far. If the command line has
  text in it, Esc clears that first, as usual.
- **Ctrl+R** rescans now.
- A wait box with `Esc: Cancel` appears when an action has to finish before Far can
  go on, such as connecting to a server. Esc gets you out straight away.

## Things to know

- Windows has no way to interrupt a blocked network call. Cancelling means Far stops
  waiting, and the abandoned call ends in the background, whenever Windows lets it.
  Quitting Far doesn't wait for it.
- The legacy browser is still asked, quietly in the background, because some machines
  only show up that way. Scanning doesn't wait for it, and anything it finds is added
  to the list when it turns up. That can be a minute later. It isn't asked again for
  24 hours after it fails, unless you press Ctrl+R.
- Opening a domain or the Windows network now sends a WS-Discovery multicast probe,
  as Explorer does, and asks each responding computer for its name over HTTP.
  **Scan network** in the plugin's settings turns this off along with the rest of
  the scanning.
- Discovery lists whatever answers on the LAN and says it's a computer. That's no
  proof of identity, as with Explorer's Network folder.
- Computer names, share names and comments are cached in the plugin's settings, as
  `ScanCache`. Passwords are not. The stock DLL ignores this value.

## Building

Needs the Visual C++ build tools and a Windows SDK.

```bash
build.cmd
```

Output goes to `dist\x64` and `dist\x86`, and `install.ps1` finds it there too.
`test\README.md` covers the tests.

Pushing a tag `v3.0.<PLUGIN_BUILD>` builds and publishes a release, with
`RELEASE_NOTES.md` as its text.

## License

BSD 3-Clause, as Far Manager itself: see [LICENSE](LICENSE).
