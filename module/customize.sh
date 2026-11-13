#!/system/bin/sh
KSUD=/data/adb/ksud
export KSU_MODULE=ksuGrantToast

echo "Welcome"

# This module is built on top of the SuLog kernel interface. Both KernelSU and
# BakaSU (formerly ReSukiSU) export it, but BakaSU does not follow KernelSU's
# version numbering, so never rely on KSU_KERNEL_VER_CODE alone.
if [ ! "$KSU" ]; then
  abort "This module only supports KernelSU / BakaSU!"
fi
if [ "$ARCH" != "arm64" ]; then
  abort "This module only supports arm64 devices!"
fi

IS_BAKASU=
if [ -n "$BAKASU" ]; then
  IS_BAKASU=1
fi
# Some BakaSU builds only export KSU_VER; detect the name as a fallback.
if [ -z "$IS_BAKASU" ] && [ -n "$KSU_VER" ]; then
  case "$KSU_VER" in
    *[Bb][Aa][Kk][Aa][Ss][Uu]*) IS_BAKASU=1 ;;
  esac
fi

if [ "$IS_BAKASU" ]; then
  echo "BakaSU detected (version: ${KSU_VER:-unknown})"
elif [ "$KSU_KERNEL_VER_CODE" -lt 32457 ]; then
  abort "Please update KernelSU! (Minimum version required: 32457)"
fi

# Probe the actual feature instead of guessing from a version code. Forks
# renumber freely and some are built from shallow checkouts, which makes
# KSU_KERNEL_VER_CODE unreliable.
SULOG_STATUS=$("$KSUD" feature check sulog 2>/dev/null)
case "$SULOG_STATUS" in
  unsupported)
    abort "SuLog is not supported by this kernel build!"
    ;;
  "")
    echo "Warning: unable to query SuLog support, continuing anyway"
    ;;
  *)
    echo "SuLog status: $SULOG_STATUS"
    ;;
esac

# KernelSU 3.x switched the module config store from TOML text to the KSUM
# binary format. A leftover TOML persist.config makes `ksud module config`
# fail with "Invalid config magic", so the WebUI can neither read nor save
# settings. `get` swallows that error, `delete` (of a missing key) surfaces it,
# so use it as a side-effect-free probe. Back the old file up and let ksud
# recreate it.
CONFIG_DIR=/data/adb/ksu/module_configs/$KSU_MODULE
CONFIG_ERROR=$("$KSUD" module config delete toastConfigProbe 2>&1)
case "$CONFIG_ERROR" in
  *"Invalid config magic"*|*"Unsupported config version"*|*"Invalid config version"*)
    echo "Detected a legacy module config, backing it up..."
    for CONFIG_FILE in persist.config tmp.config; do
      if [ -f "$CONFIG_DIR/$CONFIG_FILE" ]; then
        mv -f "$CONFIG_DIR/$CONFIG_FILE" "$CONFIG_DIR/$CONFIG_FILE.toml.bak"
      fi
    done
    ;;
esac

# Stop a stale instance from a previous install or a soft reboot.
oldProcessPid=$(pidof SuToaster)
if [ "$oldProcessPid" ]; then
  echo "Killing old process..."
  kill -9 "$oldProcessPid"
fi

# Keep the warning bilingual, it is important.
echo "该项目已在官方KernelSU与BakaSU上进行测试"
echo "其他分支版本理论上可用 但未经验证"
echo "This project is tested on official KernelSU and BakaSU"
echo "Other forks should work but are not officially tested"
sleep 3
echo "警告:"
echo "由于该模块接管了原本日志进程的数据"
echo "在安装后原本的Su日志将不再被记录"
echo "也无法在管理器中查看Su日志"
echo "这是正常现象 不必惊慌"
echo "安装将在5秒后继续"
echo "WARNING:"
echo "Because this module takes over data from the logger process"
echo "The original SuLog will no longer be recorded"
echo "SuLog can no longer be viewed in the manager"
echo "This is normal. No need to be alarmed"
echo "Module installation will continue in 5 seconds"
sleep 5
echo "Extracting libraries..."
#sleep没什么意义 但好玩
sleep 0.3
#so
unzip -oj "$MODPATH/daemon.apk" 'lib/arm64-v8a/libshimizu.so' -d "$MODPATH" || abort "Failed to extract the native library!"
mv -f "$MODPATH/libshimizu.so" "$MODPATH/Shimizu"
#dex
sleep 0.2
unzip -oj "$MODPATH/daemon.apk" 'classes.dex' -d "$MODPATH" || abort "Failed to extract the dex file!"
mv -f "$MODPATH/classes.dex" "$MODPATH/daemon.dex"
sleep 0.2
set_perm "$MODPATH/Shimizu" 0 0 0755
set_perm "$MODPATH/daemon.dex" 0 0 0755
rm -f "$MODPATH/daemon.apk"
echo "Setting description..."
"$KSUD" module config set --temp override.description "[Waiting reboot]Show a root granted toast like Magisk."
echo "Installation successful!"
echo "Please reboot for changes to take effect"
