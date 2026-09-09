# Controller Streaming Study TRD v3.0 (Mac → PC)

**Project:** Low-latency cross-platform controller streamer
**Supersedes:** TRD v2.0 (2026-08-29)
**Audience:** One engineer, strong in JavaScript, new to C++, building this by hand.

This document is a *guide*, not a solution. Each section explains what must exist, why it must exist, and what you need to understand before you can build it. Where v2.0 handed you code, v3.0 hands you the reasoning and the questions — plus milestones with hard verification criteria so you always know whether the thing you just built actually works.

How to use it:

1. Read sections 1–5 once, end to end, before writing any code.
2. Then work through the milestones in section 8 in order. Each has a "Before you code" question set — if you can't answer those from the fundamentals in section 3 and the linked docs, go read first.
3. Keep section 9 (debugging toolbox) open the whole time.

---

## 1. Requirements

### 1.1 Functional

- FR1: Up to 2 physical game controllers connected to a MacBook (USB or Bluetooth).
- FR2: Their inputs (buttons, sticks, triggers) appear on a Windows PC as up to 2 virtual Xbox 360 controllers that any XInput game can use.
- FR3: Rumble commands issued by games on the PC are played back on the corresponding physical controller on the Mac.
- FR4: A controller unplugged on the Mac must not leave a "stuck" virtual controller on the PC (inputs must neutralize, ideally the virtual pad is removed).
- FR5: Either machine can restart independently; the link recovers without manual re-pairing beyond re-launching the app.

### 1.2 Non-functional

- NFR1 (latency): Added end-to-end latency budget ≤ 10 ms on an idle home LAN (Wi-Fi will dominate; wired is better). You must be able to *measure* this, not guess it (Milestone 9).
- NFR2 (rate): Input sampled and transmitted at 120 Hz per controller (~8.3 ms period). Bandwidth check: 20-byte payload + ~46 bytes UDP/IP/Ethernet overhead ≈ 66 bytes × 120/s × 2 controllers ≈ 16 KB/s. Trivial. Latency, not bandwidth, is the constraint.
- NFR3 (loss tolerance): Occasional packet loss must degrade gracefully (a lost input frame is superseded ~8 ms later by the next one). No retransmission.
- NFR4 (scope/security): Trusted home LAN only. No encryption or authentication. Consequence to accept knowingly: anyone on your LAN who knows the port can inject inputs. The `magic` field detects accidents, not attackers.

### 1.3 Constraints

- Sender: macOS, C++17, SDL2 (via Homebrew), POSIX sockets.
- Receiver: Windows 10/11, C++17, ViGEmBus driver + ViGEmClient SDK (via vcpkg), Winsock2.
- Build system: CMake on both platforms.
- Note: the ViGEm project was declared end-of-life by its author in 2023. The driver and SDK still work and remain the de facto standard for virtual X360 pads, but pin the versions you install and keep the installers. Source: https://docs.nefarius.at/projects/ViGEm/End-of-Life/

---

## 2. What you are actually building

```
            MacBook (Sender)                         Windows PC (Receiver)
 ┌────────────────────────────────┐          ┌─────────────────────────────────┐
 │  Physical pads ──► SDL2        │          │        ViGEmBus driver          │
 │        │                       │          │             ▲    │              │
 │        ▼                       │          │   update()  │    │ rumble       │
 │  Input thread (120 Hz loop)    │  UDP     │             │    ▼ callback     │
 │   poll → map → pack → sendto ──┼─────────►│  Net recv thread   Rumble path  │
 │                                │ InputPkt │   recvfrom → parse │             │
 │  Rumble thread                 │          │   → seq filter     │             │
 │   recvfrom ◄───────────────────┼──────────┼── sendto(RumblePkt)             │
 │   → SDL_GameControllerRumble   │  UDP     │                                 │
 │                                │          │  Watchdog thread (10 Hz)        │
 └────────────────────────────────┘          └─────────────────────────────────┘
```

Two independent one-way data flows share the story: a high-rate input stream (Mac→PC) and a low-rate, event-driven rumble stream (PC→Mac). Everything else — threads, packets, watchdogs — exists to serve these two flows without one blocking the other.

---

## 3. Critique of TRD v2.0 (what changed and why)

Work through this list even if you skim everything else; several items are latent bugs that would have cost you days.

**C1. Wrong documentation link.** v2.0 cites `github.com/IBM/sdl2-gamecontroller` as "SDL2 GameController API". That is a *Node.js binding* — ironic, given you're leaving JS behind. The canonical SDL2 reference is the libsdl wiki: https://wiki.libsdl.org/SDL2/CategoryGameController . Lesson: verify every link in a generated document before trusting it.

**C2. Integer overflow in Y-axis inversion.** `int16_t` holds −32768..32767. Negating −32768 yields +32768, which does not fit — in C++ this is signed overflow, i.e., undefined behavior, not a wraparound you can reason about. v2.0's clamp note is aimed at the wrong end. Before you code: which single input value must you special-case, and to what? (Hint: clamp *before* negating.)

