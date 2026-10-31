# Tests

These run real Far in a private copy, with its own profile and in a hidden console.
They press keys and read the screen. An installed Far and its settings aren't touched.

Needs Python 3 and an installed Far to copy. `<work dir>` gets deleted and recreated.

## `run_tests.py`: fake network

```bash
build.cmd test
python test/run_tests.py "C:\Program Files\Far Manager" "%TEMP%\fartest"
```

`build.cmd test` builds `build\testdll\Network.dll` with `NETWORK_TEST_HOOKS`, which
replaces the network with a fake one controlled by environment variables:

| Variable | Meaning |
|---|---|
| `FARNET_TEST_SERVERS` | Number of fake servers. Setting it turns the fake on. |
| `FARNET_TEST_DELAY_MS` | How long every "network call" takes. 600000 stands in for a hung provider. |
| `FARNET_TEST_DISCOVERY` | Set to 1 to have discovery find `\\\\DISC000` at once and the fake servers come from the slow legacy browser. |

Never install this DLL. The release builds don't contain the hooks.

## `test_upgrade.py`: replacing the DLL

Stock DLL with an existing profile and plugin cache, then `install.ps1`, then
`-Uninstall`. Needs a LAN with at least one computer that answers WS-Discovery.

## `real_lan.py` and `compare_real.py`: your network

`real_lan.py` tries the release DLL from cold and browses into this computer only.
`compare_real.py` times the stock DLL against the new one. Both depend on the LAN, so
they report numbers rather than pass or fail.

Far's command line takes two Esc presses for short input such as `q`, with the stock
plugin as well, most likely because an autocomplete popup takes the first. That's why
the tests type `zzqx`.
