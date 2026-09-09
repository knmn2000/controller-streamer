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

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.text.TextUtils;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.net.InetAddress;

/**
 * Status screen and setup. Streaming itself lives in StreamService/Streamer, so
 * closing or backgrounding this screen no longer stops anything.
 *
 * This Activity still feeds input while it happens to be focused, which keeps
 * the app usable before the accessibility service is enabled. Once that is on,
 * both paths run and either one is sufficient.
 */
public class MainActivity extends Activity {

    private TextView headline;
    private TextView status;
    private EditText ipBox;
    private Button   accessBtn;
    private Button   powerBtn;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        // Keeps the screen alive while you are looking at this screen. It is
        // NOT what keeps streaming alive - the foreground service does that.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#101014"));
        final int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        root.setFocusableInTouchMode(true);      // stop the IP box taking focus

        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public android.view.WindowInsets onApplyWindowInsets(
                    View v, android.view.WindowInsets in) {
                int top = 0, bottom = 0, left = 0, right = 0;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets bars = in.getInsets(
                            android.view.WindowInsets.Type.systemBars()
                          | android.view.WindowInsets.Type.ime());
                    top = bars.top; bottom = bars.bottom; left = bars.left; right = bars.right;
                }
                v.setPadding(pad + left, pad + top, pad + right, pad + bottom);
                return in;
            }
        });

        TextView title = new TextView(this);
        title.setText("Pad Streamer");
        title.setTextColor(Color.WHITE);
        title.setTextSize(22);
        root.addView(title);

        headline = new TextView(this);
        headline.setTextSize(16);
        headline.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(headline);

        status = new TextView(this);
        status.setTextColor(Color.parseColor("#C8E6C9"));
        status.setTextSize(13);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        // Explicit on/off. Swiping the task from Recents is NOT a reliable stop
        // (START_STICKY can bring the service back), and the notification is
        // ONGOING so it cannot be swiped either - so there has to be a button.
        powerBtn = new Button(this);
        powerBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (Streamer.get().isRunning()) StreamService.stop(MainActivity.this);
                else                            StreamService.start(MainActivity.this);
            }
        });
        root.addView(powerBtn);

        accessBtn = new Button(this);
        accessBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                // No way to grant this programmatically - by design. Take the
                // user to the settings page and let them flip it.
                startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS));
            }
        });
        root.addView(accessBtn);

        TextView hint = new TextView(this);
        hint.setText("Auto-discovers the PC. Only fill this in if discovery fails:");
        hint.setTextColor(Color.parseColor("#9E9E9E"));
        hint.setTextSize(12);
        root.addView(hint);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        ipBox = new EditText(this);
        ipBox.setHint("192.168.1.10");
        ipBox.setInputType(InputType.TYPE_CLASS_TEXT);
        ipBox.setTextColor(Color.WHITE);
        ipBox.setHintTextColor(Color.parseColor("#616161"));
        row.addView(ipBox, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Button apply = new Button(this);
        apply.setText("Use");
        apply.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                Streamer st = Streamer.get();
                st.manualIp = ipBox.getText().toString().trim();
                st.pcAddr = null;
                st.note = st.manualIp.isEmpty() ? "cleared override" : "override: " + st.manualIp;
            }
        });
        row.addView(apply);
        root.addView(row);

        setContentView(root);

        if (Build.VERSION.SDK_INT >= 33) {
            // Needed only so the foreground-service notification is visible;
            // the service runs either way.
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 1);
        }
        StreamService.start(this);
    }

    @Override protected void onStart() {
        super.onStart();
        Streamer.get().scanExistingDevices();
        ui.post(refreshUi);
    }

    @Override protected void onStop() {
        ui.removeCallbacks(refreshUi);
        super.onStop();               // streaming deliberately keeps running
    }

    // ------------------------------------------------- fallback input intake

    // Dispatch level, not onKeyDown/onGenericMotionEvent: those only fire when
    // no focused view consumed the event first, and the IP text field was
    // swallowing every button press. Only useful while this window has focus -
    // PadAccessibilityService is what covers the rest.
    @Override public boolean dispatchKeyEvent(KeyEvent ev) {
        if (Streamer.get().feedKey(ev)) return true;
        return super.dispatchKeyEvent(ev);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        if (Streamer.get().feedMotion(ev)) return true;
        return super.dispatchGenericMotionEvent(ev);
    }

    // ------------------------------------------------------------------- ui

    private boolean accessibilityEnabled() {
        String flat = Settings.Secure.getString(getContentResolver(),
                Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
        if (TextUtils.isEmpty(flat)) return false;
        String me = new ComponentName(this, PadAccessibilityService.class).flattenToString();
        String meShort = getPackageName() + "/." + PadAccessibilityService.class.getSimpleName();
        for (String part : flat.split(":")) {
            if (part.equalsIgnoreCase(me) || part.equalsIgnoreCase(meShort)) return true;
        }
        return false;
    }

    private String describePc(Streamer st, InetAddress a) {
        return (st.pcName != null && !st.pcName.isEmpty()) ? st.pcName : a.getHostAddress();
    }

    private final Runnable refreshUi = new Runnable() {
        @Override public void run() {
            Streamer st = Streamer.get();
            InetAddress a = st.pcAddr;
            int active = st.activePads();
            final boolean global = accessibilityEnabled();

            if (a == null) {
                headline.setText("Searching for PC on the network...");
                headline.setTextColor(Color.parseColor("#FFB74D"));
            } else if (active == 0) {
                headline.setText("Connected to " + describePc(st, a) + "  -  no controller");
                headline.setTextColor(Color.parseColor("#FFB74D"));
            } else if (!global) {
                headline.setText("Streaming to " + describePc(st, a)
                        + "  -  only while this screen is open");
                headline.setTextColor(Color.parseColor("#FFB74D"));
            } else {
                headline.setText("Streaming to " + describePc(st, a)
                        + "  -  works in the background");
                headline.setTextColor(Color.parseColor("#81C784"));
            }

            powerBtn.setText(st.isRunning() ? "Stop streaming" : "Start streaming");
            accessBtn.setText(global
                    ? "Background capture: ON  (tap to review)"
                    : "Enable background capture");

            StringBuilder s = new StringBuilder();
            s.append(a == null
                    ? "PC:        searching (broadcast probe, 1 Hz)\n"
                    : "PC:        " + a.getHostAddress() + ":" + Protocol.PORT + "\n");
            s.append("sent:      ").append(st.packetsSent).append(" packets\n");
            long rtt = st.lastEchoRttUs;
            s.append("latency:   ").append(rtt < 0
                    ? "no echo yet"
                    : String.format("%.2f ms one-way (RTT/2)", rtt / 2000.0)).append('\n');
            s.append("service:   ").append(st.isRunning() ? "running" : "stopped").append('\n');
            s.append("global:    ").append(global
                    ? "on (accessibility)" : "OFF - stops when you leave").append('\n');
            s.append('\n');

            for (int i = 0; i < Streamer.SLOTS; ++i) {
                Streamer.Pad p = st.pads[i];
                if (p.deviceId == -1) {
                    s.append("slot ").append(i).append(":    empty\n");
                    continue;
                }
                s.append("slot ").append(i).append(":    ").append(p.name).append('\n');
                s.append("  axes ").append(p.axisNote).append('\n');
                s.append("  motors ").append(Rumble.describe(p.deviceId)).append('\n');
                s.append(String.format("  LX %6d LY %6d  RX %6d RY %6d%n",
                        p.lx, p.ly, p.rx, p.ry));
                s.append(String.format("  LT %3d RT %3d  btn 0x%04x%n", p.lt, p.rt, p.buttons));
                s.append(String.format("  rumble large %3d  small %3d%n",
                        p.rumbleLarge, p.rumbleSmall));
            }
            if (st.note != null && !st.note.isEmpty()) s.append('\n').append(st.note).append('\n');
            status.setText(s.toString());
            ui.postDelayed(this, 200);
        }
    };
}