**C3. RumblePacket is never specified.** v2.0 declares packet type `0x02` and then never defines its payload. Section 5 fixes this. Related unspecified decision: SDL rumble intensities are `uint16_t` 0..65535, XInput motor values are `uint8_t` 0..255. Someone must scale. Decide where (this doc: on the Mac, keep the wire format equal to what ViGEm gives you).

**C4. Heartbeat declared, never used.** Type `0x03` exists with no payload, no cadence, no consumer. v3.0 gives it a job: it is how the receiver distinguishes "controller idle" from "sender dead", and how the sender learns the receiver is alive before a controller is even connected.

**C5. Sequence-number filtering breaks every ~2 seconds.** A `uint8_t` counter at 120 Hz wraps every 2.13 s. The naive filter "drop if `seq <= last_seq`" will, right after a wrap, drop up to 128 consecutive valid packets — a visible freeze, twice per second... of confusion. Two fixes; pick one deliberately: (a) widen to `uint16_t`/`uint32_t` so wraps are rare *and still* compare modularly, or (b) keep `uint8_t` and compare with wraparound-aware arithmetic. The classic treatment is RFC 1982 "Serial Number Arithmetic": https://www.rfc-editor.org/rfc/rfc1982 . For an 8-bit counter the test collapses to one signed subtraction — figure out why `(int8_t)(incoming - last) > 0` works before you use it.

**C6. Discovery and addressing are unspecified.** No ports, no way for the Mac to learn the PC's IP, and two sockets where one per machine suffices. v3.0 decision (ADR-2 in section 5): one UDP socket per machine, fixed port on the receiver, receiver replies to whatever address `recvfrom` reported. The sender takes the PC's IP as a CLI argument for v1 — broadcast auto-discovery is a stretch goal, not a requirement.

**C7. Controller identity and hotplug are unhandled.** v2.0 polls two controllers but never says how an SDL joystick instance ID maps to `controller_id` 0/1, never handles `SDL_CONTROLLERDEVICEREMOVED`, and gives the receiver no way to know a pad is gone (the watchdog only neutralizes inputs). v3.0 adds slot assignment rules and a disconnect signal (section 6.3).

**C8. Rumble lifetime semantics missing.** `SDL_GameControllerRumble(pad, lo, hi, duration_ms)` *stops after duration_ms*. ViGEm fires its callback on *change*, including changes to (0,0). If you pass a short duration, rumble stutters; if you pass a huge one and miss the (0,0) packet (UDP!), the pad buzzes forever. Design for both failure modes: refresh strategy + a Mac-side stop condition. Questions in Milestone 7.

**C9. Deadzone double-application.** Games apply their own deadzones on top of yours. A sender-side radial deadzone with rescaling changes stick feel in every game. v3.0: pass sticks through unmodified by default; make the radial deadzone an optional flag. Also, v2.0's formula ("normalize and scale out to 32767") silently makes every off-center input full-tilt — the correct rescale is `(mag − dz) / (32767 − dz)` applied to the normalized direction. Derive it on paper first.

**C10. "Non-blocking socket" without a wait strategy.** A non-blocking `recvfrom` in a `while(true)` loop is a busy-wait that pins a core. v2.0 says non-blocking for the receiver thread but describes blocking behavior. Simpler and correct for this project: *blocking* sockets with a receive timeout (`SO_RCVTIMEO`), so threads can also notice a shutdown flag every few hundred ms. Understand all three options (block, block+timeout, non-block+poll) before choosing — section 3.5.

**C11. No shutdown design.** Threads blocked in `recvfrom` can't be joined until the call returns. Every long-running thread needs an exit path (timeout + atomic flag, or closing the socket from another thread). Decide this before writing the first thread, not after your process refuses to die.

**C12. No latency measurement plan.** "Low-latency" is a claim; NFR1 makes it a number. Milestone 9 defines the echo-timestamp method. Building the measurement early is how you find out whether Wi-Fi, SDL polling, or ViGEm is eating your budget.

