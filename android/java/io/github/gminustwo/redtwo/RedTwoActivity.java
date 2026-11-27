// Red Two on Android: SDL's activity, with the program (and SDL, linked in statically) in
// libmain.so, Aurora's surface, and what the native side asks of Java (src/runtime/main.cpp
// namespace android, achievements.cpp): the disc image from the document picker, restarting
// the game, HTTP for RetroAchievements.
package io.github.gminustwo.redtwo;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import dev.encounter.aurora.AuroraSurface;

public class RedTwoActivity extends SDLActivity {
    private static final int REQUEST_DISC = 0x5232;
    private static final String PREFS = "red_two";
    private static final String KEY_DISC = "disc_uri";

    private static final Object sPickLock = new Object();
    private static boolean sPickDone;
    private static String sPicked;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);  // (loads libmain.so; the program starts later)
        testEnvironment(getIntent());
    }

    /** Test builds only (debuggable: tools/build_android.ps1 -Debuggable): the launch intent's R2_*
     *  extras become environment variables before the program starts, like the R2_* switches of
     *  the PC builds (adb shell am start -n <package>/.RedTwoActivity --es R2_WATCHDOG 4). Set
     *  through the native side (SDL's nativeSetenv): an arm64 library running under translation
     *  (x86_64 emulators) has an environment of its own, which android.system.Os does not reach. */
    private void testEnvironment(Intent intent) {
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0 || intent == null) return;
        Bundle extras = intent.getExtras();
        if (extras == null) return;
        for (String key : extras.keySet()) {
            Object value = extras.get(key);
            if (!key.startsWith("R2_") || value == null) continue;
            try {
                nativeSetenv(key, value.toString());
            } catch (Throwable e) {
                return;  // (the library did not load: SDL shows its error)
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new AuroraSurface(context);
    }

    private static SharedPreferences prefs() {
        return mSingleton.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    // --- the disc image ---------------------------------------------------------------------
    /** The document picked last time ("" if none). */
    public static String savedDiscUri() {
        return prefs().getString(KEY_DISC, "");
    }

    public static void saveDiscUri(String uri) {
        prefs().edit().putString(KEY_DISC, uri).apply();
    }

    /** Opens a document for reading; the native side owns the descriptor. -1 on failure. */
    public static int openUriFd(String uri) {
        try {
            ParcelFileDescriptor pfd = mSingleton.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    /** Shows the system's document picker and waits for the choice (called on SDL's thread). */
    public static String pickDisc() {
        synchronized (sPickLock) {
            sPickDone = false;
            sPicked = null;
        }
        mSingleton.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                mSingleton.startActivityForResult(i, REQUEST_DISC);
            } catch (Exception e) {
                pickFinished(null);
            }
        });
        synchronized (sPickLock) {
            while (!sPickDone) {
                try {
                    sPickLock.wait();
                } catch (InterruptedException e) {
                    return "";
                }
            }
            return sPicked == null ? "" : sPicked;
        }
    }

    private static void pickFinished(String uri) {
        synchronized (sPickLock) {
            sPicked = uri;
            sPickDone = true;
            sPickLock.notifyAll();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_DISC) {
            String uri = null;
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri u = data.getData();
                try {
                    // kept across restarts of the app
                    getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception e) {
                    // (a provider without persistable grants: works until the app ends)
                }
                uri = u.toString();
            }
            pickFinished(uri);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // --- restart (the menu's reset, after importing a save) ------------------------------------
    /** RestartActivity, in a process of its own, ends this process and starts the game again. */
    public static void restartApp() {
        Intent i = new Intent(mSingleton, RestartActivity.class);
        i.putExtra(RestartActivity.EXTRA_PID, Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        mSingleton.startActivity(i);
    }

    // --- HTTP (RetroAchievements) -------------------------------------------------------------
    /** GET (post == null) or POST; the body, or null when the server could not be reached. */
    public static byte[] httpFetch(String url, byte[] post, String contentType, String userAgent, int[] status) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(30000);
            c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent", userAgent);
            if (post != null) {
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", contentType);
                c.setFixedLengthStreamingMode(post.length);
                try (OutputStream o = c.getOutputStream()) {
                    o.write(post);
                }
            }
            status[0] = c.getResponseCode();
            InputStream in = status[0] >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream s = in) {
                    byte[] buf = new byte[16384];
                    for (int n; (n = s.read(buf)) > 0; ) body.write(buf, 0, n);
                }
            }
            return body.toByteArray();
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
