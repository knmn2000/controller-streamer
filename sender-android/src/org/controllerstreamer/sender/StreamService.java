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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.InputDevice;

/**
 * Foreground service that owns the streaming lifetime.
 *
 * Two reasons this exists rather than running from the Activity:
 *
 *  1. The Activity's onStart/onStop used to start and stop the network loop, so
 *     backgrounding the app killed streaming outright.
 *  2. A plain background thread gets frozen by Doze and app-standby. A
 *     foreground service is exempt, which is what keeps the 120 Hz send loop
 *     and the rumble return path alive with the screen off.
 *
 * It also owns the InputDeviceListener, so a pad connecting or disconnecting is
 * noticed whether or not the UI is on screen.
 */
public class StreamService extends Service implements InputManager.InputDeviceListener {

    private static final String CHANNEL = "padstream";
    private static final int    NOTIF_ID = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private InputManager inputManager;

    /** Intent action for the notification's Stop button. */
    static final String ACTION_STOP = "org.controllerstreamer.sender.STOP";

    public static void start(Context ctx) {
        Intent i = new Intent(ctx, StreamService.class);
        if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i);
        else                             ctx.startService(i);
    }

    public static void stop(Context ctx) {
        ctx.stopService(new Intent(ctx, StreamService.class));
    }

    public static boolean isStreaming() { return Streamer.get().isRunning(); }

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
        startForegroundCompat();

        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, ui);

        Streamer.get().start(this);
        tick.run();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            // An explicit stop from the notification. stopSelf() also cancels
            // the START_STICKY restart, so this genuinely stays stopped -
            // unlike swiping the task away, which the system may undo.
            if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
            else                             stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;      // come back if the system reclaims us mid-game
    }

    @Override
    public void onDestroy() {
        ui.removeCallbacks(tick);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        Streamer.get().stop();
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    // ------------------------------------------------------- device changes

    @Override public void onInputDeviceAdded(int id) {
        InputDevice d = InputDevice.getDevice(id);
        if (Streamer.isGamepad(d)) Streamer.get().assignSlot(d);
    }

    @Override public void onInputDeviceChanged(int id) { }

    @Override public void onInputDeviceRemoved(int id) {
        Streamer.get().releaseDevice(id);
    }

    // ---------------------------------------------------------- notification

    private void createChannel() {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationChannel c = new NotificationChannel(
                CHANNEL, "Controller streaming", NotificationManager.IMPORTANCE_LOW);
        c.setShowBadge(false);
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        nm.createNotificationChannel(c);
    }

    private Notification buildNotification() {
        Streamer st = Streamer.get();
        String text;
        if (st.pcAddr == null) {
            text = "Searching for PC...";
        } else {
            String where = st.pcName.isEmpty() ? st.pcAddr.getHostAddress() : st.pcName;
            int n = st.activePads();
            text = (n == 0) ? ("Connected to " + where + ", no controller")
                            : ("Streaming to " + where + " - " + n
                               + (n == 1 ? " pad" : " pads"));
        }
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        // The notification is ONGOING/NO_CLEAR so it cannot be swiped away, and
        // swiping the task from Recents is not a reliable stop either. This
        // action is therefore the actual off switch.
        Intent stopIntent = new Intent(this, StreamService.class).setAction(ACTION_STOP);
        PendingIntent stop = PendingIntent.getService(this, 1, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder b = (Build.VERSION.SDK_INT >= 26)
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        return b.setContentTitle("Pad Streamer")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel),
                        "Stop", stop).build())
                .setOngoing(true)
                .build();
    }

    private void startForegroundCompat() {
        Notification n = buildNotification();
        if (Build.VERSION.SDK_INT >= 34) {
            // Android 14+ requires a declared type, and it must match the
            // manifest. SPECIAL_USE, not CONNECTED_DEVICE: the latter also
            // demands one of BLUETOOTH_CONNECT / CHANGE_WIFI_STATE / NFC /...,
            // none of which this app uses or should ask for. See the manifest
            // comment; the required justification property lives there too.
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIF_ID, n);
        }
    }

    /** Refresh the notification text so the shade reflects the live state. */
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            try {
                NotificationManager nm =
                        (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                nm.notify(NOTIF_ID, buildNotification());
            } catch (Exception ignored) { }
            ui.postDelayed(this, 2000);
        }
    };
}
