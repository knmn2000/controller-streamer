// controller-streamer - stream a game controller over the LAN to a Windows PC
// Copyright (C) 2026 knmn2000
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// protocol_test.cpp — Milestone 3: round-trip and edge-case tests for protocol.h.
// Build:  g++ -std=c++17 -I../protocol protocol_test.cpp -o protocol_test && ./protocol_test
#include "protocol.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

using namespace proto;

static void test_sizes_and_wire_bytes() {
    // static_asserts in protocol.h already gate sizes at compile time;
    // here we additionally eyeball the first bytes on the "wire".
    InputPacket p{};
    fill_header(p.header, PT_INPUT, 0, 42);
    p.payload.buttons = BTN_A | BTN_DPAD_LEFT;
    encode_input(p);

    unsigned char wire[sizeof(InputPacket)];
    std::memcpy(wire, &p, sizeof(p));
    CHECK(wire[0] == 0x4D && wire[1] == 0x58);      // magic big-endian "MX"
    CHECK(wire[2] == 0x03);                          // version
    CHECK(wire[3] == 0x01);                          // type: input
    CHECK(wire[4] == 0x00 && wire[5] == 42);         // id, seq
    CHECK(wire[6] == 0x10 && wire[7] == 0x04);       // buttons 0x1004 big-endian
}

static void test_input_roundtrip_edges() {
    InputPacket p{};
    fill_header(p.header, PT_INPUT, 1, 200);
    p.payload.buttons       = 0xF3FF;
    p.payload.left_stick_x  = INT16_MIN;
    p.payload.left_stick_y  = INT16_MAX;
    p.payload.right_stick_x = -1;
    p.payload.right_stick_y = 12345;
    p.payload.left_trigger  = 255;
    p.payload.right_trigger = 7;

    InputPacket copy = p;         // "send"
    encode_input(copy);
    decode_input(copy);           // "receive"
    CHECK(header_valid(copy.header));
    CHECK(std::memcmp(&copy.payload, &p.payload, sizeof(InputPayload)) == 0);
}

static void test_latency_roundtrip() {
    LatencyPacket p{};
    fill_header(p.header, PT_LATENCY, CONTROLLER_NONE, 0);
    p.payload.t_us = 0x0102030405060708ULL;
    LatencyPacket c = p;
    encode_latency(c);
    unsigned char* b = (unsigned char*)&c.payload.t_us;
    CHECK(b[0] == 0x01 && b[7] == 0x08);  // big-endian on the wire
    decode_latency(c);
    CHECK(c.payload.t_us == p.payload.t_us);
}

static void test_header_rejects() {
    Header h;
    fill_header(h, PT_HEARTBEAT, CONTROLLER_NONE, 0);
    CHECK(header_valid(h));
    Header bad_magic = h;   bad_magic.magic = h2n_u16(0xDEAD);
    CHECK(!header_valid(bad_magic));
    Header bad_ver = h;     bad_ver.version = 0x02;   // v2 talker must be rejected
    CHECK(!header_valid(bad_ver));
}

static void test_seq_newer() {
    CHECK( seq_newer(201, 200));   // ordinary advance
    CHECK(!seq_newer(200, 201));   // stale
    CHECK(!seq_newer(200, 200));   // duplicate
    CHECK( seq_newer(0,   255));   // the wrap that broke v2.0 (C5)
    CHECK( seq_newer(5,   250));   // wrap, several ahead
    CHECK(!seq_newer(255, 0));     // one behind, across the wrap
    CHECK(!seq_newer(128, 0));     // exactly half the circle counts as old
    CHECK( seq_newer(127, 0));     // just under half counts as new
}

static void test_mappings() {
    CHECK(invert_axis(INT16_MIN) == INT16_MAX);  // the C2 overflow case
    CHECK(invert_axis(INT16_MAX) == -32767);
    CHECK(invert_axis(0) == 0);
    CHECK(invert_axis(100) == -100);

    CHECK(trigger_to_byte(0) == 0);
    CHECK(trigger_to_byte(-5) == 0);             // resting-noise clamp
    CHECK(trigger_to_byte(127) == 0);
    CHECK(trigger_to_byte(128) == 1);
    CHECK(trigger_to_byte(INT16_MAX) == 255);    // top end lands exactly

    CHECK(motor_to_sdl(0) == 0);
    CHECK(motor_to_sdl(255) == 65535);           // full byte -> full SDL range
    CHECK(motor_to_sdl(128) == 32896);
}

int main() {
    test_sizes_and_wire_bytes();
    test_input_roundtrip_edges();
    test_latency_roundtrip();
    test_header_rejects();
    test_seq_newer();
    test_mappings();
    if (failures == 0) { std::printf("protocol_test: ALL PASS\n"); return 0; }
    std::printf("protocol_test: %d FAILURES\n", failures);
    return 1;
}
