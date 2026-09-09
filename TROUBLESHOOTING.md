# Troubleshooting

Every entry here is a failure that actually happened during development, with
the diagnostic that identified it. Several took a long time to find because the
symptom pointed somewhere other than the cause.

---

## Nothing arrives at the PC

### The sender looks alive but sends nothing

The single most misleading failure. On macOS and Linux, **Ctrl-Z suspends a
process rather than stopping it**. A suspended sender stays in the process
table, `pgrep` finds it, and it transmits absolutely nothing — no input, no
heartbeat, no error. Every retry adds another suspended process.

```bash
ps -o pid,stat,args -p $(pgrep -f "build/sender")
```

`T` in the STAT column means suspended. Bring it back with `fg`, or kill it and
restart. **Exit the sender with Ctrl-C, never Ctrl-Z.**

That same command prints the full argument list, which catches the other
candidate: a wrong destination address. UDP gives the sender **no feedback**, so
a typo'd IP produces a sender that runs happily, prints its slot lines, and
sends into the void. That failure is indistinguishable from success at the
sending end.

### Confirming the network independently of this software

Listen on the PC with the receiver **stopped**:

```powershell
pwsh -File tests\udp-probe.ps1 -Seconds 30
```

and from the sending machine:

```bash
echo hello | nc -u -w1 <pc_ip> 47800
```

If `hello` arrives, the network, the firewall rule and any OS-level local
network permission are all fine, and the fault is in the sender or its
arguments. If nothing arrives, it is the network or a firewall.

Do **not** use `ping` as a reachability test in one direction: a default Windows
install does not answer ICMP echo, so the PC looks dead while UDP works
perfectly. Pinging *from* the PC *to* the sender is a valid test.

### The PC's address changed

`<pc_ip>` is usually a DHCP lease. When it moves, the sender keeps running and
keeps sending nowhere.

```powershell
(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.*' }).IPAddress
```

Note that a PC often has several IPv4 addresses — Wi-Fi, Ethernet, plus
Hyper-V/WSL virtual adapters. Use the one on the same subnet as the sender. The
Android app avoids this entirely by discovering the PC by broadcast.

### `[net] sender is alive` never appears

In older builds this message printed at most **once per receiver process** and
almost never at all: any input packet silently set the same flag the heartbeat
checked, and at 120 Hz versus 1 Hz input always won the race. Its absence meant
nothing.

Current builds log real events instead — `[net] sender connected <ip>` and
`[net] sender LOST (no packets for N s)` — which repeat across reconnects.

---

## Virtual pads

### Pads rest at `LX=-3356 LY=-1869` instead of centred

A ViGEm pad that has never had a report applied reports leftover driver-side
values, and it looks exactly like a broken axis or byte-order path. It is
neither.

The mechanism: **ViGEm suppresses a report identical to the previously submitted
one**, and the client's cached report starts all-zero — so submitting a neutral
report right after `vigem_target_add` is a no-op and the stale values persist.
The fix in `receiver-win/main.cpp` nudges one axis one count off centre, then
submits the real neutral so it registers as a change.

If you see this on a pad with no sender behind it on an older build, it is
cosmetic: the values sit inside XInput's 7849 deadzone, so games ignore them.

### `vigem_target_add(N) failed`

Get the error code, which the receiver now prints. `tests/vigem-diag` prints it
for each step in isolation:

- `BUS_NOT_FOUND` — driver not installed, or needs a reboot
- `BUS_VERSION_MISMATCH` — ViGEmClient and ViGEmBus versions disagree
- `TIMED_OUT` / transient — the bus has not settled yet

A transient failure immediately after installing ViGEmBus is normal; wait a
moment and retry before investigating further.

### A crashed receiver left a phantom pad behind

It does not. ViGEmBus reclaims a process's targets when its handle closes at
exit — verified by a process that adds a pad and exits with no cleanup at all,
which leaves zero pads. If you see a stray pad, something is still running:

```powershell
Get-Process receiver; Get-NetUDPEndpoint -LocalPort 47800
```

### A game sees an idle "player 2"

Both virtual pads are allocated at startup by design. With one controller
connected, the second pad exists and is idle. Its sticks rest inside the
deadzone so axes are ignored, but "press A to join" screens may still see it.

---

## Building

### CMake builds with mingw instead of MSVC

If a mingw toolchain is on `PATH` — easy to acquire accidentally, since some
`cmake` packages ship alongside `gcc` and `ninja` — a bare `cmake -B build`
selects Ninja + GCC rather than MSVC. Two symptoms follow:

