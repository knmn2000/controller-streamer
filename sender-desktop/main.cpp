// sender-desktop/main.cpp — Controller Streamer sender (Study TRD v3.0, section 6).
// Builds on macOS, Windows and Linux: SDL2 for input, plus the small socket shim below.
//
// Threads (TRD 6.1):
//   main thread   — ALL SDL calls (a macOS/Cocoa requirement; harmless elsewhere),
//                   applies rumble, sends Input/Heartbeat/Disconnect/Latency.
//   rumble thread — blocks on recvfrom (250 ms timeout, TRD 4.6 option 2),
//                   validates Rumble/Latency-echo packets, stores rumble state
//                   in atomics for the main thread to apply. Never touches SDL.
//
// Usage:  sender <pc_ip> [--measure]

// On Windows SDL renames main() to SDL_main unless told otherwise. This is a
// console app that opens no window, so we keep our own main and tell SDL it is
// ready. Untouched on macOS/Linux, so their already-verified behaviour is
// exactly as before.
#if defined(_WIN32)
  #define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include "protocol.h"

// ---- socket portability ---------------------------------------------------
// The only platform-specific surface in this file. Winsock and BSD sockets
// disagree on exactly four things - the handle type, its invalid value, how a
// receive timeout is expressed, and how a handle is closed - so those get a
// shim here and the rest of the file stays platform-neutral.
//
// protocol.h has already pulled in winsock2.h on Windows; including it again is
// harmless, and ws2tcpip.h is what provides inet_pton there.
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  static const socket_t k_invalid_socket = INVALID_SOCKET;
  static void close_socket(socket_t s) { closesocket(s); }
  static void set_rcv_timeout_ms(socket_t s, int ms) {
      DWORD t = (DWORD)ms;                    // Winsock takes milliseconds
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
  }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using socket_t = int;
  static const socket_t k_invalid_socket = -1;
  static void close_socket(socket_t s) { ::close(s); }
  static void set_rcv_timeout_ms(socket_t s, int ms) {
      timeval tv{};                           // BSD takes a timeval
      tv.tv_sec  = ms / 1000;
      tv.tv_usec = (ms % 1000) * 1000;
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace proto;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Shared state. Globals get static (zero) initialization, which sidesteps
// C++17's uninitialized-atomic default constructor. Each variable is
// annotated with which threads touch it (the TRD 4.2 inventory).
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};   // all threads read; signal handler writes
static socket_t          g_sock = k_invalid_socket;  // created before threads start; then read-only
static sockaddr_in       g_dest{};          // read-only after main() sets it

struct Slot {
    // main thread only:
    SDL_GameController* gc = nullptr;
    SDL_JoystickID      instance = -1;
    uint8_t             seq = 0;             // input-direction sequence counter
    uint16_t            applied_motors = 0;  // last motors passed to SDL (large<<8|small)
    int64_t             applied_at_us = 0;

    // written by rumble thread, read by main thread:
    std::atomic<uint16_t> pending_motors;    // large<<8|small
    std::atomic<int64_t>  last_rumble_us;    // 0 = never
};
static std::array<Slot, 2> g_slots;          // slot bookkeeping fields: main thread only

static void on_signal(int) { g_running.store(false); }

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

static void send_bytes(const void* data, size_t len) {
    // Best-effort by design (NFR3): a lost frame is superseded ~8 ms later.
    (void)sendto(g_sock, (const char*)data, (int)len, 0,
                 (const sockaddr*)&g_dest, sizeof(g_dest));
}

// ---------------------------------------------------------------------------
// Rumble thread (TRD 6.4). Socket-only; SDL application happens on main.
// ---------------------------------------------------------------------------

