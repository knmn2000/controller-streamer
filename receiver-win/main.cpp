// receiver-win/main.cpp — Controller Streamer receiver (Study TRD v3.0, section 7).
//
// Threads (TRD 7.1):
//   net thread      — recvfrom loop (250 ms timeout), validates, updates ViGEm
//                     pads, records sender address, echoes latency probes.
//   watchdog thread — 10 Hz: neutralizes stale pads (>500 ms silence) and
//                     refreshes non-zero rumble to the Mac (the C8 strategy).
//   ViGEm callbacks — driver-managed; store motor state + send one rumble
//                     packet, nothing else.
//   main thread     — setup, then sleeps until Ctrl-C, then teardown.
//
// Usage:  receiver            (binds UDP 47800 on all interfaces)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <ViGEm/Client.h>

#include "protocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using namespace proto;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Shared state, annotated per TRD 4.2. Globals => zero-initialized atomics.
// ---------------------------------------------------------------------------

static std::atomic<bool>  g_running{true};        // all threads; signal handler writes
static SOCKET             g_sock = INVALID_SOCKET;// created before threads; then shared
                                                  // (sendto/recvfrom are thread-safe)
static PVIGEM_CLIENT      g_client = nullptr;     // created before threads
static PVIGEM_TARGET      g_target[2] = {nullptr, nullptr};

static std::mutex         g_addr_mtx;             // guards the two lines below
static sockaddr_in        g_sender{};             // last source of a valid packet
static bool               g_have_sender = false;

static std::atomic<int64_t>  g_last_any_us{0};    // any valid packet; liveness clock
static std::atomic<bool>     g_sender_up{false};  // net sets, watchdog clears
static std::atomic<uint64_t> g_pkt_count{0};      // for the periodic rate line

static std::atomic<int64_t>  g_last_input_us[2];  // net writes, watchdog reads
static std::atomic<uint16_t> g_motors[2];         // large<<8|small; callback writes,
                                                  // watchdog reads, net thread resets
static std::atomic<uint8_t>  g_rumble_seq[2];     // callback + watchdog both send

static void on_signal(int) { g_running.store(false); }

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Logging. Every line is timestamped and flushed immediately.
//
// stdout is block-buffered whenever it is not a console (which is exactly the
// case when someone redirects to a file, or runs this from a launcher), so
// without the setvbuf in main() log lines sit in a 4 KB buffer and only appear
// at exit. That made a live diagnosis impossible more than once. The message
// text of the pre-existing lines is unchanged so existing notes and greps for
// them still work.
// ---------------------------------------------------------------------------

static std::mutex g_log_mtx;
static FILE*      g_logfile = nullptr;   // also written when --tray hides the console

static void logf(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    std::lock_guard<std::mutex> lk(g_log_mtx);
    for (FILE* out : {stdout, g_logfile}) {
        if (!out) continue;
        std::fputs(stamp, out);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(out, fmt, ap);
        va_end(ap);
        std::fputc('\n', out);
        if (out == g_logfile) std::fflush(out);
    }
}