1. The stated compiler is wrong.
2. Ninja is single-config, so `--config Release` is ignored and the binary lands
   at `build\receiver.exe` instead of `build\Release\receiver.exe`, which then
   looks like a build that "succeeded but produced nothing".

`setup-pc.ps1` now passes an explicit generator. To do it by hand:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

A build directory configured by one generator cannot be reused by another —
delete it when switching.

### MSVC garbles characters in comments or strings

Source files here contain non-ASCII characters. MSVC decodes a file without a
BOM using the legacy codepage, which corrupts them. Either keep the UTF-8 BOM
(`receiver-win/main.cpp` and `protocol/protocol.h` carry one deliberately) or
compile with `/utf-8`, which is what `sender-desktop` and `tests` do.

The same class of bug once broke `setup-pc.ps1` with a baffling
`Missing closing '}' in statement block`: an em-dash decoded in the legacy
codepage produced a byte PowerShell treated as a string delimiter, so a string
ended mid-sentence and brace counting collapsed. **Keep `.ps1` files here pure
ASCII with a UTF-8 BOM.**

### `MAX_PATH` errors while building

Windows caps paths at 260 characters. Building from a deeply nested directory
produces confusing `The system cannot find the path specified` errors before the
compiler even starts. Build from a shorter path.

### Android: `javac` rejects `-bootclasspath`

JDK 21 only allows `-bootclasspath` with `-target 8`, and refuses to combine it
with `--release` at all. `build-apk.cmd` uses `-source 8 -target 8`
deliberately; the sources use no Java 9+ constructs and `d8` handles the rest.

### Android: signing fails with "entry does not contain a key"

The keystore alias in `build-apk.cmd` no longer matches the local
`debug.keystore`. Delete `debug.keystore` and rebuild — it is a throwaway debug
key and is regenerated automatically.

---

## Android sender

### Input freezes when you leave the app

Android delivers controller events only to the **focused window**. Opening the
notification shade or switching apps hands focus elsewhere and input stops,
while packets keep flowing with the last known values — so the PC still shows
"active" and nothing looks broken.

Enable the accessibility service (**Enable background capture** in the app). It
receives input ahead of window dispatch, and works with the screen off.

### It stopped working after an update

**Android disables an accessibility service every time its APK is
reinstalled.** Re-enable it after each install. The app's headline turns amber
when it is off.

### Android refuses to enable the service ("restricted setting")

Android's sideloading guard. Settings → Apps → the app → **⋮** →
*Allow restricted settings*, then enable it.

### Triggers and buttons read zero while sticks work

Caused by a focused text field consuming gamepad events before the Activity saw
them. Capture at `dispatchKeyEvent`/`dispatchGenericMotionEvent`, which run
ahead of the view hierarchy, rather than `onKeyDown`/`onGenericMotionEvent`.

### Sticks or triggers mapped to the wrong axis

Pads disagree about axis assignments. A DualSense reports the right stick on
`RX`/`RY` with L2/R2 on `Z`/`RZ`; many other pads are the reverse. The app
resolves this per device from the reported ranges — a stick swings negative
(`[-1,1]`), a trigger rests at zero (`[0,1]`) — and shows the result as
`axes RX=… RY=… LT=… RT=…` so a mismapping is visible rather than mysterious.

### The foreground service crashes on start

Android 14+ rejects a `connectedDevice` foreground service unless the app also
holds one of `BLUETOOTH_CONNECT` / `CHANGE_WIFI_STATE` / `NFC` / … The
`SecurityException` helpfully names the whole list. This app needs none of them,
so it declares `specialUse` with the required justification property instead.

### Swiping the app away does not stop it

By design: the service is `START_STICKY` so it survives memory pressure
mid-game, and the accessibility service can re-trigger it. Use **Stop** on the
notification, or **Stop streaming** in the app. The notification cannot be
swiped away (`ONGOING`/`NO_CLEAR`), which is why the Stop action exists.

---

## Latency

### The first reading is much worse than the real figure

An idle Wi-Fi radio inflates it badly. A measurement taken before any streaming
started read 7.5 ms one-way; the same link read **2.0 ms** once 120 Hz traffic
kept the radio awake. Measure while actually streaming.

### It misses the budget

Diagnose the hop before changing code. Wi-Fi jitter is the usual culprit, and a
Bluetooth controller adds its own latency ahead of anything this software does.
Check whether either end is wireless before suspecting the program — "the PC is
on Ethernet" does not mean the path is wired if the sender is on Wi-Fi.
