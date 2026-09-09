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
// fake-sender.cpp - stand in for a real sender so the receiver's packet path
// can be verified from the PC alone (T6 gates 2/3 and the T8 echo mechanism).
// Built against the repo's protocol/protocol.h - the SAME single source of
// truth the real sender uses, so this also proves wire compatibility.
//
//   fake-sender <ip> heartbeat
//   fake-sender <ip> input <slot> <lx> <ly> <rx> <ry> <lt> <rt> <count>
//   fake-sender <ip> disconnect <slot>
//   fake-sender <ip> latency <count>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

using namespace proto;

static SOCKET s_sock;
static sockaddr_in s_dst;

static void send_raw(const void* p, int n) {
    if (sendto(s_sock, (const char*)p, n, 0, (const sockaddr*)&s_dst, sizeof(s_dst)) != n)
        std::printf("sendto failed: %d\n", WSAGetLastError());
}

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("see header for usage\n"); return 2; }
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    DWORD to = 1000;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port   = htons(PORT);
    inet_pton(AF_INET, argv[1], &s_dst.sin_addr);

    const char* cmd = argv[2];

    if (std::strcmp(cmd, "heartbeat") == 0) {
        HeartbeatPacket hb{};
        fill_header(hb.header, PT_HEARTBEAT, CONTROLLER_NONE, 0);
        send_raw(&hb, sizeof(hb));
        std::printf("sent heartbeat (%zu bytes)\n", sizeof(hb));
        return 0;
    }

    if (std::strcmp(cmd, "input") == 0 && argc == 11) {
        const int slot = std::atoi(argv[3]);
        const int cnt  = std::atoi(argv[10]);
        for (int i = 0; i < cnt; ++i) {
            InputPacket p{};
            fill_header(p.header, PT_INPUT, (uint8_t)slot, (uint8_t)i);
            p.payload.left_stick_x  = (int16_t)std::atoi(argv[4]);
            p.payload.left_stick_y  = (int16_t)std::atoi(argv[5]);
            p.payload.right_stick_x = (int16_t)std::atoi(argv[6]);
            p.payload.right_stick_y = (int16_t)std::atoi(argv[7]);
            p.payload.left_trigger  = (uint8_t)std::atoi(argv[8]);
            p.payload.right_trigger = (uint8_t)std::atoi(argv[9]);
            encode_input(p);
            send_raw(&p, sizeof(p));
            std::this_thread::sleep_for(std::chrono::milliseconds(8));  // ~120 Hz
        }
        std::printf("sent %d input packets to slot %d\n", cnt, slot);
        return 0;
    }

    if (std::strcmp(cmd, "disconnect") == 0 && argc == 4) {
        DisconnectPacket d{};
        fill_header(d.header, PT_DISCONNECT, (uint8_t)std::atoi(argv[3]), 0);
        for (int i = 0; i < 3; ++i) send_raw(&d, sizeof(d));   // x3, as the real sender does
        std::printf("sent 3 disconnect packets for slot %s\n", argv[3]);
        return 0;
    }

    if (std::strcmp(cmd, "latency") == 0) {
        const int cnt = (argc >= 4) ? std::atoi(argv[3]) : 10;
        double best = 1e9, worst = 0, sum = 0; int got = 0;
        for (int i = 0; i < cnt; ++i) {
            LatencyPacket p{};
            fill_header(p.header, PT_LATENCY, CONTROLLER_NONE, (uint8_t)i);
            const int64_t t0 = now_us();
            p.payload.t_us = (uint64_t)t0;
            encode_latency(p);
            send_raw(&p, sizeof(p));
            char buf[64]; sockaddr_in from{}; int fl = sizeof(from);
            const int n = recvfrom(s_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n == (int)sizeof(LatencyPacket)) {
                LatencyPacket e{}; std::memcpy(&e, buf, sizeof(e));
                decode_latency(e);
                const double one_way = (now_us() - (int64_t)e.payload.t_us) / 2000.0;  // ms
                best = one_way < best ? one_way : best;
                worst = one_way > worst ? one_way : worst;
                sum += one_way; ++got;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 10 Hz
        }
        if (got) std::printf("echoes %d/%d  one-way min/avg/max = %.3f / %.3f / %.3f ms\n",
                             got, cnt, best, sum/got, worst);
        else     std::printf("no echoes received (%d sent)\n", cnt);
        return got ? 0 : 1;
    }
    std::printf("bad arguments\n");
    return 2;
}
