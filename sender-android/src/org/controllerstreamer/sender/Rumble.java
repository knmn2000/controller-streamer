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

import android.os.Build;
import android.os.CombinedVibration;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;

/**
 * Rumble output, richer than a single merged buzz.
 *
 * The protocol carries the two XInput motors separately (large = the heavy
 * low-frequency one, small = the light high-frequency one), and a DualSense has
 * two physical motors. The first version collapsed them with max(large, small)
 * and drove one vibrator, which threw away half the signal: a game rumbling
 * only the light motor felt identical to one rumbling only the heavy motor.
 *
 * When the pad exposes two or more vibrators this drives them independently via
 * CombinedVibration, so left/right and heavy/light survive the trip. Pads with
 * one vibrator fall back to the merged value, and pads without amplitude
 * control get on/off (the amplitude argument is simply ignored by the platform).
 *
 * Timing still follows the C8 strategy the Mac sender uses: apply for
 * RUMBLE_DURATION, top up every RUMBLE_REFRESH while packets keep arriving, and
 * force-stop once nothing has come for the expiry window. That survives a lost
 * (0,0) stop packet without stuttering under sustained rumble.
 */
final class Rumble {

    private Rumble() {}

    /** Human-readable capability summary, for the status screen. */
    static String describe(int deviceId) {
        InputDevice d = InputDevice.getDevice(deviceId);
        if (d == null) return "no device";
        if (Build.VERSION.SDK_INT < 31) {
            Vibrator v = legacyVibrator(d);
            return (v != null && v.hasVibrator()) ? "1 motor (legacy API)" : "none";
        }
        VibratorManager vm = d.getVibratorManager();
        if (vm == null) return "none";
        int[] ids = vm.getVibratorIds();
        if (ids.length == 0) return "none";
        StringBuilder s = new StringBuilder();
        s.append(ids.length).append(ids.length == 1 ? " motor" : " motors");
        Vibrator first = vm.getVibrator(ids[0]);
        if (first != null) s.append(first.hasAmplitudeControl()
                ? ", amplitude control" : ", on/off only");
        return s.toString();
    }

    @SuppressWarnings("deprecation")
    private static Vibrator legacyVibrator(InputDevice d) {
        return d.getVibrator();
    }

    static void apply(Streamer st, long now, long expiryMs) {
        for (int i = 0; i < Streamer.SLOTS; ++i) {
            Streamer.Pad p = st.pads[i];
            if (p.deviceId == -1) continue;

            final boolean expired = (now - p.rumbleAtMs) > expiryMs;
            final int large = expired ? 0 : p.rumbleLarge;
            final int small = expired ? 0 : p.rumbleSmall;

            if (large == 0 && small == 0) {
                if (p.rumbleAppliedLarge != 0 || p.rumbleAppliedSmall != 0) {
                    cancel(p.deviceId);
                    p.rumbleAppliedLarge = 0;
                    p.rumbleAppliedSmall = 0;
                }
                continue;
            }
            final boolean changed = large != p.rumbleAppliedLarge || small != p.rumbleAppliedSmall;
            final boolean stale   = (now - p.rumbleAppliedMs) >= Streamer.RUMBLE_REFRESH;
            if (changed || stale) {
                drive(p.deviceId, large, small);
                p.rumbleAppliedLarge = large;
                p.rumbleAppliedSmall = small;
                p.rumbleAppliedMs = now;
            }
        }
    }

    private static int amp(int motorByte) {
        // XInput motor byte 0..255 maps straight onto Android amplitude 1..255.
        // Zero would mean "default strength", not "off", so clamp up to 1 and
        // let the caller decide when to cancel instead.
        if (motorByte <= 0) return 0;
        return Math.min(255, Math.max(1, motorByte));
    }

    private static void drive(int deviceId, int large, int small) {
        InputDevice d = InputDevice.getDevice(deviceId);
        if (d == null) return;
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                driveModern(d, large, small);
            } else {
                Vibrator v = legacyVibrator(d);
                if (v == null || !v.hasVibrator()) return;
                int merged = amp(Math.max(large, small));
                if (merged == 0) { v.cancel(); return; }
                v.vibrate(VibrationEffect.createOneShot(Streamer.RUMBLE_DURATION, merged));
            }
        } catch (Exception ignored) { }
    }

    private static void driveModern(InputDevice d, int large, int small) {
        VibratorManager vm = d.getVibratorManager();
        if (vm == null) return;
        int[] ids = vm.getVibratorIds();
        if (ids.length == 0) return;

        if (ids.length == 1) {
            Vibrator v = vm.getVibrator(ids[0]);
            if (v == null || !v.hasVibrator()) return;
            int merged = amp(Math.max(large, small));
            if (merged == 0) { v.cancel(); return; }
            v.vibrate(VibrationEffect.createOneShot(Streamer.RUMBLE_DURATION, merged));
            return;
        }

        // Two or more motors: drive them separately. A motor asked for zero
        // still needs an entry, otherwise it would keep its previous effect
        // running - so give it a 1 ms nudge, which reads as "stop".
        CombinedVibration.ParallelCombination combo = CombinedVibration.startParallel();
        int[] wanted = { amp(large), amp(small) };
        boolean any = false;
        for (int i = 0; i < ids.length; ++i) {
            int w = (i < wanted.length) ? wanted[i] : 0;
            VibrationEffect e = (w == 0)
                    ? VibrationEffect.createOneShot(1, 1)
                    : VibrationEffect.createOneShot(Streamer.RUMBLE_DURATION, w);
            combo.addVibrator(ids[i], e);
            if (w != 0) any = true;
        }
        if (!any) { vm.cancel(); return; }
        vm.vibrate(combo.combine());
    }

    private static void cancel(int deviceId) {
        InputDevice d = InputDevice.getDevice(deviceId);
        if (d == null) return;
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = d.getVibratorManager();
                if (vm != null) { vm.cancel(); return; }
            }
            Vibrator v = legacyVibrator(d);
            if (v != null) v.cancel();
        } catch (Exception ignored) { }
    }
}
