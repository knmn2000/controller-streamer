# controller-streamer

**Your PC's Bluetooth is broken, so you can't pair your controller to it. Pair
the controller to your phone instead — this streams the input to the PC over
Wi-Fi, where it appears as a normal Xbox 360 controller that any game just
uses.**

That's the problem this solves. A dead, missing or flaky Bluetooth adapter on a
desktop is common, and every usual workaround is irritating: buy a USB dongle,
run a cable across the room, or re-pair the controller every time you switch
machines. None of that is necessary — the controller is *already* paired to
something with working Bluetooth. Use that machine as the radio and send the
button presses over the network you already have.

The sender can be a **phone, a Mac, or a second PC**, and rumble travels back
the other way. Measured added latency is about **2 ms one-way** over Wi-Fi on a
home LAN, against a design budget of 10 ms — comfortably inside the range where
it feels like a wired pad.

```
  ┌──────────────────────┐         UDP 47800          ┌──────────────────────┐
  │  Android / macOS /   │  input, 120 Hz  ────────▶  │   Windows PC         │
  │  Windows / Linux     │                            │                      │
  │                      │  ◀────────  rumble         │  ViGEmBus driver     │
  │  real controller     │             heartbeat      │       ▼              │
  │  (Bluetooth or USB)  │             discovery      │  2 virtual X360 pads │
  └──────────────────────┘                            └──────────────────────┘
```

Two controllers are supported simultaneously; the PC always presents two pads.

## What it is not

It is **not** remote play or screen streaming. Nothing about the game leaves the
PC — the game runs on the PC and you look at the PC's screen. Only controller
state crosses the network, 18 bytes per input packet at 120 Hz, which is exactly
why the latency budget is achievable.

The phone is being used as a Bluetooth radio and nothing more.

---

## Supported combinations

| Sender (has the controller) | Status | Notes |
|---|---|---|
| **Android** phone/tablet, API 24+ | works | Streams with the phone **locked**; needs an accessibility service enabled |
| **macOS** | works | SDL2 |
| **Windows** (PC → PC) | works | SDL2; don't run it on the receiving PC — see below |
| **Linux** | should work | Same code path as macOS; untested |