static void rumble_thread_fn(bool measure) {
    uint8_t last_rumble_seq[2] = {0, 0};
    bool    have_seq[2]        = {false, false};

    // latency stats (this thread only)
    int64_t win_start = now_us();
    int64_t rtt_min = 0, rtt_max = 0, rtt_sum = 0;
    int     rtt_n = 0;

    unsigned char buf[64];
    while (g_running.load()) {
        // int, not ssize_t: Winsock recvfrom returns int.
        const int n = recvfrom(g_sock, (char*)buf, (int)sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) continue;  // timeout or transient error: loop, recheck the flag
        if ((size_t)n < sizeof(Header)) continue;

        Header h;
        std::memcpy(&h, buf, sizeof(h));
        if (!header_valid(h)) continue;

        if (h.packet_type == PT_RUMBLE && (size_t)n >= sizeof(RumblePacket)) {
            if (h.controller_id >= 2) continue;
            const int id = h.controller_id;
            if (have_seq[id] && !seq_newer(h.sequence_num, last_rumble_seq[id])) continue;
            last_rumble_seq[id] = h.sequence_num;
            have_seq[id] = true;

            RumblePacket rp;
            std::memcpy(&rp, buf, sizeof(rp));
            const uint16_t motors =
                (uint16_t)((rp.payload.large_motor << 8) | rp.payload.small_motor);
            g_slots[id].pending_motors.store(motors);
            g_slots[id].last_rumble_us.store(now_us());
        } else if (measure && h.packet_type == PT_LATENCY &&
                   (size_t)n >= sizeof(LatencyPacket)) {
            LatencyPacket lp;
            std::memcpy(&lp, buf, sizeof(lp));
            decode_latency(lp);
            const int64_t rtt = now_us() - (int64_t)lp.payload.t_us;
            if (rtt >= 0) {
                if (rtt_n == 0 || rtt < rtt_min) rtt_min = rtt;
                if (rtt > rtt_max) rtt_max = rtt;
                rtt_sum += rtt;
                ++rtt_n;
            }
            const int64_t t = now_us();
            if (t - win_start >= 1'000'000 && rtt_n > 0) {
                std::printf("[latency] n=%d  one-way est (rtt/2): min %.2f ms  "
                            "avg %.2f ms  max %.2f ms\n",
                            rtt_n, rtt_min / 2000.0,
                            (rtt_sum / (double)rtt_n) / 2000.0, rtt_max / 2000.0);
                win_start = t;
                rtt_n = 0; rtt_sum = 0; rtt_min = 0; rtt_max = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Slot management (TRD 6.3)
// ---------------------------------------------------------------------------

static const char* type_name(SDL_GameControllerType t) {
    switch (t) {
        case SDL_CONTROLLER_TYPE_PS3:                return "PS3";
        case SDL_CONTROLLER_TYPE_PS4:                return "PS4";
        case SDL_CONTROLLER_TYPE_PS5:                return "PS5";
        case SDL_CONTROLLER_TYPE_XBOX360:            return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE:            return "Xbox One/Series";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:return "Switch Pro";
        default:                                     return "generic";
    }
}

static void send_disconnect(int slot_idx, uint8_t& seq) {
    DisconnectPacket dp{};
    for (int i = 0; i < 3; ++i) {  // it's UDP; say it thrice (TRD 5.3)
        fill_header(dp.header, PT_DISCONNECT, (uint8_t)slot_idx, seq++);
        send_bytes(&dp, sizeof(dp));
    }
}

static void open_device(int device_index) {
    if (!SDL_IsGameController(device_index)) return;

    SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(device_index);
    for (const Slot& s : g_slots)  // startup scan + ADDED event can both fire
        if (s.gc && s.instance == inst) return;

    int slot_idx = -1;
    for (int i = 0; i < 2; ++i)
        if (!g_slots[i].gc) { slot_idx = i; break; }
    if (slot_idx < 0) {
        std::printf("[slots] third controller ignored (device %d)\n", device_index);
        return;
    }

    SDL_GameController* gc = SDL_GameControllerOpen(device_index);
    if (!gc) {
        std::printf("[slots] open failed: %s\n", SDL_GetError());
        return;
    }
    Slot& s = g_slots[slot_idx];
    s.gc = gc;
    s.instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    s.applied_motors = 0;
    s.pending_motors.store(0);
    s.last_rumble_us.store(0);
    std::printf("[slots] slot %d <- \"%s\" (%s)\n", slot_idx,
                SDL_GameControllerName(gc),
                type_name(SDL_GameControllerGetType(gc)));
}

static void close_slot(int slot_idx, bool announce) {
    Slot& s = g_slots[slot_idx];
    if (!s.gc) return;
    if (announce) send_disconnect(slot_idx, s.seq);
    SDL_GameControllerClose(s.gc);
    s.gc = nullptr;
    s.instance = -1;
    s.applied_motors = 0;
    std::printf("[slots] slot %d removed\n", slot_idx);
}

// ---------------------------------------------------------------------------
// Input sampling (TRD 6.2)
// ---------------------------------------------------------------------------

struct BtnMap { SDL_GameControllerButton sdl; uint16_t bit; };
static const BtnMap k_buttons[] = {
    {SDL_CONTROLLER_BUTTON_A,             BTN_A},
    {SDL_CONTROLLER_BUTTON_B,             BTN_B},
    {SDL_CONTROLLER_BUTTON_X,             BTN_X},
    {SDL_CONTROLLER_BUTTON_Y,             BTN_Y},
    {SDL_CONTROLLER_BUTTON_BACK,          BTN_BACK},
    {SDL_CONTROLLER_BUTTON_GUIDE,         BTN_GUIDE},
    {SDL_CONTROLLER_BUTTON_START,         BTN_START},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,     BTN_LEFT_THUMB},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK,    BTN_RIGHT_THUMB},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  BTN_LEFT_SHOULDER},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_RIGHT_SHOULDER},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,       BTN_DPAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,     BTN_DPAD_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,     BTN_DPAD_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    BTN_DPAD_RIGHT},
    // TOUCHPAD / MISC1 / paddles: no XInput equivalent, deliberately dropped
    // (TRD 5.3 / 6.5).
};