**C13. Endianness busywork.** Network byte order via `htons`/`ntohs` is fine and v3.0 keeps it (it's the convention and good practice to learn). But know that this is a *choice*: for a private LAN protocol between two little-endian x86/ARM machines you could declare the wire little-endian and skip conversion. What you may not do is neither — one machine converting and the other not is the classic "my buttons are scrambled" bug. Also: `htons` operates on unsigned values; passing `int16_t` requires a round-trip cast through `uint16_t` (bit pattern is preserved — understand why that's safe: two's complement).

---

## 4. Fundamentals for a JavaScript developer

These are the concepts v2.0's Appendix B gestured at. Each subsection ends with what to read. Don't skip these; every milestone assumes them.

### 4.1 Threads instead of an event loop

Node gives you one call stack plus a hidden C++ thread pool (libuv) that parks your I/O and queues callbacks. In C++ *you are libuv now*. There is no queue and no scheduler in your process except the OS thread scheduler.

- A `std::thread` is an OS thread: its function runs concurrently, truly in parallel on another core.
- A blocking call (`recvfrom`, `sleep_until`) blocks *only its own thread*. That's not a bug — it's the design. One thread per blocking concern is the simplest correct architecture, and it's exactly why the sender has an input thread and a rumble thread.
- A thread must be `.join()`ed (waited for) before its `std::thread` object is destroyed, or your program aborts. This is why shutdown design (C11) matters.
- The 120 Hz loop: `sleep_until(next_tick)` where `next_tick += 8333µs` each iteration. Compare with `setInterval`: why does `sleep_until` with an absolute deadline not drift, while `sleep_for(8333µs)` (and `setInterval`) does? Answer that before Milestone 2.

Read: https://en.cppreference.com/w/cpp/thread/thread and https://en.cppreference.com/w/cpp/chrono (skim; use as reference).

### 4.2 Sharing state between threads (the part Appendix B forgot)

This is the biggest real difference from JS, and v2.0 never mentioned it. In JS, two callbacks can never run at the same instant, so `state.x = 5` is always safe. In C++, if thread A writes a variable while thread B reads it, without synchronization, that is a **data race — undefined behavior**, even for a single `int`. Not "you might read a stale value" — the compiler and CPU are allowed to do anything.

Your minimal toolkit:

- `std::atomic<T>` for small trivially-copyable values (flags, counters, a packed motor-state). Reads/writes are indivisible and visible across threads. Your shutdown flag is `std::atomic<bool>`.
- `std::mutex` + `std::lock_guard` for anything bigger (a struct of controller state). Only one thread inside the locked region at a time.

Where does this bite *this* project? Make the inventory yourself before coding — for each piece of data, ask "which threads touch this?": the shutdown flag (all threads), the receiver's `last_seq` per controller and last-packet-time (net thread writes, watchdog reads), ViGEm target handles (net thread and callback thread), the Mac's SDL controller handles (input thread and rumble thread both call SDL on them — check SDL's thread-safety notes for rumble specifically).

Read: https://en.cppreference.com/w/cpp/atomic/atomic , https://en.cppreference.com/w/cpp/thread/mutex . Optional but excellent background: any writeup of "C++ memory model data race" — the key phrase to search.

### 4.3 Memory, structs, padding, and why `#pragma pack` exists

A JS object is a hash map behind a pointer. A C++ struct is a contiguous slab of bytes whose layout the compiler decides. Compilers insert invisible padding so each field sits at an address divisible by its size (alignment), because misaligned loads are slow or illegal on some CPUs.

Consequence: `struct { uint16_t a; uint8_t b; uint16_t c; }` is probably 6 bytes, not 5. If sender and receiver compilers pad differently — or you assume 5 — every field after the pad is garbage on the wire.

`#pragma pack(push, 1)` … `#pragma pack(pop)` forces 1-byte alignment for wire structs so the layout is exactly the declared fields, portable between MSVC and clang. Then `reinterpret_cast<const char*>(&pkt)` treats the struct's memory as a byte buffer for `sendto` — the C++ equivalent of `Buffer` views, except *you* guarantee the layout.

Verification habit: `static_assert(sizeof(InputPacket) == 20, "layout drifted");` at global scope in the shared header. It costs nothing and catches padding regressions at compile time on *both* platforms. Work out the expected sizes of every packet in section 5 by hand first — that's the exercise.

Read: search cppreference for `static_assert`; skim https://en.cppreference.com/w/cpp/language/object (alignment section) — heavy, skim only.

### 4.4 Endianness

`0x4D58` stored little-endian is bytes `58 4D`; big-endian is `4D 58`. Networks conventionally use big-endian ("network byte order"). `htons`/`ntohs` convert 16-bit values host↔network; on a big-endian host they're no-ops. Apply them to every multi-byte field, on both ends, and never to `uint8_t`. See C13 for the `int16_t` casting subtlety. In Wireshark you can *see* the byte order — Milestone 3 has you confirm `4D 58` on the wire by eye.

Read: Beej's Guide §"Byte Order": https://beej.us/guide/bgnet/html/#byte-order

### 4.5 UDP sockets, POSIX and Winsock

Why UDP and not TCP (mini-ADR): TCP guarantees ordered, reliable delivery — by *retransmitting and stalling*. A retransmitted input frame from 50 ms ago is worse than useless; the freshest frame is the only one that matters. UDP gives you independent datagrams, loss you can ignore (NFR3), and no head-of-line blocking. Every real-time game protocol makes this same call.

The API is nearly identical on both platforms (Winsock was cloned from BSD sockets):

- Both: `socket(AF_INET, SOCK_DGRAM, 0)`, `bind`, `sendto`, `recvfrom`, `sockaddr_in`, `inet_pton`.
- Windows extras: call `WSAStartup` once before any socket call, `WSACleanup` at exit, `closesocket` instead of `close`, errors via `WSAGetLastError()` instead of `errno`, link `Ws2_32.lib`.
- Receive timeout: `setsockopt(SO_RCVTIMEO)` — note the value type differs (struct `timeval` on POSIX, `DWORD` milliseconds on Windows). This is your C10/C11 answer.
- `recvfrom` hands you the peer's address. That single fact is why the receiver never needs to be configured with the Mac's IP: it replies to whoever last sent input (ADR-2, section 5.4).

Read: Beej's Guide (the UDP/datagram sections): https://beej.us/guide/bgnet/ . Winsock deltas: https://learn.microsoft.com/en-us/windows/win32/winsock/getting-started-with-winsock and https://learn.microsoft.com/en-us/windows/win32/winsock/porting-socket-applications-to-winsock

### 4.6 Blocking, blocking-with-timeout, non-blocking

Three ways a thread can wait for a packet:

1. **Blocking:** `recvfrom` sleeps until data arrives. Zero CPU, but the thread is unwakeable — bad for shutdown.
2. **Blocking + `SO_RCVTIMEO`:** same, but returns a timeout error after N ms. The loop body checks the shutdown flag each timeout. Simple, correct, ~zero CPU. **This project's choice.**
3. **Non-blocking:** `recvfrom` returns immediately with `EWOULDBLOCK` if empty. Only sane when paired with `poll`/`select` to sleep until readiness — which is how event loops (libuv!) are built. Overkill for one socket per thread.

Before you code: what does `recvfrom` *return* in each of the three cases (data / timeout / real error), on each platform? Write the answer as a comment above your receive loop.

### 4.7 C callbacks, function pointers, and `void* context`

ViGEm's rumble notification takes a plain C function pointer. C function pointers carry no environment — there is no closure to capture your receiver object. The C convention: registration accepts an extra `void* user_data` which the library passes back into every callback. You pass `this` (cast to `void*`) at registration and cast it back to your class type inside the callback. That's the whole pattern; it's what every JS engine does under the hood to implement closures over C APIs.

Two extra rules for *this* callback specifically: it runs on a ViGEm-managed thread (thread-safety rules from 4.2 apply to anything it touches), and it should do almost nothing — package the motor values, `sendto`, return. Never block in a driver callback.

Read: the callback signature in ViGEmClient's header (`ViGEm/Client.h`, search `EVT_VIGEM_X360_NOTIFICATION`): https://github.com/nefarius/ViGEmClient

### 4.8 RAII: destructors are your `finally`

In JS, GC frees memory and you `try/finally` (or `using`) the rest. In C++, the pattern is RAII: acquire a resource in a constructor, release it in the destructor, and the destructor runs *deterministically* when the object leaves scope — even via early return. `std::lock_guard` (unlocks a mutex), `std::thread` in a wrapper that joins, a small struct whose destructor calls `SDL_GameControllerClose` or `vigem_free`. You don't need to master move semantics for this project; you do need every `*_alloc`/`*_open` to have exactly one owner responsible for the matching `*_free`/`*_close`. Make a table: resource → owner → where it's released.

### 4.9 Build system: CMake + Homebrew + vcpkg

`CMakeLists.txt` ≈ `package.json` (metadata + scripts) but it *generates build files*; it does not fetch dependencies. Fetching is the package manager's job: Homebrew on macOS (`brew install sdl2 cmake`), vcpkg on Windows (`vcpkg install`, then point CMake at the vcpkg toolchain file). In CMake you'll use `find_package(...)` + `target_link_libraries(...)`; the mental model is "tell the compiler where headers are, tell the linker which compiled libraries to staple in" — two separate steps, and each fails with a distinct error class (`file not found: SDL.h` vs `undefined symbol / unresolved external`). Learn to recognize which of the two you're looking at; it halves your build-debugging time.

Read: CMake tutorial steps 1–2 only: https://cmake.org/cmake/help/latest/guide/tutorial/ ; vcpkg + CMake: https://learn.microsoft.com/en-us/vcpkg/get_started/get-started ; SDL2 CMake note: https://wiki.libsdl.org/SDL2/README/cmake

### 4.10 SDL2 vs SDL3, one deliberate choice

SDL3 shipped in 2025 and renamed the whole subsystem (`SDL_GameController*` → `SDL_Gamepad*`, different event names). Most tutorials, Stack Overflow answers, and this TRD's function names are SDL2. Recommendation: build on SDL2 (still maintained), and keep the migration table bookmarked in case you ever port: https://wiki.libsdl.org/SDL3/README/migration . Whatever you pick, pick once — mixed docs are a time sink.

---

## 5. Wire Protocol Specification v3

One shared header file (`protocol.h`), compiled into both binaries, is the single source of truth. If the two sides ever disagree about layout, you lose hours — hence the version byte and the `static_assert`s.

### 5.1 Conventions

- All multi-byte fields in network byte order (big-endian) on the wire (see C13/4.4).
- All structs inside `#pragma pack(push, 1)` / `#pragma pack(pop)`.
- One UDP datagram = exactly one packet. No batching, no fragmentation concerns (largest packet is 20 bytes; any MTU is fine).
- `PROTOCOL_VERSION = 0x03`. Receiver drops packets whose version ≠ its own (and should log once, not per-packet).

### 5.2 Header (6 bytes)

| Field | Type | Notes |
|---|---|---|
| magic | uint16_t | `0x4D58` ("MX"). Drop silently if wrong. |
| version | uint8_t | `0x03` |
| packet_type | uint8_t | `0x01` Input, `0x02` Rumble, `0x03` Heartbeat, `0x04` Disconnect |
| controller_id | uint8_t | 0 or 1. For Heartbeat: `0xFF` (not controller-specific). |
| sequence_num | uint8_t | Per-controller, per-direction wrapping counter. See 5.5. |

### 5.3 Payloads

**Input (`0x01`), 14 bytes — total 20.** As v2.0: `uint16_t buttons` bitmask; four `int16_t` stick axes; two `uint8_t` triggers. You must define the button bitmask yourself: make each bit position equal the corresponding `XUSB_BUTTON` value from ViGEm's `XUSB_REPORT`, so the receiver's mapping is a straight copy instead of 14 `if`s. Find that enum in the ViGEm headers and check: which SDL_CONTROLLER_BUTTON has no XInput equivalent, and what will you do with it? (Look at what the guide button maps to.)

**Rumble (`0x02`), 2 bytes — total 8.** `uint8_t large_motor; uint8_t small_motor;` exactly as ViGEm's notification delivers them (0..255). The Mac scales up to SDL's 0..65535. Decide and document the scaling function — `x * 257` maps 0→0 and 255→65535 exactly; `x << 8` maps 255→65280. Does the difference matter? Compute both, then decide.

**Heartbeat (`0x03`), 0 bytes — total 6.** Sent by the sender at 1 Hz whenever the app runs (even with zero controllers). Purpose: (a) receiver-side liveness ("sender process alive" vs "controller idle"), (b) keeps the receiver's notion of the sender's address fresh for the rumble backchannel.

**Disconnect (`0x04`), 0 bytes — total 6.** Sent (a few times — it's UDP) when the Mac sees `SDL_CONTROLLERDEVICEREMOVED` for that slot. Receiver removes/neutralizes the virtual pad immediately instead of waiting 500 ms for the watchdog. The watchdog stays: Disconnect is an optimization, the watchdog is the guarantee. (Why must you keep both? What packet-loss scenario breaks a Disconnect-only design?)

### 5.4 Addressing (ADR-2: one socket per machine, receiver learns sender's address)

- Receiver binds UDP port **47800** (any free registered-range port; put it in `protocol.h`).
- Sender binds an ephemeral port (bind to port 0, or just `sendto` and let the OS pick) and sends everything to `<pc_ip>:47800`, where `<pc_ip>` is a CLI argument.
- Receiver records the source address of the most recent valid Input/Heartbeat (per `recvfrom`) and sends Rumble packets back to it — on the same socket.
- Consequences: exactly one port to open in Windows Firewall; the Mac needs no inbound configuration; sender restarts are handled automatically (new ephemeral port just gets recorded). Trade-off: rumble can't flow until at least one packet has arrived — which is exactly what the 1 Hz heartbeat guarantees.
- First-run gotcha to expect: Windows Firewall will silently eat inbound UDP. Milestone 1 makes you prove connectivity with `nc` before any C++ exists, so you never debug firewall and code simultaneously.

### 5.5 Sequence numbers and staleness (fixes C5)

Purpose: UDP can reorder. If packet 200 arrives after 201, applying it would jerk inputs backward for one frame. Rule: on receipt, accept iff the incoming number is "ahead of" the last accepted one, using wraparound-aware comparison (RFC 1982; for `uint8_t`, one signed-difference trick — see C5). On accept, update `last_seq`. Reset `last_seq` state whenever a slot goes to neutral/disconnected (why? what happens after a sender restart otherwise?). Sequence numbers are per-controller and independent per direction; rumble packets carry their own counter.

### 5.6 Verification for this whole section

`static_assert` the sizeof of every packet type. Then Milestone 3 round-trips every struct through encode→decode on one machine and asserts equality, and you eyeball `4d 58 03 01 ...` in Wireshark. Only after that do the two OSes talk.

---

## 6. Sender design (macOS)

### 6.1 Threads

| Thread | Loop | Blocks on |
|---|---|---|
| Main / input | 120 Hz via `sleep_until` | the clock |
| Rumble receiver | until shutdown | `recvfrom` with 250 ms timeout |

macOS caveat worth knowing before you fight it: SDL event processing must happen on the *main* thread on macOS (Cocoa requirement). Keep `SDL_Init`, the event pump, and device open/close on the main thread; that is why the layout above is not arbitrary. Rumble calls from the second thread work in practice, but confirm current guidance: https://wiki.libsdl.org/SDL2/CategoryThread and the function docs for what's main-thread-only.

### 6.2 Input loop responsibilities, in order

1. `SDL_PumpEvents` / drain `SDL_PollEvent` — handle DEVICEADDED / DEVICEREMOVED (slot logic, 6.3).
2. For each open slot: read buttons and axes (`SDL_GameControllerGetButton/GetAxis`).
3. Map to wire format: button bitmask (5.3), Y inversion with the C2 clamp, trigger scale 0..32767 → 0..255 (v2.0's `/128` is correct — verify the two endpoints by hand), optional deadzone (off by default, C9).
4. Build packet, hton the multi-byte fields, `sendto`.
5. Once per second: heartbeat.
6. `sleep_until(next_tick)`.

Design question before coding: should you send only when state *changes*? Answer: no — send every tick. Work out why the watchdog (and loss recovery, NFR3) depends on that.

### 6.3 Slot assignment and hotplug

- Two slots, 0 and 1. On DEVICEADDED: open the controller, assign the lowest free slot, store the mapping `SDL_JoystickInstanceID → slot`.
- On DEVICEREMOVED: the event carries the *instance ID*, not your slot — that's the mapping's purpose. Close the handle, free the slot, send Disconnect ×3.
- A third controller: ignore it, log it.
- Edge cases to test on purpose in Milestone 8: unplug during heavy input; replug the same pad (does it get the same slot? does it matter?); both pads unplugged and replugged in swapped order.

### 6.4 Rumble thread

Loop: `recvfrom` (timeout) → validate magic/version/type/seq → scale motors (5.3) → `SDL_GameControllerRumble(pad, lo, hi, duration)`. The duration strategy and the stuck-rumble guard are designed by *you* in Milestone 7 — the constraint is C8: must survive a lost (0,0) packet, must not stutter under continuous rumble.

### 6.5 Controller agnosticism (PS5 DualSense, Xbox Series, mixed)

The design is controller-agnostic by construction: SDL normalizes every recognized pad into one logical layout on the Mac, and the wire protocol plus ViGEm erase physical identity on the PC — games always see virtual X360 pads. Mixing a DualSense and an Xbox Series pad requires no code changes. What you give up and what to watch:

- Anything without an XInput equivalent is lost: DualSense adaptive-trigger resistance, touchpad surface, gyro. Touchpad *click* is an SDL button with no X360 slot — handle it in the same 5.3 decision as the guide button.
- DualSense rumble is SDL's haptic emulation of classic motors — functional, feels softer than the Xbox pad. Not a bug.
- Bluetooth: SDL's HIDAPI backend puts the DualSense in enhanced-report mode to enable BT rumble automatically; if BT rumble fails, check hint `SDL_HINT_JOYSTICK_HIDAPI_PS5`. Xbox Series pads on macOS: prefer Bluetooth (native since Big Sur); verify wired support against a current source before depending on it.
- Log `SDL_GameControllerGetType()` per slot in M2, and make it the *only* permitted place for type-specific logic if a quirk ever demands one.

---

## 7. Receiver design (Windows)

### 7.1 Threads

| Thread | Loop | Blocks on |
|---|---|---|
| Net receiver | until shutdown | `recvfrom` with 250 ms timeout |
| Watchdog | 10 Hz | the clock |
| ViGEm callback | n/a (driver-invoked) | — |

### 7.2 Net receiver responsibilities

1. Validate: size for its type, magic, version, controller_id < 2, sequence rule (5.5). Drop invalid silently (log rate-limited).
2. `ntoh` the fields.
3. Input: translate to `XUSB_REPORT` (buttons copy straight over if you did 5.3 right; note `XUSB_REPORT`'s trigger and thumb field types and confirm they match your ranges — the XInput layout it mirrors is documented at https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad ), then `vigem_target_x360_update`.
4. Record `last_packet_time[controller_id] = now` and the sender's address.
5. Heartbeat: refresh sender address + liveness only. Disconnect: neutralize/remove that pad.

Decision to make consciously: allocate both ViGEm targets at startup and leave them plugged (simpler; PC always shows two pads), or plug/unplug dynamically on first-packet/Disconnect (cleaner for games that enumerate pads). Start with static; revisit after Milestone 8. Record it as a one-line ADR in your notes.

### 7.3 Watchdog

Every 100 ms, for each active slot: if `now − last_packet_time > 500 ms`, submit a neutral `XUSB_REPORT` once and mark the slot stale (don't spam updates every tick — why?). Data it reads is written by the net thread → 4.2 applies.

### 7.4 ViGEm rumble callback

Registered per target with `this` as user-data (4.7). Body: read motor bytes → build RumblePacket (own seq counter) → `sendto` the recorded sender address (if any) → return. No locks held long, no blocking, no logging in the hot path.

ViGEm lifecycle to get right (the samples show the order): `vigem_alloc` → `vigem_connect` → per-pad `vigem_target_x360_alloc` → `vigem_target_add` → `vigem_target_x360_register_notification`; mirror-image teardown, callbacks unregistered before target removal. SDK + samples: https://github.com/nefarius/ViGEmClient — and the driver itself is a separate install (ViGEmBus release installer from https://github.com/nefarius/ViGEmBus/releases ).

---

## 8. Milestones

Each milestone: **Goal → Before you code (questions you must be able to answer) → Done when (verifiable)**. Do them in order; every one produces something you can run and check. If a "Done when" is failing, section 9 is your toolbox.

### M0 — Toolchains exist
**Goal:** A `hello world` built by CMake on each machine.
**Before you code:** What's the difference between configuring (`cmake -B build`) and building (`cmake --build build`)? Where do Homebrew and vcpkg put headers/libs, and how does CMake find them (toolchain file on Windows)?
**Done when:** Both machines compile and run a C++17 binary from a `CMakeLists.txt` you wrote, not copied blind. On Windows: from a shell, with MSVC or clang — decide and note which.

### M1 — The network path works, with zero code
**Goal:** Prove UDP flows Mac→PC and PC→Mac on port 47800 using existing tools.
**Before you code:** nothing — that's the point.
**Done when:** `nc -u` (or `ncat` on Windows) shows text typed on one machine appearing on the other, both directions, with Windows Firewall configured to allow it. Note both IPs. If this fails, no C++ you write later can succeed — fix network first.

### M2 — SDL sees your controllers
**Goal:** Mac console app: init SDL, open controllers as they're added, print all axes/buttons of both pads at 120 Hz, handle removal, exit cleanly on Ctrl-C.
**Before you code:** Why `sleep_until` not `sleep_for` (4.1)? Which SDL calls belong on the main thread on macOS (6.1)? What is a controller *mapping* and what happens with a pad SDL doesn't recognize (see SDL_GameControllerDB: https://github.com/mdqinc/SDL_GameControllerDB )?
**Done when:** Printed values track physical movement for both pads simultaneously; unplug/replug doesn't crash; the loop measurably runs at ~120 Hz (print the actual measured period once per second — this instrumentation gets reused in M9).

### M3 — Protocol round-trip on one machine
**Goal:** `protocol.h` per section 5, plus encode/decode helpers, plus a tiny test binary that packs every packet type, "sends" to a socket on localhost, receives, decodes, and asserts field equality.
**Before you code:** Expected `sizeof` of each packet, computed by hand? Why the `uint16_t` cast around `htons` for `int16_t` fields (C13)? What does your seq-comparison return for (200,201), (255,0), (0,255), (0,128)?
**Done when:** static_asserts pass, round-trip test passes, and in Wireshark (filter `udp.port == 47800`) you can point at the bytes `4d 58 03` at the start of a captured packet.

### M4 — Inputs cross the network as data
**Goal:** Mac sends real InputPackets at 120 Hz; a *temporary* Windows console app (no ViGEm yet) receives, validates, and prints decoded state.
**Before you code:** Your `recvfrom` loop's three outcomes (4.6)? Where does the shutdown flag get checked?
**Done when:** Moving a stick on the Mac scrolls correct values on the PC screen; yanking the Mac's network cable makes packets stop and the app survive; sequence filter demonstrably drops a deliberately re-sent old packet (write a one-off test send for this).

### M5 — A virtual pad exists on Windows
**Goal:** Standalone Windows program (no network): create one ViGEm X360 pad and feed it a synthetic pattern (e.g., slow circle on left stick, A pressed every second).
**Before you code:** The lifecycle order (7.4)? What does `joy.cpl` show and how do you open it? Driver installed from the ViGEmBus release?
**Done when:** `joy.cpl` (and/or https://hardwaretester.com/gamepad in a browser) shows an Xbox 360 controller whose stick draws your synthetic circle. Steam should also see it.

### M6 — End to end, input direction
**Goal:** Merge M4+M5: real packets drive the virtual pad. Add the watchdog.
**Before you code:** Which data is shared between net thread and watchdog, and how is it protected (4.2)? Why does the watchdog neutralize once, not continuously (7.3)?
**Done when:** A game or hardwaretester on the PC mirrors the physical pad with no perceptible weirdness in axes (this is where a Y-inversion or endianness bug becomes *visible* — up is up, right is right, no drift at rest); killing the Mac app mid-input neutralizes the virtual pad within ~500 ms.

### M7 — Rumble backchannel
**Goal:** PC games' rumble reaches the physical pad.
**Before you code:** Your duration/refresh strategy versus both C8 failure modes — write it down in three sentences before implementing. How will you *trigger* rumble on demand for testing (hardwaretester has a vibration button; some games; or a tiny XInput test program — `XInputSetState` docs: https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate )?
**Done when:** Rumble in a test source buzzes the correct physical pad with unnoticeable delay; continuous rumble is smooth (no stutter); killing the PC app mid-rumble leaves the pad silent within your designed timeout.

### M8 — Two controllers + hotplug hardening
**Goal:** Slot logic from 6.3 fully exercised.
**Done when:** Every edge case listed in 6.3 behaves; each physical pad rumbles independently and only for its own slot; a 30-minute two-pad session shows no slot confusion.

### M9 — Measure, then tune
**Goal:** Replace "feels fast" with numbers.
**Method to build:** Add a timestamp echo — simplest form: a debug packet type carrying the Mac's `steady_clock` micros, echoed back verbatim by the PC; Mac computes RTT/2 on receipt (one clock, no cross-machine sync needed — that's *why* echo beats one-way timestamping; make sure you can explain it). Sample continuously, log min/median/p99.
**Also measure:** actual input-loop period (from M2), and packet loss (gaps in received seq per 10k packets).
**Done when:** You have median and p99 added-latency numbers over Wi-Fi and over Ethernet, and either they meet NFR1 or you know which hop is the problem. Knobs if you're over budget, in the order to try them: Ethernet instead of Wi-Fi; check you're not accidentally sleeping >8.3 ms (timer resolution on Windows matters for the watchdog, not the hot path — why?); confirm no Nagle-style buffering assumptions (UDP has none — so where else could delay hide? Enumerate the pipeline stages from stick to screen).

---

## 9. Debugging toolbox

- **Wireshark** (both platforms): filter `udp.port == 47800`. Reads: your bytes, their order (endianness bugs are visible here), packet rate, both directions. This is your ground truth when the two programs disagree.
- **netcat / ncat**: prove connectivity without your code (M1); also handy to hand-inject garbage at your receiver to test validation.
- **`joy.cpl`** (Win+R → `joy.cpl`) and **hardwaretester.com/gamepad**: inspect the virtual pad, trigger test rumble.
- **Console printf with timestamps**: crude, but for a 120 Hz pipeline, printing *every* packet will itself distort timing — rate-limit logs (e.g., once/sec summaries). Knowing that observation perturbs the system is half of real-time debugging.
- **Failure signature table** — commit this to memory:
  - Buttons/axes scrambled or wildly wrong values → endianness or struct layout mismatch (4.3/4.4). Check Wireshark bytes vs. spec by hand.
  - Everything offset by a few bytes → packing pragma missing on one side, or header/payload size drift → your `static_assert`s should have caught it; re-check both builds share the identical `protocol.h`.
  - Stick "up" goes down → C2 territory.
  - Works, then freezes ~2 s in, repeats → sequence wraparound filter (C5).
  - Works on same machine, nothing across network → firewall (M1), wrong IP, or bind on the wrong interface.
  - Random full-speed CPU core → busy-wait; you built option 3 of 4.6 without a poll.
  - Program won't exit → a thread stuck in a blocking call with no timeout (C11).
  - Rumble stuck on → C8; you lost the (0,0) packet and have no stop condition.

---

## 10. Pre-flight pitfalls checklist

Before each milestone's "done", scan this list:

1. Every multi-byte field passes through hton/ntoh exactly once per direction.
2. `static_assert` sizes present and identical header shared, not copied-and-drifted.
3. `-32768` Y-axis case clamped before negation.
4. Sequence comparison is wraparound-aware and covered by the M3 unit test.
5. All cross-thread data is atomic or mutex-protected — re-run the 4.2 inventory after each new feature.
6. Every blocking call has a timeout or a documented wake-up path; app exits cleanly on Ctrl-C on both platforms.
7. Every `alloc/open/connect` has one owner and a release on all exit paths (4.8 table).
8. Watchdog covers *both* directions of stuck state: stuck inputs on PC, stuck rumble on Mac.
9. Logs are rate-limited in hot paths.
10. Windows Firewall rule exists for UDP 47800 and you've retested after any network change.

---

## 11. Sources

Primary references (verify links — check anything that 404s against the project's main page; you already saw v2.0 cite a wrong repo):

| Topic | Source |
|---|---|
| SDL2 GameController API | https://wiki.libsdl.org/SDL2/CategoryGameController |
| SDL2 threading notes | https://wiki.libsdl.org/SDL2/CategoryThread |
| SDL2 with CMake | https://wiki.libsdl.org/SDL2/README/cmake |
| SDL controller mapping DB | https://github.com/mdqinc/SDL_GameControllerDB |
| SDL3 migration table (reference only) | https://wiki.libsdl.org/SDL3/README/migration |
| ViGEmClient SDK + samples | https://github.com/nefarius/ViGEmClient |
| ViGEmBus driver releases | https://github.com/nefarius/ViGEmBus/releases |
| ViGEm docs / EOL notice | https://docs.nefarius.at/projects/ViGEm/ |
| XInput gamepad layout (`XINPUT_GAMEPAD`) | https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad |
| UDP sockets, byte order (best single tutorial) | https://beej.us/guide/bgnet/ |
| Winsock getting started / porting from BSD | https://learn.microsoft.com/en-us/windows/win32/winsock/getting-started-with-winsock |
| Serial number arithmetic | https://www.rfc-editor.org/rfc/rfc1982 |
| std::thread, std::atomic, std::mutex, chrono | https://en.cppreference.com/ (search each name) |
| CMake tutorial | https://cmake.org/cmake/help/latest/guide/tutorial/ |
| vcpkg + CMake | https://learn.microsoft.com/en-us/vcpkg/get_started/get-started |

---

*End of Study TRD v3.0. When a milestone teaches you something this document got wrong, edit the document — it's yours now.*