static const char* ip_of(const sockaddr_in& a) {
    static char buf[INET_ADDRSTRLEN];   // log thread only
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// Rumble backchannel (TRD 7.4): sent to wherever input last came from (ADR-2).
// ---------------------------------------------------------------------------

static void send_rumble(int slot) {
    sockaddr_in dest;
    {
        std::lock_guard<std::mutex> lk(g_addr_mtx);
        if (!g_have_sender) return;   // heartbeat guarantees this fills within 1 s
        dest = g_sender;
    }
    const uint16_t m = g_motors[slot].load();
    RumblePacket rp{};
    fill_header(rp.header, PT_RUMBLE, (uint8_t)slot,
                g_rumble_seq[slot].fetch_add(1));
    rp.payload.large_motor = (uint8_t)(m >> 8);
    rp.payload.small_motor = (uint8_t)(m & 0xFF);
    sendto(g_sock, (const char*)&rp, sizeof(rp), 0,
           (const sockaddr*)&dest, sizeof(dest));
}

// ViGEm notification: driver thread. Do almost nothing here (TRD 4.7).
static VOID CALLBACK rumble_notification(PVIGEM_CLIENT, PVIGEM_TARGET,
                                         UCHAR large, UCHAR small,
                                         UCHAR /*led*/, LPVOID user) {
    const int slot = (int)(intptr_t)user;   // context pattern, TRD 4.7
    g_motors[slot].store((uint16_t)((large << 8) | small));
    send_rumble(slot);
}

// ---------------------------------------------------------------------------
// Input path
// ---------------------------------------------------------------------------

// Teardown for the startup error paths, in TRD 7.4 mirror order. Safe to call
// at any point during setup: every handle is null/INVALID until created, so
// this releases exactly what exists and nothing more. The normal shutdown path
// at the end of main() keeps its own inline copy of this order.
//
// Note ViGEmBus itself reclaims a process's targets when the owning handle
// closes at exit, so this does not prevent phantom pads (measured: it never
// leaves one). What it does fix is the leaked PVIGEM_CLIENT and socket on the
// error paths, which previously returned without releasing either.
static void teardown_partial() {
    for (int i = 0; i < 2; ++i) {
        if (!g_target[i]) continue;
        vigem_target_x360_unregister_notification(g_target[i]);
        if (g_client) vigem_target_remove(g_client, g_target[i]);
        vigem_target_free(g_target[i]);
        g_target[i] = nullptr;
    }
    if (g_client) {
        vigem_disconnect(g_client);
        vigem_free(g_client);
        g_client = nullptr;
    }
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

static void submit_neutral(int slot) {
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    vigem_target_x360_update(g_client, g_target[slot], r);
}

static void submit_input(int slot, const InputPayload& p) {
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    r.wButtons      = p.buttons;          // bit-identical by protocol design (5.3)
    r.bLeftTrigger  = p.left_trigger;
    r.bRightTrigger = p.right_trigger;
    r.sThumbLX      = p.left_stick_x;     // Y already XInput-oriented (sender inverts)
    r.sThumbLY      = p.left_stick_y;
    r.sThumbRX      = p.right_stick_x;
    r.sThumbRY      = p.right_stick_y;
    vigem_target_x360_update(g_client, g_target[slot], r);
}

static void net_thread_fn() {
    // input-direction sequence state: this thread only (TRD 5.5)
    uint8_t last_seq[2] = {0, 0};
    bool    have_seq[2] = {false, false};

    char buf[64];
    sockaddr_in src{};
    int srclen = sizeof(src);

    while (g_running.load()) {
        srclen = sizeof(src);
        const int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&src, &srclen);
        if (n == SOCKET_ERROR) continue;              // WSAETIMEDOUT: recheck flag
        if ((size_t)n < sizeof(Header)) continue;

        Header h;
        std::memcpy(&h, buf, sizeof(h));
        if (!header_valid(h)) continue;

        // Liveness, for every packet type except discovery. Replaces the old
        // one-shot "sender is alive" latch, which only fired if a heartbeat beat
        // the first input packet (it almost never did at 120 Hz vs 1 Hz) and
        // then never reset, so a reconnect was invisible.
        //
        // PT_DISCOVER is excluded on purpose: a phone scanning the LAN is not a
        // sender connecting, and counting it would announce a connection that
        // never streams anything.
        if (h.packet_type != PT_DISCOVER) {
            g_pkt_count.fetch_add(1);
            g_last_any_us.store(now_us());
            if (!g_sender_up.exchange(true))
                logf("[net] sender connected %s", ip_of(src));
        }

        switch (h.packet_type) {
        case PT_INPUT: {
            if ((size_t)n < sizeof(InputPacket) || h.controller_id >= 2) break;
            const int id = h.controller_id;
            const int64_t t = now_us();
            const int64_t last_t = g_last_input_us[id].load();
            const bool stale_link = (last_t == 0) || (t - last_t > 500'000);
            // Reject out-of-order UNLESS the link went quiet: a restarted
            // sender begins at seq 0 and must be accepted (TRD 5.5).
            if (have_seq[id] && !stale_link &&
                !seq_newer(h.sequence_num, last_seq[id])) break;
            last_seq[id] = h.sequence_num;
            have_seq[id] = true;

            InputPacket ip;
            std::memcpy(&ip, buf, sizeof(ip));
            decode_input(ip);
            submit_input(id, ip.payload);
            g_last_input_us[id].store(t);
            {
                std::lock_guard<std::mutex> lk(g_addr_mtx);
                g_sender = src;
                g_have_sender = true;
            }
            break;
        }
        case PT_HEARTBEAT: {
            std::lock_guard<std::mutex> lk(g_addr_mtx);
            g_sender = src;                 // also teaches us where rumble goes
            g_have_sender = true;
            break;
        }
        // Discovery: answer with our host name and pad count so a sender can
        // find us without being told an IP. It learns our address from the
        // source of this reply.
        case PT_DISCOVER: {
            DiscoverReply rp{};
            fill_header(rp.header, PT_DISCOVER_REPLY, CONTROLLER_NONE, h.sequence_num);
            char name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
            DWORD n_name = sizeof(name);
            if (!GetComputerNameA(name, &n_name)) { name[0] = '?'; n_name = 1; }
            if (n_name > sizeof(rp.payload.name)) n_name = sizeof(rp.payload.name);
            rp.payload.pad_count = 2;
            rp.payload.name_len  = (uint8_t)n_name;
            std::memcpy(rp.payload.name, name, n_name);
            sendto(g_sock, (const char*)&rp, sizeof(rp), 0,
                   (const sockaddr*)&src, srclen);
            logf("[net] discovery probe from %s -> replied \"%.*s\"",
                 ip_of(src), (int)n_name, name);
            break;
        }
        case PT_DISCONNECT: {
            if (h.controller_id >= 2) break;
            const int id = h.controller_id;
            if (g_last_input_us[id].exchange(0) != 0) {   // first of the x3 wins
                submit_neutral(id);
                g_motors[id].store(0);
                have_seq[id] = false;
                logf("[net] controller %d disconnected by sender", id);
            }
            break;
        }
        case PT_LATENCY:                                   // echo verbatim (M9)
            sendto(g_sock, buf, n, 0, (const sockaddr*)&src, srclen);
            break;
        default:
            break;
        }
    }
}

// Watchdog (TRD 7.3) + rumble refresher (C8): one 10 Hz loop, two jobs.
static void watchdog_thread_fn() {
    bool neutralized[2] = {true, true};   // this thread only
    int64_t last_stat_us = now_us();
    uint64_t last_stat_pkts = 0;
    while (g_running.load()) {
        const int64_t t = now_us();

        // Sender liveness. 2 s is deliberately longer than the 1 Hz heartbeat
        // so a single dropped heartbeat does not report a false loss.
        const int64_t last_any = g_last_any_us.load();
        if (g_sender_up.load() && last_any != 0 && t - last_any > 2'000'000) {
            if (g_sender_up.exchange(false))
                logf("[net] sender LOST (no packets for %.1f s)",
                     (t - last_any) / 1e6);
        }

        // Status line every 5 s: enough to see throughput and who is talking,
        // rare enough not to bury the event lines.
        if (t - last_stat_us >= 5'000'000) {
            const uint64_t pkts = g_pkt_count.load();
            const double secs = (t - last_stat_us) / 1e6;
            const double rate = (pkts - last_stat_pkts) / secs;
            char who[INET_ADDRSTRLEN] = "-";
            {
                std::lock_guard<std::mutex> lk(g_addr_mtx);
                if (g_have_sender) inet_ntop(AF_INET, &g_sender.sin_addr, who, sizeof(who));
            }
            const bool a0 = g_last_input_us[0].load() != 0 && t - g_last_input_us[0].load() <= 500'000;
            const bool a1 = g_last_input_us[1].load() != 0 && t - g_last_input_us[1].load() <= 500'000;
            logf("[stat] %.0f pkt/s  sender %s  pads: 0=%s 1=%s",
                 rate, who, a0 ? "active" : "idle", a1 ? "active" : "idle");
            last_stat_us = t;
            last_stat_pkts = pkts;
        }

        for (int i = 0; i < 2; ++i) {
            const int64_t last = g_last_input_us[i].load();
            if (last != 0 && t - last > 500'000) {
                if (!neutralized[i]) {                    // neutralize ONCE (7.3)
                    submit_neutral(i);
                    g_motors[i].store(0);
                    neutralized[i] = true;
                    logf("[watchdog] controller %d stale -> neutral", i);
                }
            } else if (last != 0) {
                neutralized[i] = false;
            }
            if (g_motors[i].load() != 0)                  // keep Mac's 600 ms
                send_rumble(i);                           // expiry fed (C8)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// Tray mode (--tray). Opt-in on purpose: with no arguments the program behaves
// exactly as before, which is the form every gate in HANDOFF section 5 was
// verified against. In tray mode the console is hidden, so logging also goes to
// a file - otherwise an autostarted instance would have nowhere to report.
// ---------------------------------------------------------------------------

static const UINT  TRAY_MSG = WM_APP + 1;
static const UINT  ID_QUIT  = 1001;
static const UINT  ID_OPEN_LOG = 1002;
static HWND        g_tray_wnd = nullptr;
static char        g_log_path[MAX_PATH] = {0};

static void tray_tooltip(NOTIFYICONDATAA& nid) {
    char who[INET_ADDRSTRLEN] = "-";
    {
        std::lock_guard<std::mutex> lk(g_addr_mtx);
        if (g_have_sender) inet_ntop(AF_INET, &g_sender.sin_addr, who, sizeof(who));
    }
    std::snprintf(nid.szTip, sizeof(nid.szTip),
                  g_sender_up.load() ? "Controller Streamer - streaming from %s"
                                     : "Controller Streamer - waiting for a sender",
                  who);
}

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == TRAY_MSG && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        HMENU m = CreatePopupMenu();
        AppendMenuA(m, MF_STRING, ID_OPEN_LOG, "Open log");
        AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(m, MF_STRING, ID_QUIT, "Quit");
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(h);
        TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
        DestroyMenu(m);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == ID_QUIT)     { g_running.store(false); PostQuitMessage(0); }
        if (LOWORD(wp) == ID_OPEN_LOG) ShellExecuteA(nullptr, "open", g_log_path,
                                                     nullptr, nullptr, SW_SHOW);
        return 0;
    }
    if (msg == WM_DESTROY) { g_running.store(false); PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, msg, wp, lp);
}

static void run_tray_loop() {
    WNDCLASSA wc{};
    wc.lpfnWndProc   = tray_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ControllerStreamerTray";
    RegisterClassA(&wc);
    g_tray_wnd = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0,
                               nullptr, nullptr, wc.hInstance, nullptr);

    NOTIFYICONDATAA nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_tray_wnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = TRAY_MSG;
    nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    tray_tooltip(nid);
    Shell_NotifyIconA(NIM_ADD, &nid);

    // 500 ms timer: refresh the tooltip, and notice a shutdown requested from
    // anywhere else (a signal, or a failing thread) so we leave the loop.
    SetTimer(g_tray_wnd, 1, 500, nullptr);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            if (!g_running.load()) break;
            tray_tooltip(nid);
            Shell_NotifyIconA(NIM_MODIFY, &nid);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    Shell_NotifyIconA(NIM_DELETE, &nid);
}

static bool open_log_file() {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) return false;
    std::snprintf(g_log_path, sizeof(g_log_path), "%s\\controller-streamer", dir);
    CreateDirectoryA(g_log_path, nullptr);
    std::snprintf(g_log_path, sizeof(g_log_path), "%s\\controller-streamer\\receiver.log", dir);
    g_logfile = std::fopen(g_log_path, "a");
    return g_logfile != nullptr;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    bool tray = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--tray") == 0) tray = true;

    // Unbuffered: stdout is block-buffered when it is not a console, so a
    // redirected or launcher-started run would otherwise hold every log line
    // in a 4 KB buffer until exit.
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (tray) {
        if (open_log_file()) logf("--- tray mode, logging to %s ---", g_log_path);
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == INVALID_SOCKET) {
        std::fprintf(stderr, "socket: %d\n", WSAGetLastError());
        teardown_partial();
        return 1;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_sock, (const sockaddr*)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        std::fprintf(stderr, "bind %u: %d (port in use?)\n", PORT, WSAGetLastError());
        teardown_partial();
        return 1;
    }
    DWORD timeout_ms = 250;   // threads wake 4x/s to check g_running (TRD 4.6/C11)
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout_ms, sizeof(timeout_ms));

    g_client = vigem_alloc();
    if (!g_client || !VIGEM_SUCCESS(vigem_connect(g_client))) {
        std::fprintf(stderr, "ViGEm connect failed — is the ViGEmBus driver installed?\n");
        teardown_partial();
        return 1;
    }
    // Static allocation of both pads (decision recorded in TRD 7.2).
    for (int i = 0; i < 2; ++i) {
        g_target[i] = vigem_target_x360_alloc();
        // Print the VIGEM_ERROR code, not just "failed": it is the only thing
        // that separates BUS_NOT_FOUND (driver absent) from
        // BUS_VERSION_MISMATCH (client/driver skew) from TIMED_OUT (bus not
        // settled yet, e.g. right after installing the driver).
        VIGEM_ERROR e = vigem_target_add(g_client, g_target[i]);
        if (!VIGEM_SUCCESS(e)) {
            std::fprintf(stderr, "vigem_target_add(%d) failed: 0x%08X\n", i, (unsigned)e);
            teardown_partial();
            return 1;
        }
        e = vigem_target_x360_register_notification(
                g_client, g_target[i], rumble_notification, (LPVOID)(intptr_t)i);
        if (!VIGEM_SUCCESS(e)) {
            std::fprintf(stderr, "register_notification(%d) failed: 0x%08X\n", i, (unsigned)e);
            teardown_partial();
            return 1;
        }
        submit_neutral(i);
    }
    // MEASURED ViGEm behaviour: a report identical to the previously submitted
    // one is suppressed, and the client's cached report starts all-zero. So the
    // neutral submitted above is a no-op and the pad keeps whatever stale values
    // its driver buffer happens to hold - every run it read LX=-3356 LY=-1869
    // rather than centred. Nudging one axis one count off centre, then
    // submitting the real neutral, makes the neutral a genuine change so it
    // lands. Without this an idle pad looks like a broken axis path and sends
    // you chasing byte order for nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int i = 0; i < 2; ++i) {
        XUSB_REPORT nudge;
        XUSB_REPORT_INIT(&nudge);
        nudge.sThumbLX = 1;
        vigem_target_x360_update(g_client, g_target[i], nudge);
        submit_neutral(i);
    }

    logf("2 virtual X360 pads up, listening on UDP %u - Ctrl-C to quit", PORT);

    std::signal(SIGINT, on_signal);
    std::thread net(net_thread_fn);
    std::thread watchdog(watchdog_thread_fn);

    if (tray) {
        run_tray_loop();                       // returns when Quit is chosen
    } else {
        while (g_running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logf("shutting down...");
    net.join();
    watchdog.join();
    for (int i = 0; i < 2; ++i) {                 // mirror-image teardown (TRD 7.4)
        vigem_target_x360_unregister_notification(g_target[i]);
        vigem_target_remove(g_client, g_target[i]);
        vigem_target_free(g_target[i]);
    }
    vigem_disconnect(g_client);
    vigem_free(g_client);
    closesocket(g_sock);
    WSACleanup();
    return 0;
}
