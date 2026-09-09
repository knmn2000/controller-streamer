// xinput-probe.cpp - test harness for HANDOFF gates T4/T6/T7.
// Not part of the shipped project; lives outside the repo on purpose.
//
//   xinput-probe list                    slots + state + dwPacketNumber
//   xinput-probe capture <slot> <secs>   per-axis extremes (T6 gates 2 and 3)
//   xinput-probe watch <slot>            print on every state change
//   xinput-probe rumble <slot> <lg> <sm> <ms>
//
// Reading notes, because output from this tool has been doubted:
//  * XINPUT_STATE is ZeroMemory'd before every call, and .Gamepad is read ONLY
//    when XInputGetState returned ERROR_SUCCESS. Other codes print the code and
//    no state, so a non-zero stick reading can never be uninitialized stack.
//  * dwPacketNumber increments when the reported STATE CHANGES - not once per
//    UDP packet received. A physically still stick sends identical reports, so
//    a frozen pkt does NOT mean the stream died. Use `capture` or watch the
//    watchdog instead to judge liveness.
//  * Params are lg/sm, not large/small: windows.h pulls in rpcndr.h, which
//    does "#define small char", so "int small" would expand to "int char".
//
// Build: cl /nologo /EHsc /MD xinput-probe.cpp /link xinput.lib

#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* xi_err(DWORD r) {
    switch (r) {
    case ERROR_SUCCESS:              return "SUCCESS";
    case ERROR_DEVICE_NOT_CONNECTED: return "DEVICE_NOT_CONNECTED";
    default:                         return "OTHER";
    }
}

// Fills *out and returns true only on ERROR_SUCCESS.
static bool poll_slot(int i, XINPUT_STATE* out, DWORD* rc) {
    ZeroMemory(out, sizeof(*out));
    *rc = XInputGetState(i, out);
    if (*rc == ERROR_SUCCESS) return true;
    ZeroMemory(out, sizeof(*out));
    return false;
}

static void print_slot(int i, const XINPUT_STATE& s) {
    std::printf("  slot %d  pkt=%-10lu buttons=0x%04x  LT=%3u RT=%3u  "
                "LX=%6d LY=%6d  RX=%6d RY=%6d\n",
                i, s.dwPacketNumber, s.Gamepad.wButtons,
                s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger,
                s.Gamepad.sThumbLX, s.Gamepad.sThumbLY,
                s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
}

static int cmd_list() {
    int connected = 0;
    for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE s; DWORD rc;
        if (poll_slot(i, &s, &rc)) { ++connected; print_slot(i, s); }
        else std::printf("  slot %d  -- %s (0x%08lX)\n", i, xi_err(rc), rc);
    }
    std::printf("connected: %d\n", connected);
    return connected;
}

