// SPDX-License-Identifier: GPL-2.0-or-later
package com.astrobotquest.vrhost;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.hardware.lights.Light;
import android.hardware.lights.LightState;
import android.hardware.lights.LightsManager;
import android.hardware.lights.LightsRequest;
import android.os.Build;
import android.os.Bundle;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.VibratorManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Hosts the PS4 emulator core on a Meta Quest: installs its Linux runtime, starts it, feeds it a
 * paired DualSense as the DualShock 4 the game expects (buttons, sticks, touchpad, motion
 * sensors in; rumble and light bar colour out) and lets the native side show its frames in the
 * headset.
 *
 * Files the user provides live in the app's external folder
 * (/sdcard/Android/data/com.astrobotquest.vrhost/files):
 *   games/SERIAL/eboot.bin ...   the extracted game (or /data/local/tmp/astro/games/SERIAL)
 *   data/shadPS4/                 the emulator's own folder (sys_modules, saves, logs)
 *   vrhost.txt                    optional settings, see readSettings
 * and what the app leaves there for looking at afterwards:
 *   host.log, core.log            this app's log and the emulator's output of the last start
 *   host.prev.log, core.prev.log  the same of the start before
 */
public class MainActivity extends Activity
        implements InputManager.InputDeviceListener, SensorEventListener {
    private static final String TAG = "AstroVR";

    // PS4 pad button bits (ORBIS_PAD_BUTTON_*).
    private static final int L3 = 0x2, R3 = 0x4, OPTIONS = 0x8;
    private static final int UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80;
    private static final int L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800;
    private static final int TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;
    private static final int TOUCHPAD = 0x100000;

    // The touchpad as the emulator takes it: the resolution of a DualSense's.
    private static final int TOUCH_WIDTH = 1920, TOUCH_HEIGHT = 1080;

    private static final int STATUS_WIDTH = 1024, STATUS_HEIGHT = 512;

    /** Second place games are looked for; every app may read what adb puts there. */
    private static final String SHELL_GAMES = "/data/local/tmp/astro/games";

    // Mirrors CoreProcess::State.
    private static final int CORE_IDLE = 0, CORE_STARTING = 1, CORE_RUNNING = 2, CORE_STOPPED = 3,
            CORE_FAILED = 4;

    /** How long a rumble command lasts if the game says nothing more, and how often it is renewed. */
    private static final int RUMBLE_MILLIS = 1000, RUMBLE_RENEW_MILLIS = 400;

    private static final int REQUEST_MICROPHONE = 1;

    static {
        System.loadLibrary("astrovr");
    }

    private native void nativeStartXr(float refreshRate, int eyeWidth, int eyeHeight,
            boolean trackHands, int sharpen, boolean cubic, boolean showStats, float predictMs,
            int dynamicResolution, boolean cpuBoost, boolean moveInput, boolean moveRumble);

    private native boolean nativeStartCore(String loader, String runtimeRoot, String storageRoot,
            String game, String logFile, String[] extraArgs, String[] extraEnv);

    private native void nativeStop();

    private native int nativeCoreState();

    private native String nativeCoreMessage();

    private native long nativeFrameCount();

    private native void nativeSetStatusImage(ByteBuffer pixels);

    private native void nativeInput(int buttons, int leftX, int leftY, int rightX, int rightY,
            int leftTrigger, int rightTrigger, boolean touchDown, int touchX, int touchY);

    private native void nativeMotion(float gx, float gy, float gz, float ax, float ay, float az);

    private native long nativePadFeedback();

    private static final int VENDOR_SONY = 0x054c;
    private static final int VENDOR_META = 0x2833;

    /** What a reset of the view covers: the controller's heading, the player's seat. */
    private static final int RECENTER_PAD = 1, RECENTER_SEAT = 2;

    private native void nativeRecenter(int what);

    private native int nativeXrStatus();

    private native void nativeSetMicrophone(boolean enabled, float gain);

    private static native void nativeSetLogFile(String path);

    private static native void nativeLog(int priority, String message);

    private final Handler handler = new Handler(Looper.getMainLooper());
    private View inputView;
    private volatile String setupStatus = "Starting...";
    private volatile boolean setupFailed;
    private String shownStatus = "";

    // Controller state.
    private int keyButtons;
    private int hatButtons;
    private int triggerButtons;
    private float leftX, leftY, rightX, rightY, leftTrigger, rightTrigger;
    private boolean padTouchDown;
    private float padTouchX, padTouchY;
    private boolean padTouchClick;
    private final float[] acceleration = {0.0f, 9.81f, 0.0f};

    // The controller itself: what the game's feedback goes to.
    private InputDevice gamepad;
    private SensorManager controllerSensors;
    private VibratorManager controllerVibrators;
    private LightsManager.LightsSession lightSession;
    private Light lightBar;
    private long shownFeedback = -1;
    private long rumbleRenewAt;

    // Settings.
    private boolean stickTouchpad = true;
    private boolean motion = true;
    private boolean rumble = true;
    private boolean trackHands = true;
    private boolean moveInput;
    private String gamePath;
    private int sharpen = 1;
    private boolean cubic;
    private boolean antialias = true;
    private int pace;
    private int msaa = 1;
    private boolean showStats;
    private float predictMs = 25;
    private boolean microphone = true;
    private int dynamicResolution = 1;
    private boolean cpuBoost = true;
    /**
     * How much of the PlayStation VR's field of view the game draws, in percent: less than all
     * of it for more pixels to the degree, which is what the player asked for.
     */
    private int fieldOfView = 85;
    /**
     * Set by the test that runs the activity without showing it (SandboxShell "drystart"):
     * nothing is asked of the user or of the system's window manager then.
     */
    private boolean dryRun;
    private int statusPictures;
    private float microphoneGain = 1.0f;
    private boolean seatTaken;
    /** How long OPTIONS is held for the view to be reset, as on a PlayStation VR. */
    private static final long OPTIONS_HOLD_MS = 1000;
    private final Runnable optionsHeld = () -> {
        logInfo("OPTIONS held: view reset");
        nativeRecenter(RECENTER_PAD | RECENTER_SEAT);
    };
    private long quietFrames = -1;
    private long quietSince;
    private long statsFrames = -1;
    private long statsTime;
    private String statsRate = "...";
    private float refreshRate;
    /** What SHADPS4_TITLE_RESOLUTION is set to, null to leave the choice to the emulator. */
    private String resolution;
    private boolean realTime = true;
    private final List<String> extraArgs = new ArrayList<>();
    private final List<String> extraEnv = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dryRun = getIntent() != null && getIntent().getBooleanExtra("dry_run", false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Nothing is drawn here, the view only exists to receive controller events.
        inputView = new View(this);
        inputView.setFocusable(true);
        inputView.setFocusableInTouchMode(true);
        inputView.setOnCapturedPointerListener((view, event) -> handleTouchpad(event));
        setContentView(inputView);
        inputView.requestFocus();

        // Kept as files: by the time somebody asks what happened in a session, the system's
        // log has long moved on.
        File external = getExternalFilesDir(null);
        keepPrevious(new File(external, "host.log"), new File(external, "host.prev.log"));
        nativeSetLogFile(new File(external, "host.log").getAbsolutePath());
        logInfo("Any4Quest VR Host " + versionName() + " on " + Build.MODEL + ", system "
                + Build.DISPLAY);

        readSettings();
        nativeStartXr(refreshRate, 1440, 1536, trackHands, sharpen, cubic, showStats, predictMs,
                dynamicResolution, cpuBoost, moveInput, rumble);
        nativeSetMicrophone(microphone, microphoneGain);
        if (microphone && !dryRun && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            // The game listens to the headset's microphone, as it did to PlayStation VR's:
            // blowing at things is one of the ways to play with its world.
            logInfo("asking for the microphone");
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO},
                    REQUEST_MICROPHONE);
        }

        InputManager inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(this, handler);
        findGamepad();

        new Thread(this::setUpAndStart, "setup").start();
        handler.post(this::refreshStatus);
        handler.post(this::applyFeedback);
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        getSystemService(InputManager.class).unregisterInputDeviceListener(this);
        detachGamepad();
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_MICROPHONE) {
            // Nothing else to do: the native side keeps trying to open the microphone.
            logInfo("microphone " + (grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED
                            ? "allowed" : "not allowed"));
        }
    }

    /** Called by the native side when the headset session is over and will not come back. */
    private void onSessionEnded() {
        logInfo("the session is over: closing");
        if (!dryRun) {
            runOnUiThread(this::finish);
        }
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    private static void keepPrevious(File current, File previous) {
        if (current.isFile()) {
            previous.delete();
            current.renameTo(previous);
        }
    }

    private static void logInfo(String message) {
        nativeLog(Log.INFO, message);
    }

    private static void logWarning(String message, Throwable cause) {
        nativeLog(Log.WARN, cause == null ? message : message + ": " + cause);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        logInfo(hasFocus ? "input focus gained" : "input focus lost");
        if (hasFocus) {
            // Gives the controller's touchpad to the game instead of a mouse pointer.
            inputView.requestPointerCapture();
        }
    }

    // --- settings -------------------------------------------------------------------------------

    /**
     * Reads the optional vrhost.txt: one "key=value" per line.
     *   refresh_rate=72        display refresh rate to ask for (72, 80, 90, 120). The game
     *                          draws one frame for every two refreshes where the GPU manages
     *                          that, and one for every three where it does not: 45 or 30
     *                          frames a second at the 90 Hz asked for without this, and 60,
     *                          40 or 30 at 120. At 72 and 80 Hz there is only the one pace
     *                          (36, 40): frames are never given more refreshes than still
     *                          makes 30 a second, below that the game draws as it can
     *   pace=2                 leave it to the game how many refreshes a frame takes, as up to
     *                          version 0.6: it takes two, and one more whenever a frame is
     *                          late, so frames last unevenly where it only just keeps up. A
     *                          larger number holds every frame to that many refreshes. Without
     *                          this the emulator chooses, see resolution
     *   resolution=960         the size the game draws its scene at, per eye: 816 (x870), 960
     *                          (x1080), 1200 (x1280) or 1440 (x1536), or "game" for the game's
     *                          own choice. Without this the emulator chooses: the fastest pace
     *                          at which 960x1080 (the console's own smallest) fits the GPU's
     *                          time, 816x870 only where that would mean fewer than 30 frames a
     *                          second, and then the largest size that fits that pace
     *   antialias=0            leave edges as jagged as the game draws them here. The console
     *                          multisamples (see msaa), which this GPU cannot afford; by
     *                          default the emulator smooths edges in the finished scene
     *                          instead, for a fraction of the cost
     *   real_time=0            let the game count time in frames, as it does on the console:
     *                          it then runs in slow motion whenever a frame takes longer than
     *                          a sixtieth of a second, which here is always. By default the
     *                          emulator has it run by the clock
     *   hands=0                do not use hand tracking to place the controller; it is then held
     *                          at a fixed spot in front of the player
     *   input_mode=move        use left/right Touch grip poses and buttons as two PS Move
     *                          devices; default gamepad retains the DualSense/DS4 input path
     *   game_path=games/CUSA... select a game folder or eboot.bin; relative paths start in
     *                          the app's external files folder, absolute paths are accepted
     *   sharpen=0              leave the picture as soft as it gets from being shown larger
     *                          than it was drawn. By default (1) the emulator sharpens it once
     *                          per frame of the game. 2 has the headset do it instead, with its
     *                          Super Resolution filter on every refresh: finer, but it takes
     *                          about a twentieth of the GPU from the game. 3 is the same left
     *                          to the system, which drops it when the GPU is busy (always, in
     *                          a level). 4 is the headset's plainer filter
     *   cubic=1                have the headset enlarge the picture with a cubic filter instead
     *                          of a linear one: sharper, for some GPU time on every refresh
     *   stick_touchpad=0       stop using the right stick as a finger on the touchpad
     *   motion=0               ignore the controller's motion sensors
     *   rumble=0               no vibration
     *   msaa=4                 let the game multisample as it does on the console (1, 2 or 4).
     *                          The default of 1 turns that off: it is by far the most expensive
     *                          thing the headset's GPU is asked to do
     *   stats=1                keep a small panel in view with the game's frame rate, the
     *                          display's refresh rate and whether the hands are seen
     *   predict_ms=25          how far beyond the next refresh the head pose given to the game
     *                          is predicted (0 to 80). More if the edges of the picture show
     *                          when turning the head, less if the world wobbles
     *   dynamic_resolution=0   do not ask the system what size it recommends for the
     *                          picture. Asking is what the system wants of an app before it
     *                          lets the GPU run at its faster clocks; the picture is shown at
     *                          full size all the same (the game draws its scene smaller when
     *                          the GPU is short, which is what saves time). 2 also shows the
     *                          picture at the recommended size, as up to version 0.6
     *   cpu_boost=0            do not ask for the processor's fastest clock, which the system
     *                          otherwise grants for the first 45 seconds and now and then
     *   fov=100                have the game draw that many percent of the field of view it
     *                          draws on a PlayStation VR (100 by 103 degrees an eye), and show
     *                          it over that much: the same pixels over fewer degrees is a sharper
     *                          picture, with a black border where the rest was. Default 85 (18%
     *                          more pixels to the degree); 100 is the console's own, 50 to 120
     *   mic=0                  the game does not get to hear the headset's microphone
     *   mic_gain=2             make what it hears that many times louder (0.1 to 30), if
     *                          blowing does too little or everything counts as blowing
     *   arg=...                extra emulator argument (may repeat)
     *   env=NAME=value         extra emulator environment variable (may repeat)
     */
    private void readSettings() {
        File file = new File(getExternalFilesDir(null), "vrhost.txt");
        // The test that starts the activity without showing it may bring settings of its own.
        String testSettings = dryRun ? getIntent().getStringExtra("settings") : null;
        if (testSettings != null) {
            file = new File(testSettings);
        }
        if (!file.isFile()) {
            return;
        }
        try {
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                line = line.trim();
                int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals <= 0) {
                    continue;
                }
                String key = line.substring(0, equals).trim();
                String value = line.substring(equals + 1).trim();
                switch (key) {
                    case "refresh_rate":
                        refreshRate = Float.parseFloat(value);
                        break;
                    case "resolution":
                        switch (value) {
                            case "816":
                                resolution = "3";
                                break;
                            case "960":
                                resolution = "4";
                                break;
                            case "1200":
                                resolution = "5";
                                break;
                            case "1440":
                                resolution = "6";
                                break;
                            case "game":
                                resolution = "title";
                                break;
                            default:
                                resolution = null;
                                break;
                        }
                        break;
                    case "real_time":
                        realTime = !value.equals("0");
                        break;
                    case "hands":
                        trackHands = !value.equals("0");
                        break;
                    case "input_mode":
                        if (!value.equals("move") && !value.equals("gamepad")) {
                            logWarning("input_mode must be gamepad or move; keeping current mode", null);
                            continue;
                        }
                        moveInput = value.equals("move");
                        break;
                    case "game_path":
                        gamePath = value;
                        break;
                    case "sharpen":
                        sharpen = Math.max(0, Math.min(4, Integer.parseInt(value)));
                        break;
                    case "cubic":
                        cubic = !value.equals("0");
                        break;
                    case "antialias":
                        antialias = !value.equals("0");
                        break;
                    case "pace":
                        pace = Math.max(0, Math.min(6, Integer.parseInt(value)));
                        break;
                    case "stick_touchpad":
                        stickTouchpad = !value.equals("0");
                        break;
                    case "motion":
                        motion = !value.equals("0");
                        break;
                    case "rumble":
                        rumble = !value.equals("0");
                        break;
                    case "stats":
                        showStats = !value.equals("0");
                        break;
                    case "predict_ms":
                        predictMs = Math.max(0, Math.min(80, Float.parseFloat(value)));
                        break;
                    case "msaa":
                        msaa = Math.max(1, Math.min(4, Integer.parseInt(value)));
                        break;
                    case "dynamic_resolution":
                        dynamicResolution = Math.max(0, Math.min(2, Integer.parseInt(value)));
                        break;
                    case "cpu_boost":
                        cpuBoost = !value.equals("0");
                        break;
                    case "fov":
                        fieldOfView = Math.max(50, Math.min(120, Integer.parseInt(value)));
                        break;
                    case "mic":
                        microphone = !value.equals("0");
                        break;
                    case "mic_gain":
                        microphoneGain = Math.max(0.1f, Math.min(30.0f, Float.parseFloat(value)));
                        break;
                    case "arg":
                        extraArgs.add(value);
                        break;
                    case "env":
                        extraEnv.add(value);
                        break;
                    default:
                        logWarning("unknown setting " + key, null);
                        continue;
                }
                logInfo("setting " + key + "=" + value);
            }
        } catch (IOException | NumberFormatException e) {
            logWarning("unable to read " + file, e);
        }
    }

    // --- runtime and game -----------------------------------------------------------------------

    private void setUpAndStart() {
        try {
            // On the first start, and after every update of the app: a few seconds.
            File runtime = new File(getFilesDir(), "runtime");
            setupStatus = "Installing the emulator runtime...";
            if (RuntimeInstaller.install(this, runtime)) {
                logInfo("runtime unpacked");
            }

            File external = getExternalFilesDir(null);
            File games = new File(external, "games");
            games.mkdirs();
            new File(external, "data/shadPS4/sys_modules").mkdirs();
            File eboot = GameSelection.resolve(gamePath, external, new File(SHELL_GAMES));
            if (eboot == null) {
                setupFailed = true;
                setupStatus = "No game found.\n\nCopy the extracted game folder (the one that "
                        + "contains eboot.bin) to the headset, to either of\n" + games
                        + "/CUSA12392\n" + SHELL_GAMES + "/CUSA12392\nthen start this app again.";
                return;
            }

            File storage = new File(getFilesDir(), "core");
            storage.mkdirs();
            // Settings from vrhost.txt come last: they replace what is set here.
            List<String> env = new ArrayList<>();
            // Saves, firmware modules and logs go where the user can reach them over USB.
            env.add("XDG_DATA_HOME=" + new File(external, "data"));
            env.add("SHADPS4_MAX_MSAA=" + msaa);
            // The emulator has more busy threads than the headset has processor cores for it.
            // The sound must not stutter for that (the game mixes it on two threads, the
            // emulator passes it on with one per output), a frame should not wait behind the
            // game's helpers on the two threads all of it passes through, and the two that
            // keep time with the display (they have next to nothing to do) must do so on time.
            env.add("SHADPS4_THREAD_NICE=Game:Main=-14,shadPS4:GpuComm=-14,sndx_out_thread=-16,"
                    + "sndx_audio3d=-16,shadPS4:AudioOu=-16,shadPS4:VrHostL=-18,"
                    + "shadPS4:Present=-18");
            // On the GPU the emulator steps back behind this app: copying a finished picture
            // to the display takes a millisecond, and must not wait until the GPU is through
            // with everything the game has queued for the pictures after it.
            env.add("KGSL_COMPAT_PRIORITY=12");
            if (msaa == 1) {
                // Without multisampling the GPU driver's direct rendering is the faster of its
                // two ways to draw (about 5% of the GPU's time with this game); its own choice
                // per render pass goes for the tiled way too often.
                env.add("TU_DEBUG=sysmem");
            }
            // A few lines every five or ten seconds in core.log: the frame rate and where a
            // frame's time goes, how well each sound port keeps up, what the microphone hears.
            env.add("SHADPS4_FRAME_STATS=1");
            env.add("SHADPS4_AUDIOOUT_STATS=1");
            env.add("SHADPS4_AUDIOIN_STATS=1");
            // The emulated headset refreshes as often as the display does (and, once frames
            // are shown, when it does): the game draws one frame for every two refreshes, or
            // for every three where that is all the GPU manages.
            int displayRate = (nativeXrStatus() >> 8) & 0xff;
            if (displayRate == 0) {
                displayRate = refreshRate > 0 ? Math.round(refreshRate) : 90;
            }
            env.add("SHADPS4_VR_REFRESH_RATE=" + displayRate);
            logInfo("the emulated headset refreshes " + displayRate + " times a second");
            if (resolution != null) {
                env.add("SHADPS4_TITLE_RESOLUTION=" + resolution);
            }
            if (!realTime) {
                env.add("SHADPS4_TITLE_TIMESTEP=0");
            }
            if (pace >= 2) {
                env.add("SHADPS4_VR_PACE=" + pace);
            }
            if (fieldOfView != 100) {
                env.add("SHADPS4_VR_FOV=" + fieldOfView);
            }
            double angle = Math.toDegrees(Math.atan(1.2074 * fieldOfView / 100.0)
                    + Math.atan(1.1813 * fieldOfView / 100.0));
            logInfo("the game draws " + fieldOfView + "% of PlayStation VR's field of view: "
                    + Math.round(angle) + " degrees across an eye, "
                    + Math.round(1440 / angle * 10) / 10.0 + " pixels to the degree at most"
                    + " (fov= in vrhost.txt; 100 is the console's own)");
            if (!antialias) {
                env.add("SHADPS4_RESOLVE_AA=0");
            }
            if (sharpen == 1) {
                // The game draws 1440 pixels across for what the display shows with well over
                // twice as many, and enlarges its scene to get even there.
                env.add("SHADPS4_VR_SHARPEN=0.6");
            }
            env.addAll(extraEnv);
            // Both sides of the socket must select the same mode. Use input_mode rather
            // than an env override, which cannot configure the already-started XR host.
            env.add("SHADPS4_VR_INPUT_MODE=" + (moveInput ? "move" : "gamepad"));

            setupStatus = "Starting the emulator...";
            keepPrevious(new File(external, "core.log"), new File(external, "core.prev.log"));
            logInfo("game: " + eboot);
            String loader = getApplicationInfo().nativeLibraryDir + "/libastro_ld.so";
            boolean started = nativeStartCore(loader, runtime.getAbsolutePath(),
                    storage.getAbsolutePath(), eboot.getAbsolutePath(),
                    new File(external, "core.log").getAbsolutePath(),
                    extraArgs.toArray(new String[0]), env.toArray(new String[0]));
            if (!started) {
                setupFailed = true;
                setupStatus = "The emulator could not be started.";
            }
        } catch (IOException e) {
            nativeLog(Log.ERROR, "setup failed: " + e);
            setupFailed = true;
            setupStatus = "Setup failed: " + e.getMessage();
        }
    }

    // --- status panel ---------------------------------------------------------------------------

    private void refreshStatus() {
        String status = setupStatus;
        if (!setupFailed) {
            switch (nativeCoreState()) {
                case CORE_STARTING:
                    status = "Starting the emulator...";
                    break;
                case CORE_RUNNING:
                    status = nativeFrameCount() == 0 ? "Booting the game..." : "Loading...";
                    if (showStats && nativeFrameCount() > 0) {
                        status = statsLine();
                    }
                    // A game that stopped drawing looks like one that loads. Loads end.
                    long frames = nativeFrameCount();
                    long now = SystemClock.elapsedRealtime();
                    if (frames != quietFrames) {
                        quietFrames = frames;
                        quietSince = now;
                    }
                    long quiet = (now - quietSince) / 1000;
                    if (frames > 0 && quiet >= 20) {
                        status = "No new picture from the game for " + quiet / 5 * 5
                                + " seconds.\nA long load looks like this too. Past a minute the "
                                + "game is stuck: close the app with the Meta button and start it "
                                + "again. What happened is kept in host.log and core.log.";
                    }
                    break;
                case CORE_STOPPED:
                case CORE_FAILED:
                    status = nativeCoreMessage() + "\n\nLog: "
                            + new File(getExternalFilesDir(null), "core.log");
                    break;
                default:
                    break;
            }
            // Waiting is the moment to say what the game will be played with.
            if (moveInput) {
                int xr = nativeXrStatus();
                status += "\n\nTouch controllers as PS Move: left "
                        + ((xr & 4) != 0 ? "tracked" : "not tracked") + ", right "
                        + ((xr & 8) != 0 ? "tracked" : "not tracked")
                        + ". Use the headset's Reset view to recenter.";
            } else if (gamepad == null) {
                status += "\n\nNo controller found. Switch the DualSense on (PS button); pair it "
                        + "first under Settings > Bluetooth if it never was.";
            } else {
                status += "\n\nController: " + gamepad.getName()
                        + (controllerSensors != null ? "" : " (no motion sensors)")
                        + "\nTo reset the view, hold OPTIONS for a second or press the PS button: "
                        + "where you are then is your seat, and the way you face is straight "
                        + "ahead.";
            }
        }
        if (!status.equals(shownStatus)) {
            shownStatus = status;
            drawStatus(status);
        }
        handler.postDelayed(this::refreshStatus, 500);
    }

    /** What the game and the display are doing, for the panel that stats=1 keeps in view. */
    private String statsLine() {
        long frames = nativeFrameCount();
        long now = SystemClock.elapsedRealtime();
        if (statsFrames < 0) {
            statsFrames = frames;
            statsTime = now;
        } else if (now - statsTime >= 2000) {
            // Over two seconds: a rate counted over less jumps about.
            statsRate = String.format(Locale.US, "%.0f",
                    (frames - statsFrames) * 1000.0 / (now - statsTime));
            statsFrames = frames;
            statsTime = now;
        }
        String rate = statsRate;
        int xr = nativeXrStatus();
        return "Game: " + rate + " frames a second\nDisplay: " + ((xr >> 8) & 0xff) + " Hz\n"
                + (moveInput ? "Move tracking: left " + ((xr & 4) != 0 ? "yes" : "no")
                        + ", right " + ((xr & 8) != 0 ? "yes" : "no")
                        : "Hands holding the controller: " + ((xr & 2) != 0 ? "seen" : "not seen"));
    }

    private void drawStatus(String status) {
        Bitmap bitmap = Bitmap.createBitmap(STATUS_WIDTH, STATUS_HEIGHT, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Color.argb(230, 16, 20, 32));
        canvas.drawRoundRect(new RectF(0, 0, STATUS_WIDTH, STATUS_HEIGHT), 40, 40, paint);

        paint.setColor(Color.rgb(90, 170, 255));
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setTextSize(52);
        canvas.drawText("Any4Quest VR Host", 48, 86, paint);

        paint.setColor(Color.WHITE);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setTextSize(30);
        float y = 160;
        for (String paragraph : status.split("\n")) {
            // Wrap by measuring: at a space where there is one, anywhere where there is none
            // (paths have no spaces to break at).
            String rest = paragraph;
            do {
                int count = paint.breakText(rest, true, STATUS_WIDTH - 96, null);
                if (count < rest.length()) {
                    int space = rest.lastIndexOf(' ', count);
                    if (space > 0) {
                        count = space + 1;
                    }
                }
                canvas.drawText(rest, 0, count, 48, y, paint);
                rest = rest.substring(count);
                y += 40;
            } while (!rest.isEmpty());
        }

        if (dryRun && statusPictures < 8) {
            // Nobody sees the panel in a test: its pictures are kept for looking at, in the
            // folder the test has made for them.
            File picture = new File(new File(getFilesDir(), "drystart"),
                    "status-" + statusPictures++ + ".png");
            try (FileOutputStream out = new FileOutputStream(picture)) {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
            } catch (IOException e) {
                logWarning("unable to write " + picture, e);
            }
        }

        ByteBuffer pixels = ByteBuffer.allocateDirect(STATUS_WIDTH * STATUS_HEIGHT * 4);
        bitmap.copyPixelsToBuffer(pixels);
        nativeSetStatusImage(pixels);
        bitmap.recycle();
    }

    // --- controller: buttons and sticks ---------------------------------------------------------

    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
                return CROSS;
            case KeyEvent.KEYCODE_BUTTON_B:
                return CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X:
                return SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return L1;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return R1;
            case KeyEvent.KEYCODE_BUTTON_L2:
                return L2;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return R2;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return L3;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return R3;
            case KeyEvent.KEYCODE_BUTTON_START:
                return OPTIONS;
            // The Create button stands in for a click of the touchpad as well.
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BUTTON_1:
                return TOUCHPAD;
            case KeyEvent.KEYCODE_DPAD_UP:
                return UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return LEFT;
            default:
                return 0;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_MODE) {
            // The PS button resets the view: where the head is now is where the player sits,
            // the way they face is straight ahead, and so is the way the controller points
            // (which only matters while the headset does not see the hands holding it).
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                logInfo("PS button: view reset");
                nativeRecenter(RECENTER_PAD | RECENTER_SEAT);
            }
            return true;
        }
        int button = buttonFor(event.getKeyCode());
        if (button == 0) {
            return super.dispatchKeyEvent(event);
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            noteInputFrom(event.getDevice());
            if (button == CROSS && !seatTaken) {
                // The first press of X is the player settled in, controller in hand, looking
                // at the game: that, and not where they were when the app started, is their
                // seat.
                seatTaken = true;
                logInfo("first press of X: the player's seat is where they are now");
                nativeRecenter(RECENTER_SEAT);
            }
            if (button == OPTIONS && event.getRepeatCount() == 0) {
                // What a PlayStation VR does when OPTIONS is held, and what the game tells its
                // players to do when the view is off. The game still sees the button.
                handler.removeCallbacks(optionsHeld);
                handler.postDelayed(optionsHeld, OPTIONS_HOLD_MS);
            }
            keyButtons |= button;
        } else if (event.getAction() == KeyEvent.ACTION_UP) {
            if (button == OPTIONS) {
                handler.removeCallbacks(optionsHeld);
            }
            keyButtons &= ~button;
        }
        sendInput();
        return true;
    }

    private static float axis(MotionEvent event, int axis) {
        InputDevice.MotionRange range = event.getDevice() != null
                ? event.getDevice().getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) : null;
        float value = event.getAxisValue(axis);
        // Sticks rest slightly off centre.
        return range != null && Math.abs(value) <= range.getFlat() ? 0.0f : value;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            leftX = axis(event, MotionEvent.AXIS_X);
            leftY = axis(event, MotionEvent.AXIS_Y);
            rightX = axis(event, MotionEvent.AXIS_Z);
            rightY = axis(event, MotionEvent.AXIS_RZ);
            leftTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_BRAKE));
            rightTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_GAS));

            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            hatButtons = (hatX < -0.5f ? LEFT : 0) | (hatX > 0.5f ? RIGHT : 0)
                    | (hatY < -0.5f ? UP : 0) | (hatY > 0.5f ? DOWN : 0);
            triggerButtons = (leftTrigger > 0.12f ? L2 : 0) | (rightTrigger > 0.12f ? R2 : 0);
            sendInput();
            return true;
        }
        if (handleTouchpad(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    /** The controller's own touchpad, when the system exposes it with finger positions. */
    private boolean handleTouchpad(MotionEvent event) {
        if (!event.isFromSource(InputDevice.SOURCE_TOUCHPAD) || event.getDevice() == null) {
            return false;
        }
        // Sticks and touchpad are one device with two meanings for X and Y.
        InputDevice.MotionRange rangeX = event.getDevice().getMotionRange(MotionEvent.AXIS_X,
                InputDevice.SOURCE_TOUCHPAD);
        InputDevice.MotionRange rangeY = event.getDevice().getMotionRange(MotionEvent.AXIS_Y,
                InputDevice.SOURCE_TOUCHPAD);
        if (rangeX == null || rangeY == null || rangeX.getRange() <= 0 || rangeY.getRange() <= 0) {
            return false;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                padTouchDown = true;
                padTouchX = (event.getX() - rangeX.getMin()) / rangeX.getRange();
                padTouchY = (event.getY() - rangeY.getMin()) / rangeY.getRange();
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                padTouchDown = false;
                break;
            default:
                break;
        }
        padTouchClick = (event.getButtonState() & MotionEvent.BUTTON_PRIMARY) != 0;
        sendInput();
        return true;
    }

    private static int stick(float value) {
        return Math.max(0, Math.min(255, Math.round(128.0f + value * 127.0f)));
    }

    private void sendInput() {
        int buttons = keyButtons | hatButtons | triggerButtons | (padTouchClick ? TOUCHPAD : 0);

        boolean touchDown = padTouchDown;
        float touchX = padTouchX, touchY = padTouchY;
        if (!touchDown && stickTouchpad) {
            // This title has no use for the right stick, while several of its gadgets want
            // swipes: the stick moves a finger that touches down at the pad's centre.
            float deflection = (float) Math.hypot(rightX, rightY);
            if (deflection > 0.25f) {
                touchDown = true;
                touchX = 0.5f + rightX * 0.45f;
                touchY = 0.5f + rightY * 0.45f;
            }
        }

        nativeInput(buttons, stick(leftX), stick(leftY), stick(rightX), stick(rightY),
                Math.round(Math.max(0.0f, Math.min(1.0f, leftTrigger)) * 255.0f),
                Math.round(Math.max(0.0f, Math.min(1.0f, rightTrigger)) * 255.0f), touchDown,
                Math.max(0, Math.min(TOUCH_WIDTH - 1, Math.round(touchX * (TOUCH_WIDTH - 1)))),
                Math.max(0, Math.min(TOUCH_HEIGHT - 1, Math.round(touchY * (TOUCH_HEIGHT - 1)))));
    }

    // --- controller: the device, its motion sensors, rumble and light ----------------------------

    private static boolean isGamepad(InputDevice device) {
        return device != null && !device.isVirtual()
                && device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                && device.supportsSource(InputDevice.SOURCE_JOYSTICK);
    }

    /**
     * How much a device looks like the controller the game is played with. The headset's own
     * controllers also show up as gamepads (and come first in the list when the app was started
     * with one of them): a controller from the console's maker goes before anything else, the
     * headset's own after everything else, and motion sensors and motors count for the rest.
     */
    private int rank(InputDevice device) {
        int rank = 0;
        if (device.getVendorId() == VENDOR_SONY) {
            rank += 8;
        } else if (device.getVendorId() == VENDOR_META) {
            rank -= 8;
        }
        try {
            if (findSensors(device) != null) {
                rank += 2;
            }
            if (device.getVibratorManager().getVibratorIds().length > 0) {
                rank += 1;
            }
        } catch (RuntimeException e) {
            // A device that cannot say what it has is ranked by its maker alone.
        }
        return rank;
    }

    private void findGamepad() {
        InputManager inputManager = getSystemService(InputManager.class);
        InputDevice best = gamepad;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (isGamepad(device) && (best == null || rank(device) > rank(best))) {
                best = device;
            }
        }
        useGamepad(best);
    }

    /** Makes `device` the controller, if it is not that already. */
    private void useGamepad(InputDevice device) {
        if (device == null || (gamepad != null && gamepad.getId() == device.getId())) {
            return;
        }
        if (gamepad != null) {
            logInfo("another controller takes over");
            detachGamepad();
        }
        attachGamepad(device);
    }

    /**
     * Buttons and sticks are taken from whichever gamepad they come from. If that is not the
     * device taken for the controller, and it looks no less like one, it is the controller from
     * now on: its motion sensors, motors and light are the ones to use.
     */
    private void noteInputFrom(InputDevice device) {
        if (gamepad != null && device != null && device.getId() == gamepad.getId()) {
            return;
        }
        if (isGamepad(device) && (gamepad == null || rank(device) >= rank(gamepad))) {
            useGamepad(device);
        }
    }

    private void attachGamepad(InputDevice device) {
        gamepad = device;
        logInfo("controller: " + device.getName() + " (vendor " + device.getVendorId()
                + ", product " + device.getProductId() + ", sources 0x"
                + Integer.toHexString(device.getSources()) + ")");

        // Every part is optional: what a controller offers depends on it and on the system.
        try {
            SensorManager sensors = motion ? findSensors(device) : null;
            if (sensors != null) {
                sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER),
                        SensorManager.SENSOR_DELAY_FASTEST);
                sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE),
                        SensorManager.SENSOR_DELAY_FASTEST);
                controllerSensors = sensors;
            }
        } catch (RuntimeException e) {
            logWarning("no motion sensors", e);
        }
        try {
            VibratorManager vibrators = device.getVibratorManager();
            if (rumble && vibrators.getVibratorIds().length > 0) {
                controllerVibrators = vibrators;
            }
        } catch (RuntimeException e) {
            logWarning("no rumble", e);
        }
        try {
            LightsManager lights = device.getLightsManager();
            for (Light light : lights.getLights()) {
                if (light.hasRgbControl()) {
                    lightBar = light;
                    lightSession = lights.openSession();
                    break;
                }
            }
        } catch (RuntimeException e) {
            logWarning("no light bar", e);
        }
        logInfo("controller offers: motion " + (controllerSensors != null) + ", rumble "
                + (controllerVibrators != null) + ", light bar " + (lightBar != null)
                + ", touchpad " + device.supportsSource(InputDevice.SOURCE_TOUCHPAD));
        shownFeedback = -1;
    }

    /**
     * The motion sensors of a controller. The system normally presents them as part of the
     * gamepad; where it lists them as a device of their own, that one is found by its maker
     * and model.
     */
    private SensorManager findSensors(InputDevice device) {
        InputManager inputManager = getSystemService(InputManager.class);
        List<InputDevice> candidates = new ArrayList<>();
        candidates.add(device);
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice other = inputManager.getInputDevice(id);
            if (other != null && other.getId() != device.getId()
                    && other.getVendorId() == device.getVendorId()
                    && other.getProductId() == device.getProductId()) {
                candidates.add(other);
            }
        }
        for (InputDevice candidate : candidates) {
            SensorManager sensors = candidate.getSensorManager();
            if (sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null
                    && sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null) {
                return sensors;
            }
        }
        return null;
    }

    private void detachGamepad() {
        if (controllerSensors != null) {
            controllerSensors.unregisterListener(this);
            controllerSensors = null;
        }
        if (controllerVibrators != null) {
            try {
                controllerVibrators.cancel();
            } catch (RuntimeException e) {
                // The controller is gone, and its motors with it.
            }
            controllerVibrators = null;
        }
        if (lightSession != null) {
            try {
                lightSession.close();
            } catch (RuntimeException e) {
                // Likewise.
            }
            lightSession = null;
        }
        lightBar = null;
        gamepad = null;
    }

    /** Carries out what the game last asked of the controller: rumble and light bar colour. */
    private void applyFeedback() {
        long feedback = nativePadFeedback();
        long now = android.os.SystemClock.uptimeMillis();
        int small = (int) (feedback & 0xff);
        int large = (int) ((feedback >> 8) & 0xff);
        boolean changed = feedback != shownFeedback;

        if (controllerVibrators != null
                && (changed || ((small | large) != 0 && now >= rumbleRenewAt))) {
            try {
                int[] ids = controllerVibrators.getVibratorIds();
                if ((small | large) == 0) {
                    controllerVibrators.cancel();
                } else {
                    // The first motor is the heavy one, as the system lists them.
                    CombinedVibration.ParallelCombination both = CombinedVibration.startParallel();
                    if (ids.length == 1) {
                        both.addVibrator(ids[0], VibrationEffect.createOneShot(RUMBLE_MILLIS,
                                Math.max(small, large)));
                    } else {
                        if (large != 0) {
                            both.addVibrator(ids[0],
                                    VibrationEffect.createOneShot(RUMBLE_MILLIS, large));
                        }
                        if (small != 0) {
                            both.addVibrator(ids[1],
                                    VibrationEffect.createOneShot(RUMBLE_MILLIS, small));
                        }
                    }
                    controllerVibrators.vibrate(both.combine());
                    rumbleRenewAt = now + RUMBLE_RENEW_MILLIS;
                }
            } catch (RuntimeException e) {
                logWarning("rumble failed", e);
                controllerVibrators = null;
            }
        }

        if (changed && lightSession != null && lightBar != null) {
            int red = (int) ((feedback >> 16) & 0xff);
            int green = (int) ((feedback >> 24) & 0xff);
            int blue = (int) ((feedback >> 32) & 0xff);
            // Before the game has chosen a colour the bar keeps the one the system gave it.
            if ((red | green | blue) != 0) {
                try {
                    lightSession.requestLights(new LightsRequest.Builder()
                            .addLight(lightBar, new LightState.Builder()
                                    .setColor(Color.rgb(red, green, blue)).build())
                            .build());
                } catch (RuntimeException e) {
                    logWarning("light bar failed", e);
                    lightSession = null;
                }
            }
        }
        shownFeedback = feedback;
        handler.postDelayed(this::applyFeedback, 16);
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            System.arraycopy(event.values, 0, acceleration, 0, 3);
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            nativeMotion(event.values[0], event.values[1], event.values[2], acceleration[0],
                    acceleration[1], acceleration[2]);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    @Override
    public void onInputDeviceAdded(int deviceId) {
        // A controller that is switched on later takes over from a lesser one found before.
        findGamepad();
        adoptLateSensors();
    }

    /** The motion sensors of a controller may be listed a moment after the controller. */
    private void adoptLateSensors() {
        if (gamepad == null || controllerSensors != null || !motion) {
            return;
        }
        InputDevice device = getSystemService(InputManager.class).getInputDevice(gamepad.getId());
        if (device != null && findSensors(device) != null) {
            detachGamepad();
            attachGamepad(device);
        }
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        if (gamepad != null && gamepad.getId() == deviceId) {
            logInfo("controller gone");
            detachGamepad();
            // Nothing is held down on a controller that is gone.
            keyButtons = 0;
            hatButtons = 0;
            triggerButtons = 0;
            leftX = leftY = rightX = rightY = leftTrigger = rightTrigger = 0.0f;
            padTouchDown = false;
            padTouchClick = false;
            sendInput();
            findGamepad();
        }
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        adoptLateSensors();
    }
}
