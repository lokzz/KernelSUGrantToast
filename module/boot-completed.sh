#!/system/bin/sh
KSUD=/data/adb/ksud
export KSU_MODULE=ksuGrantToast

fail() {
  "$KSUD" module config set --temp override.description "[❌$1]Show a root granted toast like Magisk."
  echo "Error: $1"
  exit 1
}

# KernelSU 3.x switched the module config store to the KSUM binary format.
# A leftover TOML persist.config breaks every `ksud module config` write, which
# in turn makes the WebUI unable to save anything. `get` swallows the error,
# `delete` (of a missing key) surfaces it, so use it as a probe. This mirrors
# the guard in customize.sh.
CONFIG_DIR=/data/adb/ksu/module_configs/$KSU_MODULE
CONFIG_ERROR=$("$KSUD" module config delete toastConfigProbe 2>&1)
case "$CONFIG_ERROR" in
  *"Invalid config magic"*|*"Unsupported config version"*|*"Invalid config version"*)
    echo "Detected a legacy module config, backing it up..."
    for CONFIG_FILE in persist.config tmp.config; do
      if [ -f "$CONFIG_DIR/$CONFIG_FILE" ]; then
        mv -f "$CONFIG_DIR/$CONFIG_FILE" "$CONFIG_DIR/$CONFIG_FILE.bak"
      fi
    done
    ;;
esac

# SuLog is the data source of this module. If the kernel does not provide it
# there is no point in starting the toast process at all.
SULOG_STATUS=$("$KSUD" feature check sulog 2>/dev/null)
if [ "$SULOG_STATUS" = "unsupported" ]; then
  fail "SuLog unsupported"
fi

# Kill the previous instance first. After a soft reboot (e.g.
# `setprop ctl.restart zygote`) the old process survives with dead Binder
# references and can no longer show toasts.
oldProcessPid=$(pidof SuToaster)
if [ "$oldProcessPid" ]; then
  kill -9 "$oldProcessPid" 2>/dev/null
  sleep 0.2
fi

# Take over SuLog.
#
# The kernel allows a single reader of the SuLog event queue. KernelSU keeps
# that reader inside ksud, while BakaSU runs a dedicated `sulogd` daemon
# (process name `ksud`, exec'd as `/proc/self/exe sulogd`). Enabling the
# feature may spawn that reader; the native side in Shimizu.so kills whoever
# currently owns the queue and claims it when it initialises.
#
# The manage.sulog declaration tells ksud that this module owns the feature:
# ksud then keeps the persisted value untouched and refuses changes from
# anything but this module, which prevents a second reader from respawning.
"$KSUD" module config delete manage.sulog >/dev/null 2>&1
if ! "$KSUD" feature set sulog 1; then
  # Some builds briefly fail right after boot while ksud/driver state settles.
  sleep 0.5
  "$KSUD" feature set sulog 1 || fail "Failed to enable SuLog"
fi
sleep 0.5

"$KSUD" module config set manage.sulog true >/dev/null 2>&1

# Create the configuration hot-update pipe.
rm -f /data/adb/toast_ipc
mkfifo /data/adb/toast_ipc

"$KSUD" module config set --temp override.description "[Booting...]Show a root granted toast like Magisk."
exec /system/bin/app_process -Djava.class.path=./daemon.dex / --nice-name=SuToaster com.suisho.kernelsugranttoast.Entry
