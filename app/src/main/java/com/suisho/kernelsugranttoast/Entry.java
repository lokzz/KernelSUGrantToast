package com.suisho.kernelsugranttoast;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.VibrationEffect;
import android.os.VibratorManager;
import android.util.Log;
import android.util.LruCache;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;

import com.suisho.kernelsugranttoast.object.SettingArguments;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.TimeUnit;


public class Entry {
    private static final Set<String> allowedSettingKeys = new HashSet<>(Arrays.asList("customToastText", "ignorePackageNames", "packageSearchDepth", "longTimeToast", "internalToastCooldown", "rootGrantVibrationTime"));
    public static final String TAG = "KernelSuGrantToast";
    private static Context systemContext;
    private static Handler handler;
    private static PackageManager packageManager;
    private static FileInputStream ipcInputStream;
    private static RandomAccessFile ipcPipe;
    private static VibratorManager vibratorManager = null;
    //缓存应用名 避免每次都走PackageManager
    private static final LruCache<String, String> appNameCache = new LruCache<>(32);
    private static String customToastText = Messages.getLocaleMessage();
    private static final HashSet<String> ignorePackageList = new HashSet<>();
    private static int toastShowTime = Toast.LENGTH_SHORT;
    private static short grantVibrationTime = 0;

    @SuppressLint("UnsafeDynamicallyLoadedCode")
    public static void main(String[] args) {
        if(Process.myUid() != 0) {
            Log.e(TAG, "Need root access!!!");
            return;
        }
        try {
            Log.i(TAG, "Using ksud: " + Ksud.getPath());
            SettingArguments tmpArgs = SettingArguments.initSettings(args, ignorePackageList);
            customToastText = tmpArgs.customToastText;
            toastShowTime = tmpArgs.longTimeToast ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT;
            grantVibrationTime = tmpArgs.rootGrantVibrationTime;
            HiddenApiBypass.addHiddenApiExemptions("Landroid/app/ActivityThread;");
            if(Looper.getMainLooper() == null) Looper.prepareMainLooper();
            @SuppressLint("PrivateApi") Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = HiddenApiBypass.invoke(activityThreadClass, null, "systemMain");
            Context context = (Context) HiddenApiBypass.invoke(activityThreadClass, activityThread, "getSystemContext");
            //uid0不能弹出toast
            systemContext = context.createPackageContext("android", 0);
            File localPath = new File("");
            File libraryFile = new File(localPath.getAbsolutePath(), "Shimizu");
            if(!libraryFile.exists()) {
                onInitFailed("Library file not found!Please reinstall module!!!");
                System.exit(1);
                return;
            }
            //确定没有崩掉再加载
            //app_process没法加载内置so
            System.load(libraryFile.getAbsolutePath());
            if(!jniInit(tmpArgs.packageSearchDepth, tmpArgs.autoDeleteLog, tmpArgs.enableDebugLogs, tmpArgs.toastCooldownTime)) {
                onInitFailed("Native init failed!");
                System.exit(1);
                return;
            }
            modifyModuleDescription(String.format(Locale.getDefault(), "✅Working.PID:%d", Process.myPid()));
            initSettingHotUpdateIpcListener();
            //降权 不然就是java.lang.SecurityException: Package android is not owned by uid 0
            //等写入描述完成才执行 系统框架没模块目录权限
            jniSetUid(1000);
            //KernelSU管理器最低要求Android版本12 刚好支持这个api
            //这要是崩了就自生自灭吧
            vibratorManager = (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            //刚启动时不知道为啥占用会达到130MB 调用以加速回落
            System.gc();
            Log.i(TAG, "Init success!");
            Looper.loop();
        } catch (Throwable e) {
            Log.e(TAG, "Crashed!", e);
            //重新提权 否则无法执行ksud
            if(Process.myUid() != 0) {
                jniSetUid(0);
            }
            onInitFailed("Crashed!");
            systemContext = null;
            System.exit(1);
        }
    }

    private static void showToast(String appName) {
        if(handler == null) handler = new Handler(Looper.getMainLooper());
        if(vibratorManager != null && grantVibrationTime > 0) {
            try {
                vibratorManager.getDefaultVibrator().vibrate(VibrationEffect.createOneShot(grantVibrationTime, VibrationEffect.EFFECT_HEAVY_CLICK));
            } catch (Throwable t) {
                Log.w(TAG, "Failed to vibrate", t);
            }
        }
        final String text = String.format(Locale.getDefault(), customToastText, appName);
        handler.post(() -> {
            // Some OEM Toast.makeText() implementations read Settings.System.
            // An app_process has no ActivityManager app record, so the settings
            // provider lookup throws SecurityException; never let that kill the
            // whole module, and fall back to our own overlay if needed.
            if(showFrameworkToast(text)) return;
            if(showOverlayToast(text)) return;
            Log.e(TAG, "Failed to show toast: " + text);
        });
    }

    private static boolean showFrameworkToast(String text) {
        // Empty constructor + setText skips makeText()'s OEM settings lookup
        // while still using the system text-toast renderer.
        try {
            Toast toast = new Toast(systemContext);
            toast.setText(text);
            toast.setDuration(toastShowTime);
            toast.show();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "Text toast via constructor failed", t);
        }
        // Older frameworks require makeText() + setView.
        try {
            Toast.makeText(systemContext, text, toastShowTime).show();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "Toast.makeText failed", t);
        }
        return false;
    }

    private static View overlayView = null;

    private static boolean showOverlayToast(String text) {
        try {
            WindowManager windowManager = (WindowManager) systemContext.getSystemService(Context.WINDOW_SERVICE);
            if(windowManager == null) return false;
            removeOverlayToast(windowManager);

            TextView view = new TextView(systemContext);
            view.setText(text);
            view.setTextColor(Color.WHITE);
            view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
            view.setPadding(dp(24), dp(12), dp(24), dp(12));
            GradientDrawable background = new GradientDrawable();
            background.setColor(0xCC000000);
            background.setCornerRadius(dp(24));
            view.setBackground(background);

            WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.TYPE_TOAST,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            params.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            params.y = dp(96);
            params.windowAnimations = android.R.style.Animation_Toast;
            windowManager.addView(view, params);
            overlayView = view;

            final View addedView = view;
            final long delay = toastShowTime == Toast.LENGTH_LONG ? 3500 : 2000;
            handler.postDelayed(() -> {
                try {
                    windowManager.removeView(addedView);
                } catch (Throwable ignored) {
                }
                if(overlayView == addedView) overlayView = null;
            }, delay);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "Overlay toast failed", t);
            return false;
        }
    }

    private static void removeOverlayToast(WindowManager windowManager) {
        if(overlayView == null) return;
        try {
            windowManager.removeView(overlayView);
        } catch (Throwable ignored) {
        }
        overlayView = null;
    }

    private static int dp(int value) {
        return Math.round(value * systemContext.getResources().getDisplayMetrics().density);
    }

    public static void jniOnFallbackSuEvent(String cmdline) {
        if(packageManager == null) packageManager = systemContext.getPackageManager();
        String packageName;
        if(cmdline.contains(":")) {
            int index = cmdline.indexOf(':');
            packageName = cmdline.substring(0, index);
        } else {
            packageName = cmdline;
        }
        //忽略提示的包名
        if(ignorePackageList.contains(packageName)) return;
        try {
            String cachedAppName = appNameCache.get(packageName);
            if(cachedAppName != null) {
                showToast(cachedAppName);
                return;
            }
            ApplicationInfo appInfo = packageManager.getApplicationInfo(packageName, 0);
            String appName = appInfo.loadLabel(packageManager).toString();
            appNameCache.put(packageName, appName);
            showToast(appName);
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "Failed to get app info", e);
        } catch (Exception | Error e) {
            Log.e(TAG, "Error on showing toast!", e);
            onFatalException("Error on showing toast!");
        }
    }

    public static void jniOnNewSuEvent(int uid, int ppid) {
        if(packageManager == null) packageManager = systemContext.getPackageManager();
        try {
            String[] appsList = packageManager.getPackagesForUid(uid);
            if(appsList == null || appsList.length == 0) {
                Log.i(TAG, "No package found for uid " + uid);
                return;
            }
            //sharedUserId处理 回退旧逻辑
            if(appsList.length > 1) {
                jniProcessSharedUidApplication(ppid);
                return;
            }
            String packageName = appsList[0];
            //忽略提示的包名
            if(ignorePackageList.contains(packageName)) return;
            String cachedAppName = appNameCache.get(packageName);
            if(cachedAppName != null) {
                showToast(cachedAppName);
                return;
            }
            ApplicationInfo appInfo = packageManager.getApplicationInfo(packageName, 0);
            String appName = appInfo.loadLabel(packageManager).toString();
            appNameCache.put(packageName, appName);
            showToast(appName);
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "Failed to get app info", e);
        } catch (Exception | Error e) {
            Log.e(TAG, "Error on showing toast!", e);
            onFatalException("Error on showing toast!");
        }
    }

    private static void onInitFailed(String errorMessage) {
        modifyModuleDescription("❌" + errorMessage);
    }

    private static void onFatalException(String msg) {
        jniSetUid(0);
        modifyModuleDescription("❌" + msg);
        System.exit(1);
    }

    private static void modifyModuleDescription(String descText) {
        //使用ksu特性临时更改描述
        try {
            String ksudPath = Ksud.getPath();
            File ksudFile = new File(ksudPath);
            if(!ksudFile.exists()) {
                Log.w(TAG, "ksud file not found!");
                return;
            }
            String desc = String.format(Locale.getDefault(), "[%s]Show a root granted toast like Magisk.", descText);
            ProcessBuilder processBuilder = new ProcessBuilder(ksudPath, "module", "config", "set", "--temp", "override.description", desc);
            processBuilder.environment().put("KSU_MODULE", "ksuGrantToast");
            java.lang.Process changeDescriptorProcess = processBuilder.start();
            //等待写入完成 方便降权
            changeDescriptorProcess.waitFor(20, TimeUnit.SECONDS);
            changeDescriptorProcess.destroyForcibly();
        } catch (IOException | InterruptedException e) {
            Log.e(TAG, "Failed to modify module description", e);
        }
    }

    private static void initSettingHotUpdateIpcListener() {
        File pipeFile = new File("/data/adb/toast_ipc");
        if(!pipeFile.exists()) {
            Log.w(TAG, "IPC pipe file not found!");
            return;
        }
        //自保留写端
        try {
            ipcPipe = new RandomAccessFile(pipeFile, "rw");
            ipcInputStream = new FileInputStream(ipcPipe.getFD());
        } catch (IOException e) {
            Log.e(TAG, "Failed to open IPC pipe file", e);
            onFatalException("Failed to open IPC pipe file");
        }
        new Thread(() -> {
            try {
                //等自己的写入端开启
                BufferedReader reader = new BufferedReader(new InputStreamReader(ipcInputStream, StandardCharsets.UTF_8));
                String line;
                Log.i(TAG, "Start ipc polling");
                while ((line = reader.readLine()) != null) {
                    Log.i(TAG, "Received IPC message: " + line);
                    //debug toast显示
                    if(line.equals("showDebugToast") || line.equals("showDebugToastLong")) {
                        if(handler == null) handler = new Handler(Looper.getMainLooper());
                        String finalLine = line;
                        handler.post(() -> Toast.makeText(systemContext, "If you can see this toast,the test will passed!", finalLine.equals("showDebugToastLong") ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT).show());
                        continue;
                    }
                    String[] splitMessage = line.split((char) 0x2 + " ", 2);
                    if(splitMessage.length != 2) {
                        Log.w(TAG, "Invalid IPC message format");
                        continue;
                    }
                    if(!allowedSettingKeys.contains(splitMessage[0])) {
                        Log.w(TAG, "Invalid setting key: " + splitMessage[0]);
                        continue;
                    }
                    if(!Util.checkConfigConfigValueValid(splitMessage[0], splitMessage[1])) {
                        //重置内容检测
                        if(splitMessage[0].equals("customToastText") && splitMessage[1].isEmpty()) {
                            customToastText = Messages.getLocaleMessage();
                            Log.i(TAG, "Custom toast text reset");
                            continue;
                        } else if(splitMessage[0].equals("ignorePackageNames") && splitMessage[1].isEmpty()) {
                            ignorePackageList.clear();
                            Log.i(TAG, "Ignore package list reset");
                            continue;
                        } else if(splitMessage[0].equals("packageSearchDepth") && splitMessage[1].isEmpty()) {
                            updatePackageSearchDepth((short) 1);
                            Log.i(TAG, "Package search depth reset");
                            continue;
                        } else if(splitMessage[0].equals("internalToastCooldown") && splitMessage[1].isEmpty()) {
                            updateInternalToastCooldown((short) 3);
                            Log.i(TAG, "Internal toast cooldown reset");
                            continue;
                        } else if(splitMessage[0].equals("rootGrantVibrationTime") && splitMessage[1].isEmpty()) {
                            grantVibrationTime = 0;
                            Log.i(TAG, "Root grant vibration time reset");
                            continue;
                        }
                        Log.w(TAG, "Invalid config value: " + splitMessage[1]);
                        continue;
                    }
                    switch (splitMessage[0]) {
                        case "customToastText":
                            customToastText = splitMessage[1];
                            Log.i(TAG, "Custom toast text updated");
                            break;
                        case "ignorePackageNames":
                            ignorePackageList.clear();
                            String[] rawSplit = splitMessage[1].split(";");
                            for(String packageName : rawSplit) {
                                if(!packageName.isEmpty()) ignorePackageList.add(packageName);
                            }
                            Log.i(TAG, "Ignore package list updated");
                            break;
                        case "packageSearchDepth":
                            updatePackageSearchDepth(Short.parseShort(splitMessage[1]));
                            Log.i(TAG, "Package search depth updated");
                            break;
                        case "longTimeToast":
                            boolean newValue = Boolean.parseBoolean(splitMessage[1]);
                            toastShowTime = newValue ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT;
                            Log.i(TAG, "Long time toast updated");
                            break;
                        case "internalToastCooldown":
                            updateInternalToastCooldown(Short.parseShort(splitMessage[1]));
                            Log.i(TAG, "Internal toast cooldown updated");
                            break;
                        case "rootGrantVibrationTime":
                            grantVibrationTime = Short.parseShort(splitMessage[1]);
                            Log.i(TAG, "Root grant vibration time updated");
                            break;
                        default:
                            //autoDeleteLog没有热更新的意义
                            Log.w(TAG, "Unknown setting key or key not supported hot update: " + splitMessage[0]);
                    }
                }
                Log.w(TAG, "Stop ipc polling");
            } catch (IOException e) {
                Log.e(TAG, "Failed to read IPC pipe file", e);
                onFatalException("Failed to read IPC pipe file");
            }
        }, "Setting IPC thread").start();
    }

    private static native boolean jniInit(short packageSearchDepth, boolean autoDeleteLog, boolean enableDebugLog, short toastCooldown);

    private static native void jniSetUid(int uid);

    private static native void jniProcessSharedUidApplication(int ppid);

    private static native void updatePackageSearchDepth(short value);

    private static native void updateInternalToastCooldown(short value);
}
