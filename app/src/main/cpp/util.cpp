#include <unistd.h>
#include "util.h"
#include "android/log.h"
#include "fcntl.h"
#include "sys/syscall.h"
#include "sys/ioctl.h"
#include "dirent.h"
#include "signal.h"
#include "vector"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;
#define KSU_INSTALL_MAGIC1 0xDEADBEEFu
#define KSU_INSTALL_MAGIC2 0xCAFEBABEu
#define KSU_IOCTL_GET_SULOG_FD 0x40044B14u
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KernelSuGrantToast", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "KernelSuGrantToast", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KernelSuGrantToast", __VA_ARGS__)
static int zygotePid = -886;
static int zygote64Pid = -996;
FILE *logFile;
static uint8_t writeLogLineCount = 0;
static bool enableDebugLog = false;

bool readProcFile(const std::string &path, std::string &out) {

    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    string tmpString;
    tmpString.resize(1536);
    ssize_t readLength;
    while (true) {
        readLength = read(fd, tmpString.data(), tmpString.size());
        if (readLength < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    if (readLength <= 0) {
        out.clear();
        return false;
    }
    out.assign(tmpString.data(), readLength);
    return true;
}

bool isNumeric(const char *name) {
    if (!name || *name == '\0') return false;
    for (const char *p = name; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

void appendLog(const std::string &log) {
    if (logFile == nullptr || !enableDebugLog) return;
    fprintf(logFile, "%s\n", log.c_str());
    if (++writeLogLineCount % 5 == 0) {
        fflush(logFile);
    }
}

/*
 * The kernel exposes the SuLog event queue through an anon inode. KernelSU and
 * BakaSU name it "anon_inode:[ksu_sulog]"; downstream forks occasionally
 * rename it, so match the anon_inode prefix plus the feature name instead of
 * hardcoding one spelling.
 * */
static bool isSulogFdLink(const char *link) {
    if (link == nullptr) return false;
    if (strncmp(link, "anon_inode:[", 12) != 0) return false;
    return strstr(link, "sulog") != nullptr;
}

/*
 * Only one process may hold the SuLog fd. Find everyone who currently does.
 * */
static vector<pid_t> findSulogFdOwnerPids() {
    vector<pid_t> owners;
    DIR *procDir = opendir("/proc");
    if (!procDir) return owners;
    const pid_t self = getpid();
    while (dirent *procEntry = readdir(procDir)) {
        if (!isNumeric(procEntry->d_name)) continue;
        auto pid = static_cast<pid_t>(strtol(procEntry->d_name, nullptr, 10));
        if (pid <= 1 || pid == self) continue;
        string fdDirPath = "/proc/" + string(procEntry->d_name) + "/fd";
        DIR *fdDir = opendir(fdDirPath.c_str());
        if (!fdDir) continue;
        bool owned = false;
        while (dirent *fdEntry = readdir(fdDir)) {
            if (!isNumeric(fdEntry->d_name)) continue;
            char link[128];
            ssize_t readLength = readlinkat(dirfd(fdDir), fdEntry->d_name, link, sizeof(link) - 1);
            if (readLength <= 0) continue;
            link[readLength] = '\0';
            if (isSulogFdLink(link)) {
                owned = true;
                break;
            }
        }
        closedir(fdDir);
        if (owned) owners.push_back(pid);
    }
    closedir(procDir);
    return owners;
}

inline string getProcessCmdline(pid_t pid) {
    string statFilePath = "/proc/" + to_string(pid) + "/cmdline";
    string cmdline;
    if (readProcFile(statFilePath, cmdline)) {
        size_t nullPos = cmdline.find('\0');
        if (nullPos != string::npos) {
            cmdline = cmdline.substr(0, nullPos);
        }
        return cmdline;
    }
    return "";
}

static pid_t findPidByCmdline(const string &name) {
    DIR *procDir = opendir("/proc");
    if (!procDir) return -1;
    while (dirent *procEntry = readdir(procDir)) {
        if (!isNumeric(procEntry->d_name)) continue;
        auto pid = static_cast<pid_t>(strtol(procEntry->d_name, nullptr, 10));
        if (pid <= 1) continue;
        if (getProcessCmdline(pid) == name) {
            closedir(procDir);
            return pid;
        }
    }
    closedir(procDir);
    return -1;
}

inline pid_t getPidByName(const string &name) {
    FILE *result = popen(("pidof " + name).c_str(), "r");
    if (result != nullptr) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), result) != nullptr) {
            pclose(result);
            auto pid = static_cast<pid_t>(strtol(buf, nullptr, 10));
            if (pid > 1) return pid;
        } else {
            pclose(result);
        }
    }
    // pidof is missing or failed; walk /proc instead.
    return findPidByCmdline(name);
}

