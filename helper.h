#pragma once

#include <iostream>
#include <string>
#include <cstdint>
#include <vector>
#include <cstring>

// ---- OS Compatibility Headers ----
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int ssize_t;
typedef int socklen_t;
#define SHUT_WR SD_SEND
#define MSG_NOSIGNAL 0
#define SOCK_CLOEXEC 0
#define close(s) closesocket(s)
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

using namespace std;

// ---- Protocol & Networking Constants ----
const uint8_t STATUS_OK = 0;
const uint8_t STATUS_ERROR = 1;

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;

// ---- Data Structures ----
struct Variable
{
    char name[32];
    int32_t value;
};

struct Frame
{
    char func_name[32];
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

// ---- Standard CRC-32 Checksum Calculation ----
inline uint32_t calculateCRC32(const uint8_t* data, size_t length, uint32_t previousCrc = 0xFFFFFFFF)
{
    uint32_t crc = previousCrc;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc;
}

inline uint32_t finalizeCRC32(uint32_t crc)
{
    return crc ^ 0xFFFFFFFF;
}