static void sample_and_send(Slot& s, uint8_t slot_idx) {
    InputPacket p{};
    fill_header(p.header, PT_INPUT, slot_idx, s.seq++);

    uint16_t buttons = 0;
    for (const BtnMap& m : k_buttons)
        if (SDL_GameControllerGetButton(s.gc, m.sdl)) buttons |= m.bit;
    p.payload.buttons = buttons;

    p.payload.left_stick_x  = SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_LEFTX);
    p.payload.left_stick_y  = invert_axis(SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_LEFTY));
    p.payload.right_stick_x = SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_RIGHTX);
    p.payload.right_stick_y = invert_axis(SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_RIGHTY));
    p.payload.left_trigger  = trigger_to_byte(SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    p.payload.right_trigger = trigger_to_byte(SDL_GameControllerGetAxis(s.gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

    encode_input(p);
    send_bytes(&p, sizeof(p));
}

// Rumble application, main thread (TRD C8 strategy):
// receiver refreshes non-zero rumble at 10 Hz; we apply with an 800 ms SDL
// duration and re-apply every 400 ms while non-zero, and force-stop if no
// packet has arrived for 600 ms. A lost (0,0) therefore self-heals in
// <=600 ms; continuous rumble never stutters.
static void apply_rumble(Slot& s) {
    if (!s.gc) return;
    const int64_t last = s.last_rumble_us.load();
    if (last == 0) return;

    const int64_t  t  = now_us();
    const uint16_t pm = s.pending_motors.load();

    if (t - last > 600'000) {
        if (s.applied_motors != 0) {
            SDL_GameControllerRumble(s.gc, 0, 0, 0);
            s.applied_motors = 0;
        }
        return;
    }
    const bool changed = pm != s.applied_motors;
    const bool refresh = pm != 0 && (t - s.applied_at_us) > 400'000;
    if (changed || refresh) {
        SDL_GameControllerRumble(s.gc,
                                 motor_to_sdl((uint8_t)(pm >> 8)),
                                 motor_to_sdl((uint8_t)(pm & 0xFF)),
                                 800 /* ms */);
        s.applied_motors = pm;
        s.applied_at_us  = t;
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Unbuffered: stdout is block-buffered when it is not a console, so piping
    // or redirecting this program would otherwise hold [slots] and [latency]
    // lines in a 4 KB buffer until exit - which makes a live diagnosis
    // impossible. Same fix as receiver-win/main.cpp.
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <pc_ipv4> [--measure]\n", argv[0]);
        return 1;
    }
    const bool measure = (argc >= 3 && std::strcmp(argv[2], "--measure") == 0);

#if defined(_WIN32)
    SDL_SetMainReady();                       // pairs with SDL_MAIN_HANDLED
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    std::memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family = AF_INET;
    g_dest.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &g_dest.sin_addr) != 1) {
        std::fprintf(stderr, "invalid IPv4 address: %s\n", argv[1]);
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == k_invalid_socket) { std::perror("socket"); return 1; }
    // rumble thread wakes 4x/s to check g_running (TRD 4.6/C11)
    set_rcv_timeout_ms(g_sock, 250);

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    for (int i = 0; i < SDL_NumJoysticks(); ++i) open_device(i);  // pre-connected pads
    std::printf("streaming to %s:%u at 120 Hz — Ctrl-C to quit\n", argv[1], PORT);

    std::thread rumble_thread(rumble_thread_fn, measure);

    constexpr auto k_period = std::chrono::microseconds(8333);  // 120 Hz
    auto next_tick = Clock::now() + k_period;
    uint64_t tick = 0;
    uint8_t  hb_seq = 0, lat_seq = 0;

    while (g_running.load()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                open_device(ev.cdevice.which);            // which = device index
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (int i = 0; i < 2; ++i)               // which = instance id
                    if (g_slots[i].gc && g_slots[i].instance == ev.cdevice.which)
                        close_slot(i, true);
            }
        }

        for (uint8_t i = 0; i < 2; ++i) {
            if (!g_slots[i].gc) continue;
            sample_and_send(g_slots[i], i);
            apply_rumble(g_slots[i]);
        }

        if (tick % 120 == 0) {                            // heartbeat, 1 Hz (TRD 5.3)
            HeartbeatPacket hp{};
            fill_header(hp.header, PT_HEARTBEAT, CONTROLLER_NONE, hb_seq++);
            send_bytes(&hp, sizeof(hp));
        }
        if (measure && tick % 12 == 0) {                  // latency probe, 10 Hz (M9)
            LatencyPacket lp{};
            fill_header(lp.header, PT_LATENCY, CONTROLLER_NONE, lat_seq++);
            lp.payload.t_us = (uint64_t)now_us();
            encode_latency(lp);
            send_bytes(&lp, sizeof(lp));
        }

        ++tick;
        std::this_thread::sleep_until(next_tick);
        next_tick += k_period;
        if (Clock::now() > next_tick + k_period)          // fell far behind (lid close,
            next_tick = Clock::now() + k_period;          // debugger): resync, don't spiral
    }

    std::printf("\nshutting down...\n");
    for (int i = 0; i < 2; ++i) close_slot(i, true);      // tell PC to neutralize now
    rumble_thread.join();
    SDL_Quit();
    close_socket(g_sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}
