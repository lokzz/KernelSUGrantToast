#!/system/bin/sh
KSUD=/data/adb/ksud
export KSU_MODULE=ksuGrantToast

#清理一些残存的状态
echo "Goodbye"

# Stop the toast process so the SuLog queue is released.
oldProcessPid=$(pidof SuToaster)
if [ "$oldProcessPid" ]; then
  kill -9 "$oldProcessPid" 2>/dev/null
fi
rm -f /data/adb/toast_ipc

# Hand SuLog back to ksud / the manager. While the module was installed it
# declared manage.sulog, so ksud skipped the feature during boot. Removing the
# declaration and enabling the feature again means normal SuLog keeps working
# (and is visible in the manager) after uninstalling.
"$KSUD" module config delete manage.sulog >/dev/null 2>&1
SULOG_STATUS=$("$KSUD" feature check sulog 2>/dev/null)
case "$SULOG_STATUS" in
  supported|forced)
    "$KSUD" feature set sulog 1 >/dev/null 2>&1
    "$KSUD" feature save >/dev/null 2>&1
    ;;
esac
