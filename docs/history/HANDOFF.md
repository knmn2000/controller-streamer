# HANDOFF — Controller Streamer, PC-side bring-up

You are picking up a project mid-flight. Read this whole file before running
anything. Author/owner: the project maintainer. Do not add co-authors to any commit.

## 1. Mission

Two PS5 DualSense pads connected to a MacBook stream inputs over the LAN to
this Windows PC, where they appear as two virtual Xbox 360 controllers that any
game can use. Rumble from PC games travels back to the physical pads. Target
added latency: <= 10 ms on the local network.

**Your scope in this session: get the PC (receiver) side fully working and
verified.** The Mac side is a separate machine you cannot reach from here. Your
finish line is "the receiver builds, runs, presents two virtual pads, and
correctly consumes real packets."

## 2. Environment

- This session runs on the **Windows PC**. Windows 10/11.
- The MacBook is on the same LAN. the maintainer operates it manually; when you need
  something done there, ask him and give him the exact command to paste.
- MSVC is the intended compiler. Prefer a **Developer PowerShell for VS 2022**
  for build commands. Some steps need an **elevated** shell (driver install,
  firewall rule) — say so explicitly rather than failing silently.

## 3. What already exists

The project is written and partially verified. Layout:

```
controller-streamer/
  protocol/protocol.h        shared wire protocol v3 - SINGLE SOURCE OF TRUTH
  sender-mac/main.cpp        macOS app (SDL2 + POSIX UDP) - not your job here
  sender-mac/CMakeLists.txt
  receiver-win/main.cpp      THIS machine's app (Winsock2 + ViGEmClient)
  receiver-win/CMakeLists.txt
  receiver-win/setup-pc.ps1  one-shot setup script (see section 5)
  tests/protocol_test.cpp    protocol unit tests, no hardware needed
  tests/loopback_receiver.cpp test harness that plays the PC role on localhost
  README.md                  build + run instructions
```

There is also a design document, `controller-streaming-study-trd-v3.md`
(ask the maintainer if you need it). Code comments reference its sections by ID
(`C2`, `C5`, `C8`, `5.5`, `7.3`, ...). Those IDs are load-bearing context for
*why* code looks the way it does — read the referenced section before
"simplifying" anything that cites one.

### Verification status (important — do not re-verify what's done, do not trust what isn't)

Already proven, in a Linux container:

- `tests/protocol_test.cpp` passes: struct sizes, big-endian wire bytes,
  `INT16_MIN` axis-inversion edge case, sequence wraparound, all mapping helpers.
- Sender compiles and links against real SDL2; loopback integration test showed
  1 Hz heartbeats, 10 Hz latency probes echoed, clean Ctrl-C shutdown, zero
  malformed packets.
- `receiver-win/main.cpp` **compile-checked only**, via mingw-w64 against the
  real ViGEmClient headers, zero warnings. It has **never been compiled with
  MSVC and never executed.**
- Protocol button bitmask verified identical to ViGEm's real `XUSB_BUTTON` enum
  (all 15 values).

Never exercised anywhere, i.e. your actual risk surface:

- MSVC build of the receiver (only mingw has ever parsed it).
- ViGEm runtime behavior: driver connect, target add, `joy.cpl` visibility,
  the rumble notification callback firing.
- Real UDP between the two machines, including Windows Firewall.
- Any latency measurement on real hardware.

## 4. Working style for this task

The maintainer's standing preferences, which apply here:

- **Think before coding.** State assumptions; if two readings exist, ask rather
  than picking silently. If something is unclear, stop and name it.
- **Simplicity first.** Minimum code that solves the problem. No speculative
  features, no abstractions for single-use code, no error handling for
  impossible cases.
- **Surgical changes.** Touch only what the task requires. Don't improve
  adjacent code, don't refactor what isn't broken, match existing style. If you
  spot unrelated dead code, mention it — don't delete it.
- **Goal-driven.** Every step gets a verification check (they're written out in
  section 5). Loop until the check passes; don't declare done on vibes.
