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
package org.controllerstreamer.sender;

import android.content.Context;
import android.os.Build;
import android.os.PowerManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * All streaming state and threads, deliberately NOT owned by the Activity.
 *
 * The first version ran the network loop from Activity onStart/onStop, so
 * backgrounding the app stopped streaming outright. Everything lives here now
 * as a process-wide singleton so it survives the UI going away, and input can
 * arrive from either of two places:
 *
 *   MainActivity            - only while it holds window focus
 *   PadAccessibilityService - globally, regardless of focus (the real fix)
 *
 * Both call the same feedKey/feedMotion, so whichever is available wins and
 * having both active is harmless.
 */
public final class Streamer {

    public static final int SLOTS = 2;

    private static final long SEND_PERIOD_MS   = 8;    // ~120 Hz, matches the Mac
    private static final long HEARTBEAT_MS     = 1000; // 1 Hz (TRD 5.3)
    private static final long PROBE_MS         = 1000;
    private static final long RUMBLE_EXPIRY_MS = 600;  // force-stop, mirrors the Mac (C8)
    static final long RUMBLE_DURATION  = 800;
    static final long RUMBLE_REFRESH   = 400;

    /** Live state for one pad. Written by an input source, read by the sender. */
    public static final class Pad {
        public volatile int deviceId = -1;
        public volatile String name = "";
        public volatile int buttons = 0;
        public volatile short lx, ly, rx, ry;
        public volatile int lt, rt;
        byte seq = 0;
        // Which MotionEvent axes this pad uses, resolved per device by
        // chooseAxes(): a DualSense reports the right stick on RX/RY with L2/R2
        // on Z/RZ, while many Android pads are the other way round.
        public volatile int axRX = -1, axRY = -1, axLT = -1, axRT = -1;
        public volatile String axisNote = "";
        // rumble, split per motor so both can be driven independently
        public volatile int rumbleLarge = 0, rumbleSmall = 0;
        public volatile long rumbleAtMs = 0;
        long rumbleAppliedMs = 0;
        int  rumbleAppliedLarge = -1, rumbleAppliedSmall = -1;
    }

    private static final Streamer INSTANCE = new Streamer();
    public static Streamer get() { return INSTANCE; }
    private Streamer() { for (int i = 0; i < SLOTS; ++i) pads[i] = new Pad(); }

    public final Pad[] pads = new Pad[SLOTS];

    private final AtomicBoolean running = new AtomicBoolean(false);
    private DatagramSocket sock;
    private PowerManager.WakeLock wakeLock;
    private Context appContext;

    public volatile InetAddress pcAddr;
    public volatile String pcName = "";
    public volatile String manualIp = "";
    public volatile long lastEchoRttUs = -1;
    public volatile long packetsSent = 0;
    public volatile String note = "";
    public volatile boolean accessibilityConnected = false;

    public boolean isRunning() { return running.get(); }

    public int activePads() {
        int n = 0;
        for (int i = 0; i < SLOTS; ++i) if (pads[i].deviceId != -1) ++n;
        return n;
    }

    // ------------------------------------------------------------ lifecycle

    public synchronized void start(Context ctx) {
        appContext = ctx.getApplicationContext();
        if (running.getAndSet(true)) return;

        // A partial wake lock keeps the CPU scheduled with the screen off, so
        // the 120 Hz send loop is not throttled by Doze. It does NOT keep the
        // screen on and does not by itself make input arrive while locked.
        try {
            PowerManager pm = (PowerManager) appContext.getSystemService(Context.POWER_SERVICE);
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "padstream:stream");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
        } catch (Exception ignored) { }

        scanExistingDevices();

