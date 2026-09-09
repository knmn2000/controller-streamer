package org.controllerstreamer.sender;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Wire Protocol v3 - a byte-exact Java port of protocol/protocol.h.
 *
 * protocol.h is the single source of truth. This file must agree with it on
 * every byte or the two ends silently disagree. The C++ side has static_asserts
 * for that; Java has none, so main() below checks the same wire vectors that
 * tests/protocol_test.cpp asserts. Run it on the desktop JVM with:
 *
 *     java -cp classes org.controllerstreamer.sender.Protocol
 *
 * All multi-byte fields are big-endian (network order), which is ByteBuffer's
 * default and matches htons/ntohs on the C++ side.
 */
public final class Protocol {

    public static final int  MAGIC   = 0x4D58;   // "MX"
    public static final byte VERSION = 0x03;
    public static final int  PORT    = 47800;

    public static final byte PT_INPUT      = 0x01;
    public static final byte PT_RUMBLE     = 0x02;
    public static final byte PT_HEARTBEAT  = 0x03;
    public static final byte PT_DISCONNECT = 0x04;
    public static final byte PT_LATENCY    = 0x05;
    public static final byte PT_DISCOVER       = 0x06;
    public static final byte PT_DISCOVER_REPLY = 0x07;

    public static final byte CONTROLLER_NONE = (byte) 0xFF;

    // Button bits equal ViGEm's XUSB_BUTTON values so the receiver copies the
    // mask straight into XUSB_REPORT.wButtons (TRD 5.3). Do not renumber.
    public static final int BTN_DPAD_UP        = 0x0001;
    public static final int BTN_DPAD_DOWN      = 0x0002;
    public static final int BTN_DPAD_LEFT      = 0x0004;
    public static final int BTN_DPAD_RIGHT     = 0x0008;
    public static final int BTN_START          = 0x0010;
    public static final int BTN_BACK           = 0x0020;
    public static final int BTN_LEFT_THUMB     = 0x0040;
    public static final int BTN_RIGHT_THUMB    = 0x0080;
    public static final int BTN_LEFT_SHOULDER  = 0x0100;
    public static final int BTN_RIGHT_SHOULDER = 0x0200;
    public static final int BTN_GUIDE          = 0x0400;
    public static final int BTN_A              = 0x1000;
    public static final int BTN_B              = 0x2000;
    public static final int BTN_X              = 0x4000;
    public static final int BTN_Y              = 0x8000;

    public static final int SIZE_HEADER     = 6;
    public static final int SIZE_INPUT      = 18;
    public static final int SIZE_RUMBLE     = 8;
    public static final int SIZE_HEARTBEAT  = 6;
    public static final int SIZE_DISCONNECT = 6;
    public static final int SIZE_LATENCY    = 14;
    public static final int SIZE_DISCOVER       = 6;
    public static final int SIZE_DISCOVER_REPLY = 38;

    private Protocol() {}

    private static ByteBuffer buf(int n) {
        return ByteBuffer.allocate(n).order(ByteOrder.BIG_ENDIAN);
    }

    private static void header(ByteBuffer b, byte type, byte controllerId, byte seq) {
        b.putShort((short) MAGIC);
        b.put(VERSION);
        b.put(type);
        b.put(controllerId);
        b.put(seq);
    }

    /** 18-byte input packet. Sticks must already be XInput-oriented (+Y up). */
    public static byte[] input(int slot, byte seq, int buttons,
                               short lx, short ly, short rx, short ry,
                               int leftTrigger, int rightTrigger) {
        ByteBuffer b = buf(SIZE_INPUT);
        header(b, PT_INPUT, (byte) slot, seq);
        b.putShort((short) buttons);
        b.putShort(lx);
        b.putShort(ly);
        b.putShort(rx);
        b.putShort(ry);
        b.put((byte) leftTrigger);
        b.put((byte) rightTrigger);
        return b.array();
    }

    public static byte[] heartbeat(byte seq) {
        ByteBuffer b = buf(SIZE_HEARTBEAT);
        header(b, PT_HEARTBEAT, CONTROLLER_NONE, seq);
        return b.array();
    }

    public static byte[] disconnect(int slot, byte seq) {
        ByteBuffer b = buf(SIZE_DISCONNECT);
        header(b, PT_DISCONNECT, (byte) slot, seq);
        return b.array();
    }

    /** Echoed back verbatim by the receiver - drives both latency and discovery. */
    public static byte[] latency(byte seq, long tUs) {
        ByteBuffer b = buf(SIZE_LATENCY);
        header(b, PT_LATENCY, CONTROLLER_NONE, seq);
        b.putLong(tUs);
        return b.array();
    }

    public static boolean headerValid(byte[] p, int len) {
        if (len < SIZE_HEADER) return false;
        int magic = ((p[0] & 0xFF) << 8) | (p[1] & 0xFF);
        return magic == MAGIC && p[2] == VERSION;
    }

    public static byte packetType(byte[] p)   { return p[3]; }
    public static int  controllerId(byte[] p) { return p[4] & 0xFF; }