- Concrete sequential instructions when you hand work back to the maintainer.
- He is strong in JavaScript and new to C++. When a C++-specific mechanism
  matters (struct packing, byte order, thread synchronization, RAII), explain
  the fundamental briefly rather than assuming it's known.

## 5. Task sequence

Do these in order. Each has a hard verification gate — don't advance past a
failing gate.

### T1 — Locate the project and confirm integrity
The maintainer extracted the zip somewhere under a Downloads folder (they
previously ran from `Downloads\files`, with the files possibly loose rather
than in their folders). Find the tree, confirm the layout in section 3 —
specifically that `receiver-win\` and `protocol\` are **sibling** directories,
because CMake resolves `../protocol` relative to the receiver folder.
**Verify:** `receiver-win\CMakeLists.txt`, `receiver-win\main.cpp`,
`receiver-win\setup-pc.ps1`, and `protocol\protocol.h` all exist in that
relationship. If the files are loose, re-extract the zip properly instead of
moving files around by hand.

### T2 — Run the setup script
```powershell
powershell -ExecutionPolicy Bypass -File .\setup-pc.ps1
```
from inside `receiver-win\`, in an **elevated** shell. It prints `[1/6]`..`[6/6]`
with a green OK per step and does: admin check, toolchain check (git, cmake,
MSVC), ViGEmBus driver install if missing, firewall rule for UDP 47800, build,
then prints this PC's LAN IP.
**Verify:** all six steps OK. A red `!!` stops the script with a reason; act on
that reason rather than working around the script.

Known history worth knowing: an earlier version of this script failed with
`Missing closing '}' in statement block` at line 47. Cause was not the braces —
the file contained em-dash characters, Windows PowerShell 5.1 decoded UTF-8 in
the legacy codepage, and one resulting byte was a curly quote that PowerShell
honored as a string delimiter, so a string ended mid-sentence and brace
counting collapsed. The shipped script is now pure ASCII with a UTF-8 BOM.
**If you ever edit a `.ps1` here, keep it ASCII and preserve the BOM** or you
will resurrect that bug. Same reasoning applies to `.cpp`/`.h` files compiled
by MSVC: they carry a BOM deliberately.

### T3 — Independent verification of setup state
Don't take the script's word for it:
```powershell
Get-Service ViGEmBus
Get-NetFirewallRule -DisplayName "Controller Streamer" | Select DisplayName, Enabled
Test-Path .\build\Release\receiver.exe
```
**Verify:** service exists (Running or Stopped are both fine — it starts on
demand), rule `Enabled: True`, path `True`.

If T2's build failed and you're fixing it manually, the build is
`cmake -B build` then `cmake --build build --config Release`. CMake fetches the
ViGEmClient SDK at configure time (pinned tag `v1.21.222.0`) — that needs
internet and git. If the fetch fails, the documented fallback is to clone
https://github.com/nefarius/ViGEmClient into `receiver-win\` and swap the
`FetchContent_*` lines for `add_subdirectory(ViGEmClient)`.

### T4 — Prove the virtual pads exist
Run `.\build\Release\receiver.exe`. Expected output:
`2 virtual X360 pads up, listening on UDP 47800`.
**Verify:** Win+R -> `joy.cpl` shows two "Xbox 360 Controller" entries while it
runs, and they disappear on Ctrl-C. This is the first real test of the whole
ViGEm path (alloc -> connect -> target_add -> register_notification) and the
teardown order.

### T5 — Prove the network path, without the Mac app
Have the maintainer run, on the Mac: `nc -u <pc_ip> 47800` and type text.
On this PC, with the receiver **stopped**, listen with `ncat -u -l 47800`
(ncat ships with Nmap) so you're testing routing and firewall, not the app.
**Verify:** typed text arrives. If not, the problem is firewall/IP/interface —
fix it here before touching code. Note that the PC may have several IPv4
addresses (Wi-Fi, Ethernet, Hyper-V/WSL virtual adapters); make sure the maintainer uses
the one on the same subnet as the MacBook.

### T6 — End to end
Receiver running here; the maintainer runs on the Mac: `./build/sender <pc_ip>`.
**Verify, in this order:**
1. Receiver prints `[net] sender is alive` within ~1 second (that's the 1 Hz
   heartbeat, and it also teaches the receiver where to send rumble).
2. With a pad connected on the Mac, https://hardwaretester.com/gamepad or
   `joy.cpl` mirrors physical movement. Check axis **orientation and sign**
   deliberately: stick up must read up, right must read right, and sticks must
   rest near zero. A wrong sign here means the Y-inversion or byte-order path
   is broken — that's what this gate is for.
3. Triggers travel 0..255 and rest at 0.
4. Kill the Mac sender mid-input: receiver logs
   `[watchdog] controller N stale -> neutral` within ~500 ms and the virtual pad
   goes neutral (no stuck inputs).
5. Ctrl-C the Mac sender cleanly instead: receiver logs
   `controller N disconnected by sender` (that's the explicit Disconnect
   packet, which is the fast path; the watchdog is the guarantee behind it).

### T7 — Rumble backchannel
Trigger vibration from this PC: the hardwaretester page has a vibration button;
a small `XInputSetState` test program also works.
**Verify:** the correct physical pad buzzes (slot 0 vs slot 1 must not cross),
continuous rumble is smooth rather than stuttering, and killing the receiver
mid-rumble leaves the pad silent within ~600 ms.
Design context you'll need if this misbehaves: the receiver re-sends non-zero
rumble at 10 Hz from the watchdog thread; the Mac applies it with an 800 ms SDL
duration, refreshes every 400 ms, and force-stops if no rumble packet arrives
for 600 ms. That combination is deliberate — it survives a lost `(0,0)` stop
packet (UDP is lossy) without stuttering under sustained rumble. Don't collapse
it to a single `Rumble(...)` call.

### T8 — Numbers
The maintainer runs `./build/sender <pc_ip> --measure` on the Mac. It prints, once per
second, min/avg/max one-way estimate from a 10 Hz timestamp echo (RTT/2 from a
single clock, so no cross-machine clock sync is involved).
**Verify:** report median-ish and worst-case against the 10 ms budget, over
Wi-Fi and, if he can, Ethernet. If the budget is missed, diagnose the hop
before changing code — Wi-Fi jitter is the usual culprit, not the program.

## 6. Failure signature reference

- Buttons/axes scrambled or absurd values -> byte order or struct layout
  mismatch. Both sides must be compiled against the *same* `protocol/protocol.h`.
  Check the `static_assert`s fired at compile time, then look at real bytes in
  Wireshark (`udp.port == 47800`); a valid packet starts `4d 58 03`.
- Stick up reads down -> axis inversion path (`invert_axis`, and note the
  `INT16_MIN` special case is intentional, not a typo).
- Works then freezes for about a second, repeatedly -> sequence-number
  wraparound handling (`seq_newer`). It is RFC 1982 style on purpose; a naive
  `<=` comparison breaks every ~2 s at 120 Hz with a `uint8_t` counter.
- Nothing arrives across the network but localhost works -> firewall, wrong IP,
  or wrong interface (see T5).
- `ViGEm connect failed` -> driver not installed or needs a reboot.
- Receiver won't exit -> a thread stuck in a blocking call. Both receive loops
  use a 250 ms `SO_RCVTIMEO` precisely so they can notice the shutdown flag;
  don't remove those timeouts.
- Rumble stuck on -> the Mac-side 600 ms expiry isn't being fed, or the
  receiver's 10 Hz refresh stopped.

## 7. Out of scope unless the maintainer asks

Radial stick deadzone (deliberately not implemented — games apply their own,
and sender-side rescaling changes feel in every game), auto-discovery of the
PC's IP (it's a CLI argument by design), encryption/auth (trusted LAN only),
DualSense adaptive triggers / touchpad surface / gyro (no XInput equivalent),
and dynamic plug/unplug of the virtual pads (both are allocated statically at
startup — revisit only if a game misbehaves).

## 8. Reporting back

When you finish or get stuck, report: which task IDs passed their gates, the
exact command and output for any failure, and any change you made to the repo
with the reason. If you had to modify `protocol/protocol.h`, say so loudly —
the Mac side must be rebuilt from the identical file or the two ends will
disagree on the wire.