// Record per-axis extremes over N seconds. T6 gates 2 and 3 in one pass:
// stick up must drive LY positive, right must drive LX positive, triggers
// must reach 255 and rest at 0.
static int cmd_capture(int slot, int seconds) {
    SHORT lxmin=0,lxmax=0,lymin=0,lymax=0,rxmin=0,rxmax=0,rymin=0,rymax=0;
    BYTE  ltmax=0, rtmax=0, ltmin=255, rtmin=255;
    WORD  btns=0;
    DWORD firstpkt=0, lastpkt=0;
    int polls=0, ok=0, changes=0;
    bool have=false;
    const DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    std::printf("capturing slot %d for %d s...\n", slot, seconds);
    std::fflush(stdout);
    while (GetTickCount() < end) {
        XINPUT_STATE s; DWORD rc;
        ++polls;
        if (poll_slot(slot, &s, &rc)) {
            ++ok;
            if (!have) { have = true; firstpkt = s.dwPacketNumber; }
            if (s.dwPacketNumber != lastpkt) ++changes;
            lastpkt = s.dwPacketNumber;
            const XINPUT_GAMEPAD& g = s.Gamepad;
            if (g.sThumbLX < lxmin) lxmin = g.sThumbLX;
            if (g.sThumbLX > lxmax) lxmax = g.sThumbLX;
            if (g.sThumbLY < lymin) lymin = g.sThumbLY;
            if (g.sThumbLY > lymax) lymax = g.sThumbLY;
            if (g.sThumbRX < rxmin) rxmin = g.sThumbRX;
            if (g.sThumbRX > rxmax) rxmax = g.sThumbRX;
            if (g.sThumbRY < rymin) rymin = g.sThumbRY;
            if (g.sThumbRY > rymax) rymax = g.sThumbRY;
            if (g.bLeftTrigger  > ltmax) ltmax = g.bLeftTrigger;
            if (g.bLeftTrigger  < ltmin) ltmin = g.bLeftTrigger;
            if (g.bRightTrigger > rtmax) rtmax = g.bRightTrigger;
            if (g.bRightTrigger < rtmin) rtmin = g.bRightTrigger;
            btns |= g.wButtons;
        }
        Sleep(8);
    }
    std::printf("polls=%d connected=%d  pkt %lu -> %lu (%d state changes)\n",
                polls, ok, firstpkt, lastpkt, changes);
    std::printf("  LX  min %6d  max %6d   (right should go POSITIVE)\n", lxmin, lxmax);
    std::printf("  LY  min %6d  max %6d   (up should go POSITIVE)\n", lymin, lymax);
    std::printf("  RX  min %6d  max %6d\n", rxmin, rxmax);
    std::printf("  RY  min %6d  max %6d\n", rymin, rymax);
    std::printf("  LT  min %4u  max %4u   (rest 0, full 255)\n", ltmin, ltmax);
    std::printf("  RT  min %4u  max %4u   (rest 0, full 255)\n", rtmin, rtmax);
    std::printf("  buttons seen (OR of all polls) 0x%04x\n", btns);
    return 0;
}

static int cmd_rumble(int slot, int lg, int sm, int ms) {
    XINPUT_STATE s; DWORD rc;
    if (!poll_slot(slot, &s, &rc)) {
        std::printf("slot %d not connected (%s) - nothing to rumble\n", slot, xi_err(rc));
        return 1;
    }
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed  = (WORD)(lg * 257);   // byte 0..255 -> 0..65535
    v.wRightMotorSpeed = (WORD)(sm * 257);
    std::printf("slot %d: large=%d small=%d for %d ms\n", slot, lg, sm, ms);
    if (XInputSetState(slot, &v) != ERROR_SUCCESS) { std::printf("XInputSetState failed\n"); return 1; }
    Sleep(ms);
    XINPUT_VIBRATION off{};
    XInputSetState(slot, &off);
    std::printf("slot %d: stopped\n", slot);
    return 0;
}

static int cmd_watch(int slot) {
    std::printf("watching slot %d, Ctrl-C to stop\n", slot);
    XINPUT_STATE prev{}; bool first = true;
    for (;;) {
        XINPUT_STATE s; DWORD rc;
        if (!poll_slot(slot, &s, &rc)) { std::printf("  slot %d %s\n", slot, xi_err(rc)); Sleep(500); continue; }
        if (first || std::memcmp(&s.Gamepad, &prev.Gamepad, sizeof(s.Gamepad)) != 0) {
            print_slot(slot, s); prev = s; first = false;
        }
        Sleep(50);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: xinput-probe list | capture <slot> <secs> | "
                    "watch <slot> | rumble <slot> <lg> <sm> <ms>\n");
        return 2;
    }
    if (std::strcmp(argv[1], "list") == 0) return cmd_list() > 0 ? 0 : 1;
    if (std::strcmp(argv[1], "capture") == 0 && argc == 4)
        return cmd_capture(std::atoi(argv[2]), std::atoi(argv[3]));
    if (std::strcmp(argv[1], "watch") == 0 && argc == 3) return cmd_watch(std::atoi(argv[2]));
    if (std::strcmp(argv[1], "rumble") == 0 && argc == 6)
        return cmd_rumble(std::atoi(argv[2]), std::atoi(argv[3]),
                          std::atoi(argv[4]), std::atoi(argv[5]));
    std::printf("bad arguments\n");
    return 2;
}
