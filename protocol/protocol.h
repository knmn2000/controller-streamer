// protocol.h — Wire Protocol v3 (Study TRD v3.0, section 5).
// Compiled into BOTH binaries. This file is the single source of truth;
// never copy it, always share it.
#pragma once

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>   // htons/ntohs
#else
  #include <arpa/inet.h>  // htons/ntohs
#endif

namespace proto {

constexpr uint16_t MAGIC    = 0x4D58;  // "MX"
constexpr uint8_t  VERSION  = 0x03;
constexpr uint16_t PORT     = 47800;   // receiver's fixed UDP port (ADR-2)

enum PacketType : uint8_t {
    PT_INPUT        = 0x01,
    PT_RUMBLE       = 0x02,
    PT_HEARTBEAT    = 0x03,
    PT_DISCONNECT   = 0x04,
    PT_LATENCY      = 0x05,  // debug: sender timestamp, echoed verbatim by receiver (M9)
    // Added after v3 shipped. Purely additive: VERSION stays 3, no existing
    // struct changed, and both ends already ignore packet types they do not
    // know, so a sender built before these existed still interoperates.
    PT_DISCOVER       = 0x06,  // sender broadcasts this to find a receiver
    PT_DISCOVER_REPLY = 0x07,  // receiver answers with its name and pad count
};

constexpr uint8_t CONTROLLER_NONE = 0xFF;  // header.controller_id for non-slot packets

// Button bits are chosen to EQUAL ViGEm's XUSB_BUTTON values (TRD 5.3),
// so the receiver copies the mask into XUSB_REPORT.wButtons unchanged.
enum Button : uint16_t {
    BTN_DPAD_UP        = 0x0001,
    BTN_DPAD_DOWN      = 0x0002,
    BTN_DPAD_LEFT      = 0x0004,
    BTN_DPAD_RIGHT     = 0x0008,
    BTN_START          = 0x0010,
    BTN_BACK           = 0x0020,
    BTN_LEFT_THUMB     = 0x0040,
    BTN_RIGHT_THUMB    = 0x0080,
    BTN_LEFT_SHOULDER  = 0x0100,
    BTN_RIGHT_SHOULDER = 0x0200,
    BTN_GUIDE          = 0x0400,
    BTN_A              = 0x1000,
    BTN_B              = 0x2000,
    BTN_X              = 0x4000,
    BTN_Y              = 0x8000,
};

#pragma pack(push, 1)

struct Header {
    uint16_t magic;          // network order on the wire
    uint8_t  version;
    uint8_t  packet_type;
    uint8_t  controller_id;  // 0,1 or CONTROLLER_NONE
    uint8_t  sequence_num;   // per-controller, per-direction (TRD 5.5)
};

struct InputPayload {
    uint16_t buttons;        // Button bitmask
    int16_t  left_stick_x;   // XInput orientation (Y already inverted by sender)
    int16_t  left_stick_y;
    int16_t  right_stick_x;
    int16_t  right_stick_y;
    uint8_t  left_trigger;   // 0..255
    uint8_t  right_trigger;
};

struct InputPacket     { Header header; InputPayload payload; };

struct RumblePayload   { uint8_t large_motor; uint8_t small_motor; };  // as ViGEm delivers, 0..255
struct RumblePacket    { Header header; RumblePayload payload; };

struct HeartbeatPacket { Header header; };
struct DisconnectPacket{ Header header; };

struct LatencyPayload  { uint64_t t_us; };  // sender steady_clock micros, echoed untouched
struct LatencyPacket   { Header header; LatencyPayload payload; };

// Discovery (added post-v3). The sender broadcasts a DiscoverPacket; every
// receiver on the LAN answers with a DiscoverReply, and the sender learns the
// address from the reply's source. name is NOT NUL-terminated - read exactly
// name_len bytes - so a 30-char host name still fits.
struct DiscoverPacket  { Header header; };
struct DiscoverReplyPayload {
    uint8_t pad_count;      // virtual pads this receiver presents
    uint8_t name_len;       // valid bytes in name, 0..30
    char    name[30];       // host name, not NUL-terminated
};
struct DiscoverReply   { Header header; DiscoverReplyPayload payload; };

#pragma pack(pop)

// Layout drift is a cross-platform bug you find in Wireshark at midnight.
// Catch it at compile time instead (TRD 4.3).
static_assert(sizeof(Header)           == 6,  "Header layout drifted");
static_assert(sizeof(InputPacket)      == 18, "InputPacket layout drifted");
static_assert(sizeof(RumblePacket)     == 8,  "RumblePacket layout drifted");
static_assert(sizeof(HeartbeatPacket)  == 6,  "HeartbeatPacket layout drifted");
static_assert(sizeof(DisconnectPacket) == 6,  "DisconnectPacket layout drifted");
static_assert(sizeof(LatencyPacket)    == 14, "LatencyPacket layout drifted");
static_assert(sizeof(DiscoverPacket)   == 6,  "DiscoverPacket layout drifted");
static_assert(sizeof(DiscoverReply)    == 38, "DiscoverReply layout drifted");

// ---- byte order -----------------------------------------------------------
// htons/ntohs operate on unsigned; int16_t goes through a uint16_t cast.
// Safe because two's-complement bit patterns survive the round trip (TRD C13).

inline uint16_t h2n_u16(uint16_t v) { return htons(v); }
inline uint16_t n2h_u16(uint16_t v) { return ntohs(v); }
inline int16_t  h2n_i16(int16_t v)  { return (int16_t)htons((uint16_t)v); }
inline int16_t  n2h_i16(int16_t v)  { return (int16_t)ntohs((uint16_t)v); }

inline uint64_t swap64(uint64_t v) {
    uint8_t b[8];
    std::memcpy(b, &v, 8);
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r = (r << 8) | b[i];
    return r;
}
inline bool host_is_little_endian() {
    const uint16_t probe = 1;
    uint8_t first;
    std::memcpy(&first, &probe, 1);
    return first == 1;
}
inline uint64_t h2n_u64(uint64_t v) { return host_is_little_endian() ? swap64(v) : v; }
inline uint64_t n2h_u64(uint64_t v) { return h2n_u64(v); }  // symmetric

// ---- header build / validate ----------------------------------------------

inline void fill_header(Header& h, PacketType t, uint8_t controller_id, uint8_t seq) {
    h.magic         = h2n_u16(MAGIC);
    h.version       = VERSION;
    h.packet_type   = (uint8_t)t;
    h.controller_id = controller_id;
    h.sequence_num  = seq;
}

// Validates magic + version on a received header (fields still in network order).
inline bool header_valid(const Header& h) {
    return n2h_u16(h.magic) == MAGIC && h.version == VERSION;
}

// ---- in-place encode/decode (multi-byte payload fields only) ---------------

inline void encode_input(InputPacket& p) {
    p.payload.buttons       = h2n_u16(p.payload.buttons);
    p.payload.left_stick_x  = h2n_i16(p.payload.left_stick_x);
    p.payload.left_stick_y  = h2n_i16(p.payload.left_stick_y);
    p.payload.right_stick_x = h2n_i16(p.payload.right_stick_x);
    p.payload.right_stick_y = h2n_i16(p.payload.right_stick_y);
}
inline void decode_input(InputPacket& p) {
    p.payload.buttons       = n2h_u16(p.payload.buttons);
    p.payload.left_stick_x  = n2h_i16(p.payload.left_stick_x);
    p.payload.left_stick_y  = n2h_i16(p.payload.left_stick_y);
    p.payload.right_stick_x = n2h_i16(p.payload.right_stick_x);
    p.payload.right_stick_y = n2h_i16(p.payload.right_stick_y);
}
inline void encode_latency(LatencyPacket& p) { p.payload.t_us = h2n_u64(p.payload.t_us); }
inline void decode_latency(LatencyPacket& p) { p.payload.t_us = n2h_u64(p.payload.t_us); }
// Rumble/Heartbeat/Disconnect payloads have no multi-byte fields.

// ---- sequence numbers (TRD 5.5, fixes C5) ----------------------------------
// Wraparound-aware "is `incoming` newer than `last`?" via RFC 1982 collapsed
// to one signed 8-bit subtraction: the uint8_t difference reinterpreted as
// int8_t is positive iff `incoming` is 1..127 steps ahead of `last`, across
// the 255->0 wrap. Exactly half the circle counts as "newer".
inline bool seq_newer(uint8_t incoming, uint8_t last) {
    return (int8_t)(uint8_t)(incoming - last) > 0;
}

// ---- axis / trigger / rumble mapping (TRD 2.x, C2, C3) ---------------------

// SDL Y axes: positive = down. XInput: positive = up. Negate — but
// -(-32768) does not fit in int16_t (signed overflow = UB), so clamp
// the single problem value first (C2).
inline int16_t invert_axis(int16_t v) {
    if (v == INT16_MIN) return INT16_MAX;
    return (int16_t)-v;
}

// SDL trigger 0..32767 -> XInput 0..255. 32767/128 = 255 exactly at the top.
// Some pads can report tiny negatives at rest; clamp low end too.
inline uint8_t trigger_to_byte(int16_t t) {
    if (t <= 0) return 0;
    int v = t / 128;
    return (uint8_t)(v > 255 ? 255 : v);
}

// XInput motor byte 0..255 -> SDL rumble 0..65535.
// *257 maps 0->0 and 255->65535 exactly (TRD 5.3).
inline uint16_t motor_to_sdl(uint8_t m) { return (uint16_t)(m * 257u); }

} // namespace proto