    public static long latencyTUs(byte[] p) {
        return ByteBuffer.wrap(p, SIZE_HEADER, 8).order(ByteOrder.BIG_ENDIAN).getLong();
    }

    public static int rumbleLarge(byte[] p) { return p[SIZE_HEADER]     & 0xFF; }
    public static int rumbleSmall(byte[] p) { return p[SIZE_HEADER + 1] & 0xFF; }

    /** 6-byte discovery probe; every receiver on the LAN answers one. */
    public static byte[] discover(byte seq) {
        ByteBuffer b = buf(SIZE_HEADER);
        header(b, PT_DISCOVER, CONTROLLER_NONE, seq);
        return b.array();
    }

    public static int discoverPadCount(byte[] p) { return p[SIZE_HEADER] & 0xFF; }

    /** Host name from a DiscoverReply. Not NUL-terminated; length-prefixed. */
    public static String discoverName(byte[] p, int len) {
        if (len < SIZE_DISCOVER_REPLY) return "";
        int n = p[SIZE_HEADER + 1] & 0xFF;
        if (n > 30) n = 30;
        return new String(p, SIZE_HEADER + 2, n, java.nio.charset.Charset.forName("US-ASCII"));
    }

    // ---- mapping helpers (mirror protocol.h; see TRD C2/C3) -----------------

    /**
     * Android axis (-1..+1, where +Y means DOWN) to XInput short (+Y means UP).
     * Negating the float before rounding means we can never emit -32768, so
     * protocol.h's INT16_MIN overflow special case cannot arise on this path.
     */
    public static short axisToShort(float v, boolean invert) {
        float f = invert ? -v : v;
        if (f >  1f) f =  1f;
        if (f < -1f) f = -1f;
        return (short) Math.round(f * 32767f);
    }

    /** Android trigger 0..1 to XInput 0..255. */
    public static int triggerToByte(float v) {
        if (v <= 0f) return 0;
        if (v >= 1f) return 255;
        return Math.round(v * 255f);
    }

    // ---- self test: the same wire vectors tests/protocol_test.cpp asserts ---

    private static int failures = 0;

    private static void check(boolean ok, String what) {
        if (!ok) { System.out.println("FAIL " + what); ++failures; }
    }

    private static String hex(byte[] b, int n) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < n; ++i) s.append(String.format("%02x ", b[i]));
        return s.toString().trim();
    }

    public static void main(String[] args) {
        check(input(0, (byte) 0, 0, (short) 0, (short) 0, (short) 0, (short) 0, 0, 0).length == 18,
              "InputPacket == 18");
        check(heartbeat((byte) 0).length == 6, "HeartbeatPacket == 6");
        check(disconnect(0, (byte) 0).length == 6, "DisconnectPacket == 6");
        check(latency((byte) 0, 0L).length == 14, "LatencyPacket == 14");

        // exact vector from protocol_test.cpp::test_sizes_and_wire_bytes
        byte[] p = input(0, (byte) 42, BTN_A | BTN_DPAD_LEFT,
                         (short) 0, (short) 0, (short) 0, (short) 0, 0, 0);
        check(p[0] == 0x4D && p[1] == 0x58, "magic big-endian MX");
        check(p[2] == 0x03, "version");
        check(p[3] == 0x01, "type input");
        check(p[4] == 0x00 && p[5] == 42, "controller id and seq");
        check((p[6] & 0xFF) == 0x10 && (p[7] & 0xFF) == 0x04, "buttons 0x1004 big-endian");
        System.out.println("input wire head: " + hex(p, 8) + "    expect: 4d 58 03 01 00 2a 10 04");

        byte[] l = latency((byte) 0, 0x0102030405060708L);
        check((l[6] & 0xFF) == 0x01 && (l[13] & 0xFF) == 0x08, "t_us big-endian on the wire");
        check(latencyTUs(l) == 0x0102030405060708L, "t_us round trip");
        System.out.println("latency wire:    " + hex(l, 14));

        check(headerValid(p, 18), "valid header accepted");
        byte[] badVer = p.clone();
        badVer[2] = 0x02;
        check(!headerValid(badVer, 18), "v2 talker rejected");
        byte[] badMagic = p.clone();
        badMagic[0] = 0x00;
        check(!headerValid(badMagic, 18), "bad magic rejected");

        check(axisToShort(0f, true) == 0, "axis centre reads 0");
        check(axisToShort(-1f, true) == 32767, "android up (-1) becomes XInput +32767");
        check(axisToShort(1f, true) == -32767, "android down (+1) becomes XInput -32767");
        check(axisToShort(1f, false) == 32767, "android right (+1) becomes XInput +32767");
        check(axisToShort(-5f, true) == 32767, "out of range clamped");
        check(triggerToByte(0f) == 0, "trigger rests at 0");
        check(triggerToByte(1f) == 255, "trigger full scale 255");
        check(triggerToByte(-0.01f) == 0, "negative trigger clamped");

        if (failures == 0) {
            System.out.println("Protocol selfTest: ALL PASS");
        } else {
            System.out.println("Protocol selfTest: " + failures + " FAILURES");
            System.exit(1);
        }
    }
}
