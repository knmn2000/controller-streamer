// loopback_receiver.cpp — test harness, NOT shipped. Plays the PC's role on
// localhost so the real sender binary can be integration-tested end-to-end
// on one machine (Milestone 4 shape): binds 47800, validates every packet,
// echoes latency probes, and prints a summary. Exits after --seconds N.
#include "protocol.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

using namespace proto;

int main(int argc, char** argv) {
    int seconds = (argc > 1) ? std::atoi(argv[1]) : 3;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    timeval tv{}; tv.tv_usec = 250000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    long n_input = 0, n_hb = 0, n_disc = 0, n_lat = 0, n_bad = 0, n_stale = 0;
    uint8_t last_seq[2] = {0,0}; bool have_seq[2] = {false,false};

    time_t t_end = time(nullptr) + seconds;
    char buf[64]; sockaddr_in src{}; socklen_t sl;
    while (time(nullptr) < t_end) {
        sl = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &sl);
        if (n < 0) continue;
        Header h; std::memcpy(&h, buf, sizeof(h));
        if ((size_t)n < sizeof(Header) || !header_valid(h)) { ++n_bad; continue; }
        switch (h.packet_type) {
            case PT_INPUT: {
                if ((size_t)n != sizeof(InputPacket) || h.controller_id >= 2) { ++n_bad; break; }
                int id = h.controller_id;
                if (have_seq[id] && !seq_newer(h.sequence_num, last_seq[id])) { ++n_stale; break; }
                last_seq[id] = h.sequence_num; have_seq[id] = true;
                ++n_input;
                break;
            }
            case PT_HEARTBEAT:  ++n_hb; break;
            case PT_DISCONNECT: ++n_disc; break;
            case PT_LATENCY:    ++n_lat;
                sendto(sock, buf, n, 0, (sockaddr*)&src, sl);   // echo
                break;
            default: ++n_bad; break;
        }
    }
    printf("summary: input=%ld heartbeat=%ld disconnect=%ld latency=%ld stale=%ld bad=%ld\n",
           n_input, n_hb, n_disc, n_lat, n_stale, n_bad);
    close(sock);
    // pass criteria: heartbeats arrived, latency probes arrived, nothing malformed
    return (n_hb >= 1 && n_lat >= 10 && n_bad == 0) ? 0 : 1;
}