bool utilInit(bool useDebugLog) {
    if (useDebugLog) {
        enableDebugLog = useDebugLog;
        logFile = fopen("/data/local/SuToaster.log", "w");
        if (logFile == nullptr) {
            LOGW("Failed to open module log file");
        }
    }
    zygotePid = getPidByName("zygote");
    zygote64Pid = getPidByName("zygote64");
    appendLog("Zygote pid:" + to_string(zygotePid) + " zygote64 pid:" + to_string(zygote64Pid));
    return zygotePid > 1 || zygote64Pid > 1;
}

bool tryKillFdOwnerProcess() {
    /*
     * 开启sulog后启动的ksud进程名
     * 如果是启动后才开启该功能 进程名会不同
     * 但我选择初始化时如果sulog未开启直接退出
     * */
    vector<pid_t> owners = findSulogFdOwnerPids();
    if (owners.empty()) {
        LOGW("No process currently holds the SuLog fd");
        return false;
    }
    bool signalSent = false;
    for (pid_t pid : owners) {
        LOGI("Killing SuLog fd owner pid:%d", pid);
        if (kill(pid, SIGKILL) == 0) {
            signalSent = true;
        } else {
            LOGW("Failed to kill SuLog fd owner pid:%d, errno:%d", pid, errno);
        }
    }
    if (!signalSent) return false;
    //等待退出 最多1.5秒
    for (int i = 0; i < 75; ++i) {
        bool anyAlive = false;
        for (pid_t pid : owners) {
            if (kill(pid, 0) == 0 || errno != ESRCH) {
                anyAlive = true;
                break;
            }
        }
        if (!anyAlive) return true;
        usleep(20000);
    }
    LOGW("SuLog fd owner is still alive after SIGKILL");
    // The fd is released as soon as the process dies; retry the ioctl anyway.
    return true;
}

void killSpareSulogdProcesses() {
    /*
     * BakaSU runs the SuLog reader as a dedicated `sulogd` daemon (exec'd via
     * /proc/self/exe). After this module takes the queue over, a surviving
     * sulogd can never obtain the fd and would retry forever, so stop it.
     * */
    DIR *procDir = opendir("/proc");
    if (!procDir) return;
    const pid_t self = getpid();
    while (dirent *procEntry = readdir(procDir)) {
        if (!isNumeric(procEntry->d_name)) continue;
        auto pid = static_cast<pid_t>(strtol(procEntry->d_name, nullptr, 10));
        if (pid <= 1 || pid == self) continue;
        string raw;
        if (!readProcFile("/proc/" + to_string(pid) + "/cmdline", raw)) continue;
        bool isSulogd = false;
        size_t start = 0;
        while (start < raw.size()) {
            size_t end = raw.find('\0', start);
            if (end == string::npos) end = raw.size();
            if (end - start == 6 && memcmp(raw.data() + start, "sulogd", 6) == 0) {
                isSulogd = true;
                break;
            }
            start = end + 1;
        }
        if (!isSulogd) continue;
        LOGI("Killing spare sulogd pid:%d", pid);
        kill(pid, SIGKILL);
    }
    closedir(procDir);
}

int getKernelSuDriver() {
    int fd = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
    return fd;
}

