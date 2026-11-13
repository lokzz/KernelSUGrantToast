#include <unistd.h>
#include "util.h"
#include "android/log.h"
#include "fcntl.h"
#include "sys/syscall.h"
#include "sys/ioctl.h"
#include "sys/stat.h"
#include "dirent.h"
#include "signal.h"
#include "vector"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

/* ------------------------------------------------------------------------- *
 * On-disk SuLog writer
 *
 * BakaSU runs `sulogd` to turn the kernel event queue into
 * /data/adb/ksu/log/sulog-YYYY-MM-DD[-N].log. This module takes the queue
 * over, so it has to write those files itself; otherwise the manager's
 * Su Log page (and the on-disk audit trail) goes dead. The line format below
 * is byte-for-byte the one sulogd produces.
 * ------------------------------------------------------------------------- */

#ifndef SULOG_ROOT_DIR
#define SULOG_ROOT_DIR "/data/adb/ksu/log"
#endif
#ifndef SULOG_MAX_FILE_SIZE_BYTES
#define SULOG_MAX_FILE_SIZE_BYTES (10ULL * 1024ULL * 1024ULL)
#endif
#ifndef SULOG_SESSION_STATE_PATH
#define SULOG_SESSION_STATE_PATH "/data/adb/ksu/.toaster_sulog_session"
#endif

static const char *SULOG_DIR = SULOG_ROOT_DIR;
static const uint64_t SULOG_MAX_FILE_SIZE = SULOG_MAX_FILE_SIZE_BYTES;
static const int SULOG_RETENTION_DAYS = 3;
static const size_t SULOG_LINE_MAX = 8192;

static int suLogWriterFd = -1;
static uint64_t suLogWriterSize = 0;
static int suLogWriterDay = -1;
static uint32_t suLogWriterIndex = 0;
static bool suLogWriterDisabled = false;
static time_t suLogLastFailedOpen = 0;

static int suLogLocalDay(time_t when) {
    struct tm tmv;
    if (localtime_r(&when, &tmv) == nullptr) return -1;
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

static void suLogLocalDate(time_t when, char out[11]) {
    struct tm tmv;
    if (localtime_r(&when, &tmv) == nullptr) {
        memcpy(out, "0000-00-00", 11);
        return;
    }
    snprintf(out, 11, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static std::string suLogFieldString(const char *data, size_t len) {
    if (data == nullptr || len == 0) return "";
    return std::string(data, strnlen(data, len));
}

// Matches sulogd's escape_field(): \\, \", \n, \r, \t, and \xNN for controls.
static void suLogAppendEscaped(std::string &out, const std::string &value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char) value[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out.push_back((char) c);
                }
        }
    }
}

std::string formatSuLogLine(const EventRecordHeader &record, const uint8_t *payload, size_t payloadLen) {
    if (payload == nullptr) return "";
    if (record.record_type == KSU_EVENT_TYPE_DROPPED) {
        if (payloadLen < 24) return "";
        uint64_t dropped = 0, firstSeq = 0, lastSeq = 0;
        memcpy(&dropped, payload, 8);
        memcpy(&firstSeq, payload + 8, 8);
        memcpy(&lastSeq, payload + 16, 8);
        char line[192];
        snprintf(line, sizeof(line),
                 "ts_ns=%llu seq=%llu type=dropped dropped=%llu first_seq=%llu last_seq=%llu",
                 (unsigned long long) record.ts_ns, (unsigned long long) record.seq,
                 (unsigned long long) dropped, (unsigned long long) firstSeq,
                 (unsigned long long) lastSeq);
        return std::string(line);
    }
    if (payloadLen < sizeof(SulogEventHeader)) return "";
    SulogEventHeader hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    const size_t fixed = sizeof(SulogEventHeader);
    if ((size_t) hdr.filename_len > payloadLen - fixed) return "";
    const size_t afterFile = fixed + hdr.filename_len;
    if ((size_t) hdr.argv_len > payloadLen - afterFile) return "";
    std::string comm = suLogFieldString(hdr.comm, sizeof(hdr.comm));
    std::string file = suLogFieldString(reinterpret_cast<const char *>(payload + fixed), hdr.filename_len);
    std::string argv = suLogFieldString(reinterpret_cast<const char *>(payload + afterFile), hdr.argv_len);
    const char *name = "unknown";
    switch (hdr.event_type) {
        case KSU_SULOG_EVENT_ROOT_EXECVE: name = "root_execve"; break;
        case KSU_SULOG_EVENT_SUCOMPAT: name = "sucompat"; break;
        case KSU_SULOG_EVENT_IOCTL_GRANT_ROOT: name = "ioctl_grant_root"; break;
        default: break;
    }
    char head[320];
    snprintf(head, sizeof(head),
             "ts_ns=%llu seq=%llu type=%s version=%u retval=%d pid=%u tgid=%u ppid=%u uid=%u euid=%u comm=\"",
             (unsigned long long) record.ts_ns, (unsigned long long) record.seq, name,
             (unsigned) hdr.version, (int) hdr.retval,
             (unsigned) hdr.pid, (unsigned) hdr.tgid, (unsigned) hdr.ppid,
             (unsigned) hdr.uid, (unsigned) hdr.euid);
    std::string line(head);
    suLogAppendEscaped(line, comm);
    line += "\" file=\"";
    suLogAppendEscaped(line, file);
    line += "\" argv=\"";
    suLogAppendEscaped(line, argv);
    line += "\"";
    if (line.size() > SULOG_LINE_MAX) line.resize(SULOG_LINE_MAX);
    return line;
}

