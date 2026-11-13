# KernelSU Grant Toast

<p align="center">
  <b>English</b> | 
  <a href="/README_zh.md">简体中文</a> | 
  <a href="/README_ru.md">Русский</a>
</p>
<hr>

##### Make KernelSU display a "Superuser access granted" toast, like Magisk.

### Screenshots

![](./mdAssets/1000023200.jpg)
![](./mdAssets/1000023198.jpg)
![](./mdAssets/1000023199.jpg)

### Features

- Shows a toast notification when an app gains elevated privileges.
- Supports custom notification text.
- Supports ignoring privilege-grant notifications for specified apps.

### Installation

Download the module package from the Releases page, then select and install it in KernelSU or BakaSU.

After installation, reboot the device for the module to take effect.

This module does not depend on Zygisk or MetaModule.

### Compatibility

- Official KernelSU, build 32457 or newer, with SuLog available.
- BakaSU (formerly ReSukiSU), current 4.x builds.
- Other KernelSU forks that keep the same SuLog event ABI and the
  `KSU_IOCTL_GET_SULOG_FD` ioctl.

The installer probes the kernel for the SuLog feature instead of trusting
`KSU_KERNEL_VER_CODE`, so a fork is not rejected just because it uses a
different version numbering.

BakaSU treats SuLog as a managed feature. The module declares
`managedFeatures=sulog`, takes over the `sulogd` reader, and keeps the
declaration so a second reader cannot respawn. While the module is installed
the manager's SuLog switch is managed by the module and cannot be toggled.

### How It Works

When Su logging is enabled, the root implementation starts a persistent reader
that receives log data forwarded by the kernel and writes it to a file.
KernelSU keeps that reader inside ksud; BakaSU runs it as a separate `sulogd`
daemon.

This data is highly real-time, making it fully suitable for event monitoring.

After the module is installed and the device has finished booting, the module
kills the process responsible for writing the logs (whichever process owns the
queue) and takes over the file descriptor used to receive the relevant data.
Only one process can hold that descriptor, so the original reader must be
terminated first.

If an Android app is found to have been granted root access, the module
retrieves information about the app and displays a notification when the
configured conditions are met.

### Notes

Because the process that originally writes the log file is terminated after the
device finishes booting, Su logs will no longer be recorded.

To prevent files from accumulating, the module also deletes old log files after
it starts.

As a result, you will no longer be able to view Su log data in the manager
while the module is active. Uninstalling the module restores SuLog
automatically.

(In theory, the original process could be left running and the module could
obtain information by monitoring changes to the log file, but this may have
poor performance.)

### Final Note

This module is mainly for fun.

Thanks for using it.