int getSuLogFd(int driverFd) {
    struct {
        uint32_t flags;
    } suLog_cmd = {0};
    for (int attempt = 0; attempt < 4; ++attempt) {
        errno = 0;
        int fd = ioctl(driverFd, KSU_IOCTL_GET_SULOG_FD, &suLog_cmd);
        if (fd >= 0) return fd;
        int err = errno;
        if (err == EINTR) continue;
        if (err != EBUSY) {
            LOGE("Get Su log fd failed, errno:%d", err);
            return -1;
        }
        LOGW("SuLog fd is busy (attempt %d), releasing the current reader", attempt + 1);
        tryKillFdOwnerProcess();
        usleep(150000);
    }
    LOGE("Get Su log fd failed: still busy after retries");
    return -1;
}

pid_t getPpid(pid_t pid) {
    string statFilePath = "/proc/" + to_string(pid) + "/stat";
    string line;
    if (readProcFile(statFilePath, line)) {
        size_t afterComm = line.find(") ");
        if (afterComm == string::npos) return -1;
        size_t statePos = afterComm + 2;
        size_t stateEnd = line.find(' ', statePos);
        if (stateEnd == string::npos) return -1;
        size_t ppidStart = stateEnd + 1;
        size_t ppidEnd = line.find(' ', ppidStart);
        string ppidStr = (ppidEnd == string::npos) ? line.substr(ppidStart) : line.substr(ppidStart, ppidEnd - ppidStart);
        char *end = nullptr;
        long v = strtol(ppidStr.c_str(), &end, 10);
        if (end == ppidStr.c_str() || *end != '\0') return -1;
        return static_cast<pid_t>(v);
    }
    return -1;
}

AndroidAppInfo queryAndroidApplicationInfo(pid_t pid, short depth) {
    appendLog("Querying android app info for pid:" + to_string(pid));
    pid_t targetPpid = getPpid(pid);
    //不可能有Android应用pid小于100
    if (targetPpid < 100) {
        appendLog("Target pid less than 100,not an android app");
        return {false, pid, ""};
    }
    bool parentIsZygote = targetPpid == zygotePid || targetPpid == zygote64Pid;
    //尝试深度搜索
    if (!parentIsZygote && depth > 0) {
        appendLog("Trying to find android app info with depth:" + to_string(depth));
        pid_t currentProcessPpid = targetPpid;
        pid_t newTargetPpid = targetPpid;
        bool parentPpidIsZygote = parentIsZygote;
        for (short i = 0; i < depth; ++i) {
            currentProcessPpid = newTargetPpid;
            newTargetPpid = getPpid(newTargetPpid);
            parentPpidIsZygote = newTargetPpid == zygotePid || newTargetPpid == zygote64Pid;
            if (parentPpidIsZygote || newTargetPpid < 100) break;
        }
        appendLog("Found android app info with depth:" + to_string(depth) +
                  " ppid:" + to_string(currentProcessPpid) +
                  " cmdline:" + getProcessCmdline(currentProcessPpid));
        return {parentPpidIsZygote, currentProcessPpid, parentPpidIsZygote ? getProcessCmdline(currentProcessPpid) : ""};
    }
    //是android应用了 再加个包名
    appendLog("Found android app info without depth ppid:" + to_string(pid) +
              " cmdline:" + getProcessCmdline(pid));
    return {parentIsZygote, pid, parentIsZygote ? getProcessCmdline(pid) : ""};
}

void deleteSuLogFile() {
    DIR *dir = opendir("/data/adb/ksu/log");
    if (!dir) {
        LOGW("Failed to open KernelSU log directory!");
        return;
    }
    while (struct dirent *entry = readdir(dir)) {
        if (entry->d_type != DT_REG) continue;
        if (strstr(entry->d_name, "sulog-") != nullptr &&
            strstr(entry->d_name, ".log") != nullptr) {
            string path = "/data/adb/ksu/log/" + string(entry->d_name);
            if (unlink(path.c_str()) < 0) {
                LOGW("Failed to delete %s", path.c_str());
            }
        }
    }
    closedir(dir);
}
