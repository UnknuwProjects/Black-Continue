package com.racas.black;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

public class BlackActivity extends SDLActivity {
    static final String SHARED = "Black";

    private AlertDialog dialog;
    private boolean notNow;
    private boolean resumed;
    private UpdateChecker.Release update;
    private final GameImporter importer = new GameImporter(this);
    private boolean importing;
    private boolean checking;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        for (View v : new View[] {mSurface, mLayout})
            if (v != null) v.setOnCapturedPointerListener(BlackActivity::onCapturedPointer);
        File own = getExternalFilesDir(null);
        if (own != null) new File(own, "assets").mkdirs();
        
        // Hanya cek update saat startup jika diaktifkan di menu pengaturan
        if (isStartupCheckEnabled(this)) {
            UpdateChecker.check(this, false, (r, failed) -> {
                if (r == null) return;
                update = r;
                tellOfUpdate();
            });
        }
    }

    private static native void nativeLooseKey(int keyCode, boolean down);
    private static native void nativeLooseAxes(float lx, float ly, float rx, float ry, float lt, float rt, float hx,
                                               float hy);
    private static native void nativeLooseRelease();
    private static native void nativeSetStats(float cpuTemp, float batTemp, int cpuLoad, int ramUsed,
                                                int batLevel);

    // The overlay's readings, every two seconds while the game is in front: the battery's temperature from the sticky
    // battery broadcast, the RAM in use from ActivityManager, the CPU's temperature from the thermal zones, its load from
    // /proc/stat or, where newer Android keeps that from apps, from the clocks against their maximums. What the phone
    // does not give goes on as -1 and the overlay leaves it out.
    private Thread tempThread;
    private volatile boolean tempRun;
    private List<ThermalZone> cpuZones;
    private long lastCpuTotal = -1;
    private long lastCpuIdle = -1;

    private static final class ThermalZone {
        final int rank;  // lower is more likely the CPU itself
        final File temp;

        ThermalZone(int rank, File temp) {
            this.rank = rank;
            this.temp = temp;
        }
    }

    private void startTemps() {
        if (tempThread != null) return;
        tempRun = true;
        tempThread = new Thread(() -> {
            while (tempRun) {
                if (!mBrokenLibraries) {
                    try {
                        nativeSetStats(readCpuTemp(), readBatteryTemp(), readCpuLoad(), readRamUsed(),
                                readBatteryLevel());
                    } catch (Throwable t) {
                        tempRun = false;
                    }
                }
                try {
                    Thread.sleep(2000);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }, "overlay-stats");
        tempThread.setDaemon(true);
        tempThread.start();
    }

    private void stopTemps() {
        tempRun = false;
        if (tempThread != null) tempThread.interrupt();
        tempThread = null;
    }

    private float readBatteryTemp() {
        try {
            Intent i = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (i == null) return -1f;
            int tenths = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1);
            return tenths < 0 ? -1f : tenths / 10f;
        } catch (Exception e) {
            return -1f;
        }
    }

    private int readBatteryLevel() {
        try {
            Intent i = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (i == null) return -1;
            int level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
            if (level < 0 || scale <= 0) return -1;
            return clampPercent(level * 100 / scale);
        } catch (Exception e) {
            return -1;
        }
    }

    private int readRamUsed() {
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(info);
            if (info.totalMem <= 0) return -1;
            return clampPercent((int) ((info.totalMem - info.availMem) * 100 / info.totalMem));
        } catch (Exception e) {
            return -1;
        }
    }

    private static int clampPercent(int v) {
        return Math.max(0, Math.min(100, v));
    }

    private static String readLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            return r.readLine();
        } catch (Exception e) {
            return null;
        }
    }

    private static long readLong(String path) {
        try {
            String line = readLine(new File(path));
            return line == null ? -1 : Long.parseLong(line.trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private int readCpuLoad() {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/stat"))) {
            String[] p = r.readLine().trim().split("\\s+");
            long total = 0, idle = 0;
            for (int i = 1; i < p.length && i <= 8; i++) {
                long v = Long.parseLong(p[i]);
                total += v;
                if (i == 4 || i == 5) idle += v;
            }
            boolean had = lastCpuTotal >= 0;
            long dTotal = total - lastCpuTotal;
            long dIdle = idle - lastCpuIdle;
            lastCpuTotal = total;
            lastCpuIdle = idle;
            if (had && dTotal > 0) return clampPercent((int) ((dTotal - dIdle) * 100 / dTotal));
        } catch (Exception e) {
            // kept from apps: the clocks below
        }
        long cur = 0, max = 0;
        int cores = Runtime.getRuntime().availableProcessors();
        for (int i = 0; i < cores; i++) {
            String base = "/sys/devices/system/cpu/cpu" + i + "/cpufreq/";
            long c = readLong(base + "scaling_cur_freq");
            long m = readLong(base + "cpuinfo_max_freq");
            if (c > 0 && m > 0) {
                cur += c;
                max += m;
            }
        }
        return max > 0 ? clampPercent((int) (cur * 100 / max)) : -1;
    }

    private static int zoneRank(String type) {
        if (type.contains("gpu") || type.contains("battery") || type.contains("charger")) return -1;
        if (type.contains("cpu-silicon")) return 0;
        if (type.contains("cpu-0")) return 1;
        if (type.contains("cpu")) return 2;  // cpuss, mtktscpu, cputop
        if (type.contains("tmu")) return 3;  // Exynos
        if (type.contains("soc")) return 4;
        if (type.contains("tsens")) return 5;
        if (type.contains("cluster")) return 6;
        if (type.contains("big") || type.contains("mid") || type.contains("little")) return 7;
        return -1;
    }

    // the zones that name a CPU and that the app may read, best first, found once
    private List<ThermalZone> findCpuZones() {
        List<ThermalZone> out = new ArrayList<>();
        List<String> seen = new ArrayList<>();
        String[] roots = {"/sys/class/thermal", "/sys/devices/virtual/thermal"};
        for (String root : roots) {
            File[] zones = new File(root).listFiles((dir, name) -> name.startsWith("thermal_zone"));
            if (zones == null) continue;
            for (File zone : zones) {
                String type = readLine(new File(zone, "type"));
                if (type == null) continue;
                int rank = zoneRank(type.trim().toLowerCase(Locale.US));
                File temp = new File(zone, "temp");
                if (rank < 0 || !temp.canRead()) continue;
                String path = temp.getAbsolutePath();
                if (seen.contains(path)) continue;
                seen.add(path);
                out.add(new ThermalZone(rank, temp));
            }
        }
        Collections.sort(out, (a, b) -> a.rank - b.rank);
        return out;
    }

    private float readCpuTemp() {
        if (cpuZones == null) cpuZones = findCpuZones();
        for (ThermalZone zone : cpuZones) {
            String line = readLine(zone.temp);
            if (line == null) continue;
            try {
                float v = Float.parseFloat(line.trim());
                if (v > 1000f) v /= 1000f;  // most phones give thousandths of a degree
                if (v > 10f && v < 120f) return v;
            } catch (NumberFormatException e) {
                // the next zone
            }
        }
        return -1f;
    }

    private static boolean loose(InputEvent e, int sources) {
        return !mBrokenLibraries && (e.getSource() & sources) != 0
                && !SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId());
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        boolean pad = KeyEvent.isGamepadButton(e.getKeyCode()) || (e.getKeyCode() >= KeyEvent.KEYCODE_DPAD_UP
                && e.getKeyCode() <= KeyEvent.KEYCODE_DPAD_RIGHT);
        if (pad && loose(e, InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) {
            if (e.getRepeatCount() == 0) nativeLooseKey(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (loose(e, InputDevice.SOURCE_JOYSTICK) && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            nativeLooseAxes(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                    e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                    e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private static boolean onCapturedPointer(View view, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_MOVE:
                for (int h = 0; h < e.getHistorySize(); h++)
                    onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getHistoricalX(h), e.getHistoricalY(h), true);
                onNativeMouse(0, MotionEvent.ACTION_MOVE, e.getX(), e.getY(), true);
                return true;
            case MotionEvent.ACTION_BUTTON_PRESS:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_DOWN, 0, 0, true);
                return true;
            case MotionEvent.ACTION_BUTTON_RELEASE:
                onNativeMouse(e.getButtonState(), MotionEvent.ACTION_UP, 0, 0, true);
                return true;
            case MotionEvent.ACTION_SCROLL:
                onNativeMouse(0, MotionEvent.ACTION_SCROLL, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                        e.getAxisValue(MotionEvent.AXIS_VSCROLL), false);
                return true;
            default:
                return true;
        }
    }

    // Helper methods untuk Startup Update Check (diletakkan di luar method lain secara benar)
    public static boolean isStartupCheckEnabled(android.content.Context context) {
        return context.getSharedPreferences("updates", MODE_PRIVATE).getBoolean("startup_check", true);
    }

    public static void setStartupCheckEnabled(android.content.Context context, boolean enabled) {
        context.getSharedPreferences("updates", MODE_PRIVATE).edit().putBoolean("startup_check", enabled).apply();
    }

    public void checkForUpdates() {
        runOnUiThread(() -> {
            if (checking) return;
            checking = true;
            Toast.makeText(this, "Checking for updates…", Toast.LENGTH_SHORT).show();
            UpdateChecker.check(this, true, (r, failed) -> {
                checking = false;
                if (r != null) {
                    update = r;
                    tellOfUpdate();
                    return;
                }
                if (isFinishing() || (dialog != null && dialog.isShowing())) return;
                String msg = failed ? "Couldn't reach GitHub. Check your internet connection and try again."
                                    : "You have the latest version (" + UpdateChecker.installedVersion(this) + ").";
                dialog = new AlertDialog.Builder(this)
                        .setTitle(failed ? "Update check failed" : "No update")
                        .setMessage(msg)
                        .setPositiveButton("OK", null)
                        .show();
            });
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        startTemps();
        askForGame();
        tellOfUpdate();
    }

    @Override
    protected void onPause() {
        resumed = false;
        stopTemps();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (!mBrokenLibraries) nativeLooseRelease();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < 30) {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    private static final boolean ALL_FILES_ACCESS = Build.VERSION.SDK_INT >= 30;
    private static final int STORAGE_REQUEST = 0x426C;
    private boolean storageAsked;

    private boolean hasStorageAccess() {
        if (ALL_FILES_ACCESS) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void askForGame() {
        if (importing || (dialog != null && dialog.isShowing())) return;
        File own = getExternalFilesDir(null);
        if (own != null && hasGame(new File(own, "assets"))) return;
        if (!hasStorageAccess()) {
            if (!notNow) askAllFilesAccess();
            return;
        }
        File shared = new File(Environment.getExternalStorageDirectory(), SHARED);
        shared.mkdirs();
        if (hasGame(shared) || hasGame(new File(shared, "assets"))) return;
        for (File card : cardFolders())
            if (hasGame(card) || hasGame(new File(card, "assets"))) return;
        tellWhereTheGameGoes();
    }

    private List<File> cardFolders() {
        List<File> out = new ArrayList<>();
        if (!ALL_FILES_ACCESS) {
            File[] own = getExternalFilesDirs(null);
            for (int i = 1; i < own.length; i++) {
                File root = own[i];
                for (int up = 0; root != null && up < 4; up++) root = root.getParentFile();
                if (root != null) out.add(new File(root, SHARED));
            }
            return out;
        }
        StorageManager sm = getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (v.isPrimary() || root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            out.add(new File(root, SHARED));
        }
        return out;
    }

    public String[] cardFolderPaths() {
        List<File> cards = cardFolders();
        String[] out = new String[cards.size()];
        for (int i = 0; i < out.length; i++) out[i] = cards.get(i).getPath();
        return out;
    }

    private void tellOfUpdate() {
        if (update == null || importing || !resumed || isFinishing() || (dialog != null && dialog.isShowing())) return;
        UpdateChecker.Release r = update;
        update = null;
        String installed = "";
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {}

        StringBuilder msg = new StringBuilder(r.name + " is available");
        if (!installed.isEmpty()) msg.append(" (you have ").append(installed).append(")");
        msg.append('.');
        if (!r.changes.isEmpty()) msg.append("\n\nWhat's new:\n").append(r.changes);
        msg.append("\n\nInstall it over this one: your settings and saves stay.");

        android.widget.CheckBox cbSkip = new android.widget.CheckBox(this);
        cbSkip.setText("Don't ask again for this version");
        cbSkip.setPadding(32, 16, 32, 16);

        android.widget.LinearLayout container = new android.widget.LinearLayout(this);
        container.setOrientation(android.widget.LinearLayout.VERTICAL);
        
        android.widget.TextView tvMessage = new android.widget.TextView(this);
        tvMessage.setText(msg.toString());
        tvMessage.setPadding(48, 32, 48, 16);
        tvMessage.setTextSize(14);
        
        container.addView(tvMessage);
        container.addView(cbSkip);

        dialog = new AlertDialog.Builder(this)
                .setTitle("Update available")
                .setView(container)
                .setPositiveButton("Download", (d, w) -> {
                    if (cbSkip.isChecked()) {
                        UpdateChecker.skip(this, r.tag);
                    }
                    open(r.apk != null ? r.apk : r.page);
                })
                .setNegativeButton("No", (d, w) -> {
                    if (cbSkip.isChecked()) {
                        UpdateChecker.skip(this, r.tag);
                    }
                })
                .show();
    }

    private void open(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {}
    }

    private static boolean hasGame(File dir) {
        if (new File(dir, "default.xbe").exists()) return true;
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files)
            if (f.getName().toLowerCase().endsWith(".iso")) return true;
        return false;
    }

    private void askAllFilesAccess() {
        dialog = new AlertDialog.Builder(this)
                .setTitle("Storage access")
                .setMessage("Black Android reads the game from the Black folder on your phone's storage:\n\n"
                        + "/storage/emulated/0/" + SHARED + "\n\n"
                        + (ALL_FILES_ACCESS
                                ? "On the next screen turn on \"Allow access to manage all files\" for Black Android, then come back."
                                : "Allow Black Android to access your storage on the next screen."))
                .setCancelable(false)
                .setPositiveButton("Allow access", (d, w) -> openAccessSettings())
                .setNegativeButton("Not now", (d, w) -> notNow = true)
                .setNeutralButton("Import game", (d, w) -> startImport())
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    private void tellWhereTheGameGoes() {
        dialog = new AlertDialog.Builder(this)
                .setTitle("Game files")
                .setMessage("Copy the game into the Black folder on your phone's storage:\n\n"
                        + "/storage/emulated/0/" + SHARED + "\n\n"
                        + "or into a Black folder at the top of your SD card.\n\n"
                        + "Its disc image (.iso), or default.xbe with its folders. The game starts as soon as they "
                        + "are there.\n\n"
                        + "Or import the disc image from your Downloads, a cloud drive or a USB stick.")
                .setPositiveButton("OK", null)
                .setNeutralButton("Import game", (d, w) -> startImport())
                .setOnDismissListener(d -> tellOfUpdate())
                .show();
    }

    private void startImport() {
        importing = importer.pick();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != GameImporter.REQUEST) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            importing = false;
            return;
        }
        File dir = hasStorageAccess()
                ? new File(Environment.getExternalStorageDirectory(), SHARED)
                : new File(getExternalFilesDir(null), "assets");
        importer.copy(uri, dir, ok -> {
            importing = false;
            if (!ok) askForGame();
            tellOfUpdate();
        });
    }

    private void openAccessSettings() {
        if (!ALL_FILES_ACCESS) {
            boolean ask = !storageAsked || shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE);
            if (ask) {
                storageAsked = true;
                requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE}, STORAGE_REQUEST);
            } else {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
            return;
        }
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (ActivityNotFoundException e2) {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
        }
    }
}