        Thread t = new Thread(new Runnable() { @Override public void run() { netLoop(); } },
                              "pad-net");
        t.setPriority(Thread.MAX_PRIORITY);
        t.start();
    }

    public synchronized void stop() {
        running.set(false);
        DatagramSocket s = sock;
        if (s != null) s.close();
        if (wakeLock != null && wakeLock.isHeld()) {
            try { wakeLock.release(); } catch (Exception ignored) { }
        }
        wakeLock = null;
    }

    // -------------------------------------------------- device to slot map

    public void scanExistingDevices() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (isGamepad(d)) assignSlot(d);
        }
    }

    public static boolean isGamepad(InputDevice d) {
        if (d == null) return false;
        int s = d.getSources();
        boolean joystick = (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        boolean gamepad  = (s & InputDevice.SOURCE_GAMEPAD)  == InputDevice.SOURCE_GAMEPAD;
        return (joystick || gamepad) && !d.isVirtual();
    }

    /** Lowest free slot wins, the same rule as the Mac sender. */
    public synchronized int assignSlot(InputDevice d) {
        for (int i = 0; i < SLOTS; ++i) if (pads[i].deviceId == d.getId()) return i;
        for (int i = 0; i < SLOTS; ++i) {
            if (pads[i].deviceId == -1) {
                pads[i].deviceId = d.getId();
                pads[i].name = String.valueOf(d.getName());
                chooseAxes(d, pads[i]);
                note = "slot " + i + " <- " + pads[i].name;
                return i;
            }
        }
        note = "third controller ignored: " + d.getName();
        return -1;
    }

    public int slotOf(int deviceId) {
        for (int i = 0; i < SLOTS; ++i) if (pads[i].deviceId == deviceId) return i;
        return -1;
    }

    public synchronized void releaseDevice(int deviceId) {
        int slot = slotOf(deviceId);
        if (slot < 0) return;
        Pad p = pads[slot];
        sendDisconnect(slot, p);
        p.deviceId = -1;
        p.name = "";
        p.buttons = 0;
        p.lx = p.ly = p.rx = p.ry = 0;
        p.lt = p.rt = 0;
        p.rumbleLarge = p.rumbleSmall = 0;
        note = "slot " + slot + " removed";
    }

    private static InputDevice.MotionRange rangeOf(InputDevice d, int axis) {
        InputDevice.MotionRange r = d.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK);
        return (r != null) ? r : d.getMotionRange(axis, InputDevice.SOURCE_GAMEPAD);
    }

    /** A stick axis swings negative; a trigger axis rests at zero. */
    private static boolean isStickAxis(InputDevice d, int axis) {
        InputDevice.MotionRange r = rangeOf(d, axis);
        return r != null && r.getMin() < -0.5f;
    }

    private static boolean isTriggerAxis(InputDevice d, int axis) {
        InputDevice.MotionRange r = rangeOf(d, axis);
        return r != null && r.getMin() > -0.5f && r.getMax() > 0.5f;
    }

    private static int firstTrigger(InputDevice d, int[] candidates) {
        for (int a : candidates) if (isTriggerAxis(d, a)) return a;
        return -1;
    }

    /**
     * Resolve this pad's axes from the ranges it reports rather than assuming a
     * layout. Polarity is the discriminator: sticks report roughly [-1,1] and
     * triggers report [0,1], so the same axis id can be told apart per pad.
     */
    public static void chooseAxes(InputDevice d, Pad p) {
        p.axLT = firstTrigger(d, new int[]{
                MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_Z });
        p.axRT = firstTrigger(d, new int[]{
                MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS, MotionEvent.AXIS_RZ });

        int[] candidates = { MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
                             MotionEvent.AXIS_RX, MotionEvent.AXIS_RY };
        int found = 0;
        for (int a : candidates) {
            if (a == p.axLT || a == p.axRT) continue;
            if (!isStickAxis(d, a)) continue;
            if (found == 0)      { p.axRX = a; ++found; }
            else if (found == 1) { p.axRY = a; ++found; break; }
        }
        p.axisNote = "RX=" + axisName(p.axRX) + " RY=" + axisName(p.axRY)
                   + " LT=" + axisName(p.axLT) + " RT=" + axisName(p.axRT);
    }

    public static String axisName(int a) {
        if (a < 0) return "-";
        switch (a) {
            case MotionEvent.AXIS_X:        return "X";
            case MotionEvent.AXIS_Y:        return "Y";
            case MotionEvent.AXIS_Z:        return "Z";
            case MotionEvent.AXIS_RX:       return "RX";
            case MotionEvent.AXIS_RY:       return "RY";
            case MotionEvent.AXIS_RZ:       return "RZ";
            case MotionEvent.AXIS_LTRIGGER: return "LTRIG";
            case MotionEvent.AXIS_RTRIGGER: return "RTRIG";
            case MotionEvent.AXIS_BRAKE:    return "BRAKE";
            case MotionEvent.AXIS_GAS:      return "GAS";
            default:                        return "ax" + a;
        }
    }

    // ------------------------------------------------------- input intake

    /** Returns true if the event belonged to a tracked pad. */
    public boolean feedMotion(MotionEvent ev) {
        if ((ev.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) return false;
        if (ev.getAction() != MotionEvent.ACTION_MOVE) return false;
        InputDevice d = ev.getDevice();
        int slot = (d != null) ? assignSlot(d) : slotOf(ev.getDeviceId());
        if (slot < 0) return false;
        Pad p = pads[slot];

        // Android: +Y is DOWN. XInput: +Y is UP. Invert both vertical axes,
        // exactly as the Mac sender does via invert_axis.
        p.lx = Protocol.axisToShort(ev.getAxisValue(MotionEvent.AXIS_X), false);
        p.ly = Protocol.axisToShort(ev.getAxisValue(MotionEvent.AXIS_Y), true);
        if (p.axRX >= 0) p.rx = Protocol.axisToShort(ev.getAxisValue(p.axRX), false);
        if (p.axRY >= 0) p.ry = Protocol.axisToShort(ev.getAxisValue(p.axRY), true);
        if (p.axLT >= 0) p.lt = Protocol.triggerToByte(ev.getAxisValue(p.axLT));
        if (p.axRT >= 0) p.rt = Protocol.triggerToByte(ev.getAxisValue(p.axRT));

        float hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);
        int b = p.buttons & ~(Protocol.BTN_DPAD_UP | Protocol.BTN_DPAD_DOWN
                            | Protocol.BTN_DPAD_LEFT | Protocol.BTN_DPAD_RIGHT);
        if (hx < -0.5f) b |= Protocol.BTN_DPAD_LEFT;
        if (hx >  0.5f) b |= Protocol.BTN_DPAD_RIGHT;
        if (hy < -0.5f) b |= Protocol.BTN_DPAD_UP;
        if (hy >  0.5f) b |= Protocol.BTN_DPAD_DOWN;
        p.buttons = b;
        return true;
    }

    private static int bitFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:      return Protocol.BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B:      return Protocol.BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X:      return Protocol.BTN_X;
            case KeyEvent.KEYCODE_BUTTON_Y:      return Protocol.BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_L1:     return Protocol.BTN_LEFT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1:     return Protocol.BTN_RIGHT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return Protocol.BTN_LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return Protocol.BTN_RIGHT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_START:  return Protocol.BTN_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return Protocol.BTN_BACK;
            case KeyEvent.KEYCODE_BUTTON_MODE:   return Protocol.BTN_GUIDE;
            case KeyEvent.KEYCODE_DPAD_UP:       return Protocol.BTN_DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:     return Protocol.BTN_DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:     return Protocol.BTN_DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:    return Protocol.BTN_DPAD_RIGHT;
            default: return 0;
        }
    }

    /** Returns true if the event belonged to a tracked pad. */
    public boolean feedKey(KeyEvent ev) {
        final int act = ev.getAction();
        if (act != KeyEvent.ACTION_DOWN && act != KeyEvent.ACTION_UP) return false;
        if ((ev.getSource() & InputDevice.SOURCE_GAMEPAD) == 0
                && (ev.getSource() & InputDevice.SOURCE_JOYSTICK) == 0) return false;
        InputDevice d = ev.getDevice();
        int slot = (d != null) ? assignSlot(d) : slotOf(ev.getDeviceId());
        if (slot < 0) return false;
        Pad p = pads[slot];
        final boolean down = (act == KeyEvent.ACTION_DOWN);
        final int keyCode = ev.getKeyCode();

        // Pads without analog trigger axes report L2/R2 as buttons instead.
        if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) { p.lt = down ? 255 : 0; return true; }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) { p.rt = down ? 255 : 0; return true; }

        int bit = bitFor(keyCode);
        if (bit == 0) return false;
        if (down) p.buttons |= bit; else p.buttons &= ~bit;
        return true;
    }

    // -------------------------------------------------------------- network

    private void netLoop() {
        try {
            sock = new DatagramSocket();
            sock.setBroadcast(true);
            sock.setSoTimeout(250);
        } catch (Exception e) {
            note = "socket failed: " + e;
            running.set(false);
            return;
        }

        Thread rx = new Thread(new Runnable() { @Override public void run() { rxLoop(); } },
                               "pad-rx");
        rx.start();

        long lastBeat = 0, lastProbe = 0;
        byte beatSeq = 0, probeSeq = 0;

        while (running.get()) {
            long now = System.currentTimeMillis();

            if (pcAddr == null && !manualIp.isEmpty()) {
                try { pcAddr = InetAddress.getByName(manualIp); } catch (Exception ignored) { }
            }

            if (now - lastProbe >= PROBE_MS) {
                lastProbe = now;
                if (pcAddr == null) {
                    // PT_DISCOVER also returns the receiver's host name. The
                    // latency broadcast beside it is a fallback for a receiver
                    // built before discovery existed: it echoes PT_LATENCY
                    // verbatim, which still reveals its address.
                    broadcast(Protocol.discover(probeSeq));
                    broadcast(Protocol.latency(probeSeq++, System.nanoTime() / 1000L));
                } else {
                    send(Protocol.latency(probeSeq++, System.nanoTime() / 1000L));
                }
            }

            if (pcAddr != null) {
                for (int i = 0; i < SLOTS; ++i) {
                    Pad p = pads[i];
                    if (p.deviceId == -1) continue;
                    send(Protocol.input(i, p.seq++, p.buttons, p.lx, p.ly, p.rx, p.ry, p.lt, p.rt));
                }
                if (now - lastBeat >= HEARTBEAT_MS) {
                    lastBeat = now;
                    send(Protocol.heartbeat(beatSeq++));
                }
            }

            Rumble.apply(this, now, RUMBLE_EXPIRY_MS);

            try { Thread.sleep(SEND_PERIOD_MS); } catch (InterruptedException e) { break; }
        }

        for (int i = 0; i < SLOTS; ++i)
            if (pads[i].deviceId != -1) sendDisconnect(i, pads[i]);
        DatagramSocket s = sock;
        if (s != null) s.close();
    }

    private void rxLoop() {
        byte[] b = new byte[64];
        while (running.get()) {
            DatagramPacket dp = new DatagramPacket(b, b.length);
            try { sock.receive(dp); } catch (Exception e) { continue; }
            int n = dp.getLength();
            if (!Protocol.headerValid(b, n)) continue;

            switch (Protocol.packetType(b)) {
                case Protocol.PT_DISCOVER_REPLY:
                    if (pcAddr == null) pcAddr = dp.getAddress();
                    pcName = Protocol.discoverName(b, n);
                    break;
                case Protocol.PT_LATENCY:
                    if (pcAddr == null) pcAddr = dp.getAddress();
                    if (n >= Protocol.SIZE_LATENCY)
                        lastEchoRttUs = (System.nanoTime() / 1000L) - Protocol.latencyTUs(b);
                    break;
                case Protocol.PT_RUMBLE: {
                    int slot = Protocol.controllerId(b);
                    if (slot < SLOTS && n >= Protocol.SIZE_RUMBLE) {
                        pads[slot].rumbleLarge = Protocol.rumbleLarge(b);
                        pads[slot].rumbleSmall = Protocol.rumbleSmall(b);
                        pads[slot].rumbleAtMs = System.currentTimeMillis();
                    }
                    break;
                }
                default: break;
            }
        }
    }

    private void send(byte[] data) {
        InetAddress a = pcAddr;
        DatagramSocket s = sock;
        if (a == null || s == null || s.isClosed()) return;
        try {
            s.send(new DatagramPacket(data, data.length, a, Protocol.PORT));
            ++packetsSent;
        } catch (Exception ignored) { }
    }

    /** Probe every interface broadcast address, plus the global one. */
    private void broadcast(byte[] data) {
        DatagramSocket s = sock;
        if (s == null || s.isClosed()) return;
        List<InetAddress> targets = new ArrayList<InetAddress>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress bc = ia.getBroadcast();
                    if (bc != null) targets.add(bc);
                }
            }
        } catch (Exception ignored) { }
        try { targets.add(InetAddress.getByName("255.255.255.255")); } catch (Exception ignored) { }
        for (InetAddress t : targets) {
            try { s.send(new DatagramPacket(data, data.length, t, Protocol.PORT)); }
            catch (Exception ignored) { }
        }
    }

    private void sendDisconnect(int slot, Pad p) {
        for (int i = 0; i < 3; ++i) send(Protocol.disconnect(slot, p.seq++));
    }
}