static bool suLogWriterOpen(time_t now, uint32_t index) {
    char date[11];
    suLogLocalDate(now, date);
    char path[160];
    if (index == 0) {
        snprintf(path, sizeof(path), "%s/sulog-%s.log", SULOG_DIR, date);
    } else {
        snprintf(path, sizeof(path), "%s/sulog-%s-%u.log", SULOG_DIR, date, index);
    }
    uid_t currentUid = getuid();
    bool elevated = false;
    if (currentUid != 0) {
        if (setresuid(0, 0, 0) == 0) {
            elevated = true;
        } else {
            // The polling thread normally still has uid 0 as its saved uid.
            // If that fails, try the open anyway instead of stopping the log.
            LOGW("Failed to regain root to open %s, errno:%d", path, errno);
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    int openErrno = errno;
    uint64_t size = 0;
    if (fd >= 0) {
        fchmod(fd, 0600);
        struct stat st;
        if (fstat(fd, &st) == 0) size = (uint64_t) st.st_size;
    }
    if (elevated) {
        setresuid(currentUid, currentUid, 0);
    }
    if (fd < 0) {
        LOGW("Failed to open SuLog file %s, errno:%d", path, openErrno);
        suLogLastFailedOpen = time(nullptr);
        return false;
    }
    if (suLogWriterFd >= 0) close(suLogWriterFd);
    suLogWriterFd = fd;
    suLogWriterSize = size;
    suLogWriterDay = suLogLocalDay(now);
    suLogWriterIndex = index;
    suLogLastFailedOpen = 0;
    LOGI("Opened SuLog file %s (size=%llu)", path, (unsigned long long) size);
    return true;
}

static void suLogCleanupExpired(time_t now) {
    char cutoff[11];
    suLogLocalDate(now - (time_t) SULOG_RETENTION_DAYS * 86400, cutoff);
    DIR *dir = opendir(SULOG_DIR);
    if (!dir) return;
    while (struct dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        if (strncmp(name, "sulog-", 6) != 0) continue;
        size_t len = strlen(name);
        if (len < 20 || strcmp(name + len - 4, ".log") != 0) continue;
        if (strncmp(name + 6, cutoff, 10) < 0) {
            string path = string(SULOG_DIR) + "/" + name;
            if (unlink(path.c_str()) == 0) {
                LOGI("Removed expired SuLog %s", path.c_str());
            }
        }
    }
    closedir(dir);
}

// Returns false on a hard write error (non-EINTR).
static bool suLogWriteAll(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t) n;
    }
    return true;
}

static void suLogWriteRaw(const char *data, size_t len) {
    if (suLogWriterFd < 0) return;
    if (!suLogWriteAll(suLogWriterFd, data, len)) {
        LOGW("SuLog write failed, errno:%d; disabling disk log", errno);
        close(suLogWriterFd);
        suLogWriterFd = -1;
        suLogWriterDisabled = true;
        return;
    }
    suLogWriterSize += len;
}