The receiver is Windows-only by nature: it depends on
[ViGEmBus](https://github.com/nefarius/ViGEmBus) to create the virtual pads.

---

## Quick start

### 1. The Windows PC (receiver) — do this first

From an **elevated** PowerShell, inside `receiver-win\`:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-pc.ps1
```

That checks your toolchain, installs the ViGEmBus driver if missing, adds a
firewall rule for UDP 47800, builds the receiver, and prints the PC's LAN IP.
It is safe to re-run; every step checks before acting.

Then run it:

```powershell
.\build\Release\receiver.exe
```

You should see `2 virtual X360 pads up, listening on UDP 47800`, and two
"Xbox 360 Controller" entries in `joy.cpl`.

Optional, once you are happy with it:

```powershell
.\build\Release\receiver.exe --tray          # hide the console, sit in the tray
powershell -ExecutionPolicy Bypass -File .\autostart.ps1 -Install   # start at logon
```

In tray mode the log goes to `%LOCALAPPDATA%\controller-streamer\receiver.log`.

### 2a. Sender — Android

Build the APK (needs the Android SDK; no Gradle, no Android Studio project):

```
cd sender-android
build-apk.cmd
```

Install `dist\controller-streamer-sender.apk`, open it, and it finds the PC by
itself — there is no IP to type. Then tap **Enable background capture** and turn
the service on under Settings → Accessibility.

**That accessibility service is what makes it usable.** Android delivers
controller events only to the focused window, so without it, input freezes the
moment you open the notification shade or lock the phone. With it, streaming
keeps working with the screen off.

Two caveats worth knowing up front:

- Android **disables the accessibility service every time you reinstall the
  APK**. Re-enable it after each update. The app's headline turns amber when
  it is off, so the state is visible rather than silent.
- If Android refuses with a *"restricted setting"* message, that is its
  sideloading guard: Settings → Apps → the app → **⋮** → *Allow restricted
  settings*, then try again.

### 2b. Sender — macOS, Windows or Linux

```bash
brew install sdl2 cmake          # macOS
sudo apt install libsdl2-dev     # Debian/Ubuntu
```

```bash
cd sender-desktop
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sender <pc_ip>
```

On Windows, SDL2 has no standard location, so either install it via vcpkg, pass
`-DSDL2_DIR=<dir with sdl2-config.cmake>`, or let CMake fetch a prebuilt copy:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DFETCH_SDL2=ON
cmake --build build --config Release
```

`SDL2.dll` must sit next to `sender.exe` at runtime.

> **PC → PC:** run the sender on the machine with the controller, never on the
> receiving PC. The receiver's own virtual pads are visible to SDL, so a sender
> on the same machine will pick them up and stream them back to itself.

### 3. Measure it

```bash
./build/sender <pc_ip> --measure
```

Prints min/avg/max one-way estimates every second, derived from a 10 Hz
timestamp echo. It is RTT/2 measured against a single clock, so no cross-machine
clock sync is involved.

---

## How it works

**Wire protocol v3** lives in [`protocol/protocol.h`](protocol/protocol.h) and is
the single source of truth, compiled into every C++ end and hand-ported (with a
matching test) for Android. Packets are big-endian and `#pragma pack(1)`, with
`static_assert`s on every struct size so layout drift fails at compile time
rather than in a packet capture at midnight.

A valid packet starts `4d 58 03` — `"MX"` plus the version.

| Packet | Size | Direction |
|---|---|---|
| Input | 18 B | sender → PC, 120 Hz |
| Rumble | 8 B | PC → sender, on change + 10 Hz refresh |
| Heartbeat | 6 B | sender → PC, 1 Hz |
| Disconnect | 6 B | sender → PC, ×3 on pad removal |
| Latency probe | 14 B | echoed verbatim by the PC |
| Discover / reply | 6 B / 38 B | broadcast; reply carries the PC's hostname |

A few decisions that are load-bearing rather than incidental:

- **Button bits equal ViGEm's `XUSB_BUTTON` values**, so the receiver copies the
  mask straight into `XUSB_REPORT.wButtons` with no translation table.
- **Sequence numbers are RFC 1982 style.** A naive `<=` comparison breaks every
  ~2 s at 120 Hz with a `uint8_t` counter.
- **Axis inversion special-cases `INT16_MIN`**, because `-(-32768)` does not fit
  in an `int16_t` and signed overflow is undefined behaviour.
- **No stick deadzone is applied anywhere.** Games apply their own, and
  rescaling at the sender would change the feel in every game.
- **Discovery reuses the latency echo.** The receiver echoes `PT_LATENCY`
  verbatim, so a sender that broadcasts one learns the PC's address from the
  reply's source — which is why discovery also works against receivers built
  before `PT_DISCOVER` existed.

There is a longer design document in [`docs/design-trd-v3.md`](docs/design-trd-v3.md);
code comments reference its section IDs (`C2`, `C5`, `C8`, `7.3` …).

## Repository layout

```
protocol/protocol.h        wire protocol v3 - single source of truth
receiver-win/              Windows receiver (Winsock2 + ViGEmClient)
  setup-pc.ps1             one-shot setup: driver, firewall, build
  autostart.ps1            run at logon in tray mode
sender-desktop/            macOS / Windows / Linux sender (SDL2)
sender-android/            Android sender, built without Gradle
tests/                     protocol tests and diagnostics (see below)
docs/                      design document and project history
```

## Tests and diagnostics

```bash
cd tests
cmake -B build && cmake --build build
./build/protocol_test          # or build\Release\protocol_test.exe
```

`protocol_test` needs no hardware and no network, and is the one test that must
pass everywhere. The Android port has an equivalent that checks the *same* wire
vectors, since Java has no `static_assert` to protect it:

```bash
java -cp sender-android/out/classes org.controllerstreamer.sender.Protocol
```

Windows-only diagnostics, which exist because the failure modes here are hard to
see from the outside:

| Tool | What it is for |
|---|---|
| `xinput-probe list` | what the virtual pads currently report |
| `xinput-probe capture <slot> <secs>` | per-axis extremes — proves stick orientation and trigger range |
| `xinput-probe rumble <slot> <lg> <sm> <ms>` | drive one pad's motors, to check slots don't cross |
| `fake-sender` | drive the receiver from the same PC, with no phone or Mac involved |
| `vigem-diag` | the exact `VIGEM_ERROR` code from connect/target_add |

`xinput-probe capture` is the honest way to check axis signs: hold a direction
during the window and read the extreme, rather than trusting a live readout.

Note that XInput's `dwPacketNumber` increments when the reported **state
changes**, not once per packet received — a physically still stick freezes it.
It is not a liveness signal.

## Known limitations

- **Two controllers maximum**, matched to the two virtual pads. A third is
  declined with a message.
- **Both virtual pads always exist**, so with one controller connected a game
  may see an idle "player 2". Its sticks rest inside XInput's deadzone, so games
  ignore the axes, but join prompts may notice it.
- **The Android app must be open or its accessibility service enabled.** There
  is no way to read controller input in the background otherwise.
- **DualSense adaptive triggers and its voice-coil haptics are not reachable.**
  Not an Android limitation: PC games drive rumble through XInput, which carries
  exactly two 0–255 motor values, so that is the maximum fidelity available on
  this path.
- **No encryption or authentication.** Anyone on your LAN can send packets to
  the receiver. It is built for a trusted home network.
- Virtual pads are created at startup, not on demand.

## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — it covers the failure modes that
actually came up, and the specific diagnostic that identified each one.

## Licence

GPL-3.0. See [LICENSE](LICENSE).

Uses [ViGEmBus / ViGEmClient](https://github.com/nefarius/ViGEmBus) by Nefarius
Software Solutions for the virtual controllers, and [SDL2](https://www.libsdl.org/)
for controller input on desktop.
