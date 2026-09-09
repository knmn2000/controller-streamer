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

import android.accessibilityservice.AccessibilityService;
import android.os.Build;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.accessibility.AccessibilityEvent;

/**
 * Global gamepad capture.
 *
 * Android delivers gamepad KeyEvents and MotionEvents only to the window that
 * currently has focus. That is why streaming stopped the moment the
 * notification shade opened or another app came forward - an Activity simply
 * cannot see the pad once it loses focus, and no service or wake lock changes
 * that. An accessibility service is the one public mechanism that receives
 * input events ahead of window dispatch, so it keeps working regardless of
 * what is in front.
 *
 *   onKeyEvent    - buttons. Needs canRequestFilterKeyEvents in the config.
 *   onMotionEvent - analog sticks and triggers. Added in Android 14 (API 34),
 *                   and only for the sources named in motionEventSources.
 *
 * Every event is passed through untouched (we return false) so enabling this
 * never swallows input from anything else on the phone.
 *
 * The user has to enable this by hand in Settings > Accessibility; there is no
 * way to grant it programmatically, by design.
 */
public class PadAccessibilityService extends AccessibilityService {

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();

        // Ask for what we actually need, at runtime. Key filtering is also
        // declared in the config XML, but motion event sources cannot be:
        // android:motionEventSources is not an XML attribute in android-35, so
        // joystick motion has to be requested here. API 34+ only - on older
        // Android this service still delivers buttons, just not sticks.
        try {
            android.accessibilityservice.AccessibilityServiceInfo info = getServiceInfo();
            if (info != null) {
                info.flags |= android.accessibilityservice
                        .AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS;
                if (Build.VERSION.SDK_INT >= 34) setJoystickSources(info);
                setServiceInfo(info);
            }
        } catch (Exception ignored) { }

        Streamer st = Streamer.get();
        st.accessibilityConnected = true;
        st.note = "global capture active";
        st.scanExistingDevices();
        // Streaming may not have been started by the Activity yet - for
        // instance after a reboot with autostart, when the UI was never opened.
        StreamService.start(this);
    }

    /**
     * Isolated so ART never has to verify a setMotionEventSources reference on
     * a device below API 34, where the method does not exist.
     */
    private static void setJoystickSources(
            android.accessibilityservice.AccessibilityServiceInfo info) {
        info.setMotionEventSources(android.view.InputDevice.SOURCE_JOYSTICK);
    }

    @Override
    public boolean onKeyEvent(KeyEvent event) {
        Streamer.get().feedKey(event);
        return false;                 // never consume; let the system have it too
    }

    /**
     * API 34+, requested via setMotionEventSources() in onServiceConnected.
     * Returns void - motion events can only be observed here, never consumed,
     * which is exactly what we want: nothing else on the phone is affected.
     */
    @Override
    public void onMotionEvent(MotionEvent event) {
        Streamer.get().feedMotion(event);
    }

    @Override public void onAccessibilityEvent(AccessibilityEvent event) { }

    @Override public void onInterrupt() { }

    @Override
    public boolean onUnbind(android.content.Intent intent) {
        Streamer st = Streamer.get();
        st.accessibilityConnected = false;
        st.note = "global capture disabled";
        return super.onUnbind(intent);
    }
}