bool initSuLogWriter() {
    if (suLogWriterDisabled) return false;
    mkdir(SULOG_DIR, 0700);
    time_t now = time(nullptr);
    if (now == (time_t) -1) return false;
    suLogCleanupExpired(now);

    // Continue today's highest index; rotate once it has reached the limit.
    char date[11];
    suLogLocalDate(now, date);
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "sulog-%s", date);
    const size_t prefixLen = strlen(prefix);
    uint32_t highestIndex = 0;
    bool found = false;
    DIR *dir = opendir(SULOG_DIR);
    if (dir != nullptr) {
        while (struct dirent *entry = readdir(dir)) {
            const char *name = entry->d_name;
            if (strncmp(name, prefix, prefixLen) != 0) continue;
            const char *rest = name + prefixLen;
            uint32_t index = 0;
            if (rest[0] == '-') {
                char *end = nullptr;
                unsigned long value = strtoul(rest + 1, &end, 10);
                if (end == rest + 1 || strcmp(end, ".log") != 0) continue;
                index = (uint32_t) value;
            } else if (strcmp(rest, ".log") != 0) {
                continue;
            }
            if (!found || index > highestIndex) {
                found = true;
                highestIndex = index;
            }
        }
        closedir(dir);
    }
    uint32_t index = found ? highestIndex : 0;
    if (found) {
        char path[160];
        if (index == 0) {
            snprintf(path, sizeof(path), "%s/sulog-%s.log", SULOG_DIR, date);
        } else {
            snprintf(path, sizeof(path), "%s/sulog-%s-%u.log", SULOG_DIR, date, index);
        }
        struct stat st;
        if (stat(path, &st) == 0 && (uint64_t) st.st_size >= SULOG_MAX_FILE_SIZE) {
            index++;
        }
    }
    if (!suLogWriterOpen(now, index)) return false;

    // Session marker, matching sulogd. Reuse the previous boot id to emit a
    // daemon_restart with a counter: the manager keys daemon events by
    // (restart, boot_id), so two identical daemon_start lines in one file
    // would collide in its list.
    std::string bootId;
    if (readProcFile("/proc/sys/kernel/random/boot_id", bootId)) {
        while (!bootId.empty() &&
               (bootId.back() == '\n' || bootId.back() == '\r' || bootId.back() == ' ')) {
            bootId.pop_back();
        }
    }
    uint64_t restart = 0;
    std::string previousState;
    if (readProcFile(SULOG_SESSION_STATE_PATH, previousState)) {
        size_t space = previousState.find(' ');
        if (space != std::string::npos) {
            std::string previousBoot = previousState.substr(0, space);
            if (previousBoot == bootId) {
                restart = (uint64_t) strtoull(previousState.c_str() + space + 1, nullptr, 10) + 1;
            }
        }
    }
    std::string marker = restart == 0 ? "type=daemon_start boot_id=\"" : "type=daemon_restart boot_id=\"";
    suLogAppendEscaped(marker, bootId);
    marker += "\"";
    if (restart != 0) {
        marker += " restart=" + std::to_string(restart);
    }
    marker += "\n";
    suLogWriteRaw(marker.data(), marker.size());

    std::string state = bootId + " " + std::to_string(restart) + "\n";
    int stateFd = open(SULOG_SESSION_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (stateFd >= 0) {
        suLogWriteAll(stateFd, state.data(), state.size());
        close(stateFd);
    }
    LOGI("SuLog writer started (%s), restart=%llu", SULOG_DIR, (unsigned long long) restart);
    return true;
}

void closeSuLogWriter() {
    if (suLogWriterFd >= 0) {
        close(suLogWriterFd);
        suLogWriterFd = -1;
    }
}

void writeSuLogRecord(const EventRecordHeader &record, const uint8_t *payload, size_t payloadLen) {
    if (suLogWriterFd < 0 || suLogWriterDisabled) return;
    std::string line = formatSuLogLine(record, payload, payloadLen);
    if (line.empty()) return;
    line += '\n';

    time_t now = time(nullptr);
    int day = suLogLocalDay(now);
    // Rotate before the write, like sulogd's rotate_if_needed(next_write_len).
    if (day != suLogWriterDay ||
        (suLogWriterSize > 0 && suLogWriterSize + line.size() > SULOG_MAX_FILE_SIZE)) {
        // Retry failed opens at most once a minute so a full disk cannot spam open().
        if (suLogLastFailedOpen == 0 || now - suLogLastFailedOpen >= 60) {
            uint32_t nextIndex = (day != suLogWriterDay) ? 0 : suLogWriterIndex + 1;
            suLogWriterOpen(now, nextIndex);
        }
    }
    suLogWriteRaw(line.data(), line.size());
}
