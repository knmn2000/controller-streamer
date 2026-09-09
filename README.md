# controller-streamer

My PC's Bluetooth was dead, so I couldn't pair a controller to it. This pairs the
controller to my **phone** instead and streams the input to the PC over Wi-Fi,
where it shows up as a plain **Xbox 360 controller**. Games need no
configuration. Rumble comes back the other way.

Added latency measures **~2 ms one-way** over Wi-Fi (budget was 10 ms).

```
   phone / Mac / PC                                    Windows PC
 ┌──────────────────────┐         UDP 47800        ┌──────────────────────┐
 │  real controller     │   input 120 Hz  ──────▶  │  receiver.exe        │
 │  (Bluetooth or USB)  │                          │        │             │
 │                      │  ◀──────  rumble         │  ViGEmBus driver     │
 │  reads it with SDL2  │           heartbeat      │        ▼             │
 │  or Android input    │           discovery      │  2 virtual X360 pads │
 └──────────────────────┘                          └──────────────────────┘
```

The game still runs on the PC and you still watch the PC's screen. Only
controller state crosses the network, 18 bytes per packet. Your phone is acting
as the Bluetooth radio and nothing else.

| Sender | State | Notes |
|---|---|---|
| Android, API 24+ | works | keeps streaming with the phone **locked** ([how](#android)) |
| macOS | works | SDL2 |
| Windows (PC to PC) | works | SDL2. Not on the receiving PC ([why](#pc-to-pc)) |
| Linux | untested | same code path as macOS |

Receiver is Windows-only: it needs [ViGEmBus](https://github.com/nefarius/ViGEmBus)
to create the pads. Two controllers max.

---

## 1. Windows PC

Elevated PowerShell, in `receiver-win\`:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-pc.ps1
```

Installs the ViGEmBus driver, adds a firewall rule for UDP 47800, builds, prints
your LAN IP. Safe to re-run. Then:

```powershell
.\build\Release\receiver.exe
```

Expect `2 virtual X360 pads up, listening on UDP 47800` and two "Xbox 360
Controller" entries in `joy.cpl`.

Optional:

```powershell
.\build\Release\receiver.exe --tray                                 # tray icon, no console
powershell -ExecutionPolicy Bypass -File .\autostart.ps1 -Install   # start at logon
```

Tray mode logs to `%LOCALAPPDATA%\controller-streamer\receiver.log`.

## 2. Sender

### Android

Install [`release/controller-streamer-sender.apk`](release/controller-streamer-sender.apk)
(29 KB, checksum and signature notes in [`release/`](release/README.md)), or build it:

```
cd sender-android
build-apk.cmd
```

No Gradle, no Android Studio project. Needs the Android SDK.

Open the app. It finds the PC on its own, so there is no IP to type. Then tap
**Enable background capture** and switch it on under Settings → Accessibility.

Android only delivers controller events to the focused window. Without that
service, input freezes the moment you pull down the notification shade. With it,
streaming survives a locked screen. Two things that will trip you up:
[reinstalling the APK disables it](TROUBLESHOOTING.md#it-stopped-working-after-an-update),
and Android may [block enabling it as a "restricted setting"](TROUBLESHOOTING.md#android-refuses-to-enable-the-service-restricted-setting).

### macOS, Windows, Linux

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

Windows has no standard SDL2 location, so pick one: vcpkg,
`-DSDL2_DIR=<dir with sdl2-config.cmake>`, or let CMake fetch a prebuilt copy.

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DFETCH_SDL2=ON
cmake --build build --config Release
```

`SDL2.dll` has to sit next to `sender.exe`.

<a name="pc-to-pc"></a>
> **PC to PC:** run the sender on the machine holding the controller, never on
> the receiving PC. SDL sees the receiver's own virtual pads and will stream them
> straight back to itself.

## 3. Measure

```bash
./build/sender <pc_ip> --measure
```

Min/avg/max one-way, once a second, from a 10 Hz timestamp echo. RTT/2 off a
single clock, so no cross-machine clock sync.

---

## Protocol

[`protocol/protocol.h`](protocol/protocol.h) is the single source of truth,
compiled into every C++ end and hand-ported to Java with a
[test on the same wire vectors](sender-android/src/org/controllerstreamer/sender/Protocol.java).
Big-endian, `#pragma pack(1)`, a `static_assert` on every struct size. Valid
packets start `4d 58 03` (`"MX"` + version).

| Packet | Size | Direction |
|---|---|---|
| Input | 18 B | sender → PC, 120 Hz |
| Rumble | 8 B | PC → sender, on change + 10 Hz refresh |
| Heartbeat | 6 B | sender → PC, 1 Hz |
| Disconnect | 6 B | sender → PC, ×3 on removal |
| Latency probe | 14 B | echoed verbatim by the PC |
| Discover / reply | 6 / 38 B | broadcast; reply carries the PC's hostname |

Four decisions you should not "simplify" without reading why:

- **Button bits equal ViGEm's `XUSB_BUTTON` values.** The receiver copies the
  mask into `XUSB_REPORT.wButtons` with no translation table.
- **Sequence numbers use RFC 1982 comparison.** A naive `<=` breaks every ~2 s at
  120 Hz with a `uint8_t` counter.
- **Axis inversion special-cases `INT16_MIN`.** `-(-32768)` overflows an
  `int16_t`, which is undefined behaviour.
- **No stick deadzone anywhere.** Games apply their own; rescaling here changes
  the feel in every game.

Longer rationale in [`docs/design-trd-v3.md`](docs/design-trd-v3.md), which the
code comments reference by section (`C2`, `C5`, `C8`, `7.3`).

## Layout

```
protocol/protocol.h    wire protocol v3, shared by every end
receiver-win/          Windows receiver (Winsock2 + ViGEmClient)
  setup-pc.ps1         driver, firewall, build
  autostart.ps1        run at logon in tray mode
sender-desktop/        macOS / Windows / Linux sender (SDL2)
sender-android/        Android sender, no Gradle
tests/                 protocol tests + Windows diagnostics
docs/                  design document, project history
release/               prebuilt APK
```

## Tests

```bash
cd tests && cmake -B build && cmake --build build
./build/protocol_test                    # build\Release\protocol_test.exe on Windows
```

`protocol_test` needs no hardware and no network. The Java port has an
equivalent:

```bash
java -cp sender-android/out/classes org.controllerstreamer.sender.Protocol
```

Windows diagnostics in [`tests/`](tests/):

| Tool | Use |
|---|---|
| `xinput-probe list` | what the virtual pads report right now |
| `xinput-probe capture <slot> <secs>` | per-axis extremes. Hold a direction, read the extreme: this is how you check stick signs |
| `xinput-probe rumble <slot> <lg> <sm> <ms>` | drive one pad's motors, to check slots don't cross |
| `fake-sender` | drive the receiver from the same PC, no phone or Mac needed |
| `vigem-diag` | the exact `VIGEM_ERROR` from connect/target_add |

## Limits

| | |
|---|---|
| Controllers | 2, matched to the 2 virtual pads. A third is declined |
| Idle pad | both pads always exist, so a game may see an idle "player 2". Sticks rest inside XInput's deadzone, but join prompts can still notice |
| Android background | needs the app open or the accessibility service on. No other way to read controller input |
| DualSense haptics | adaptive triggers and voice-coil haptics are unreachable. XInput carries two 0-255 motor values, so that is the ceiling, not an Android limit |
| Security | none. Anyone on your LAN can send packets. Built for a home network |

## Troubleshooting

[TROUBLESHOOTING.md](TROUBLESHOOTING.md) lists the failures that actually
happened and the diagnostic that found each one, including a sender that looks
alive but sends nothing, CMake picking mingw over MSVC, and pads resting at
`LX=-3356`.

## Licence

GPL-3.0, see [LICENSE](LICENSE). Built on
[ViGEmBus](https://github.com/nefarius/ViGEmBus) by Nefarius Software Solutions
and [SDL2](https://www.libsdl.org/).
