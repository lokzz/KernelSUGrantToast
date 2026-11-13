#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/types.h>

#ifndef KERNELSUGRANTTOAST_UTIL_H
#define KERNELSUGRANTTOAST_UTIL_H
#pragma once

// SuLog UAPI constants/structure layout shared with KernelSU and BakaSU.
#define TASK_COMM_LEN 16
#define KSU_EVENT_TYPE_DROPPED 0xFFFFu
#define KSU_SULOG_EVENT_ROOT_EXECVE 1u
#define KSU_SULOG_EVENT_SUCOMPAT 2u
#define KSU_SULOG_EVENT_IOCTL_GRANT_ROOT 3u

struct __attribute__((packed)) EventRecordHeader {
    uint16_t record_type;
    uint16_t flags;
    uint32_t payload_len;
    uint64_t seq;
    uint64_t ts_ns;
};

struct __attribute__((packed)) SulogEventHeader {
    uint16_t version;
    uint16_t event_type;
    int32_t retval;
    uint32_t pid, tgid, ppid, uid, euid;
    char comm[TASK_COMM_LEN];
    uint32_t filename_len, argv_len;
};

typedef struct AndroidAppInfo {
    bool isAndroidApp;
    pid_t realPid;
    std::string cmdline;
} AndroidAppInfo;

bool utilInit(bool enableDebugLog);

bool tryKillFdOwnerProcess();

void killSpareSulogdProcesses();

int getKernelSuDriver();

int getSuLogFd(int driverFd);

void deleteSuLogFile();

void appendLog(const std::string& log);
AndroidAppInfo queryAndroidApplicationInfo(pid_t pid,short depth);

/*
 * Recreates the stock SuLog writer. BakaSU normally runs `sulogd` to write the
 * event queue to /data/adb/ksu/log/sulog-*.log; this module takes the queue
 * over, so it has to keep writing those files itself to preserve the manager's
 * Su Log page and the on-disk audit trail.
 */
bool initSuLogWriter();

void closeSuLogWriter();

// Formats one record in the exact format used by BakaSU's sulogd. Returns an
// empty string when the record is malformed.
std::string formatSuLogLine(const EventRecordHeader &record, const uint8_t *payload, size_t payloadLen);

void writeSuLogRecord(const EventRecordHeader &record, const uint8_t *payload, size_t payloadLen);

#endif //KERNELSUGRANTTOAST_UTIL_H
