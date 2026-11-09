// ======================= TIME-TRAVEL DEBUGGER - CLIENT =======================

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sstream>

// OS Compatibility Block
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int ssize_t;
#define SHUT_WR SD_SEND
#define MSG_NOSIGNAL 0
#define close(s) closesocket(s)
#else
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "helper.h"
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;
const char* TDBG_FILE = "ttdb_last.tdbg";
const int MAX_JUMP = 1000000000;
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// ---- Structs for Client Session ----
struct TdbgReader
{
    FILE* file;
    TTDBHeader header;
    vector<int64_t> index;
    TdbgReader() : file(nullptr) {}
};

struct DebugSession
{
    bool active;
    TdbgReader reader;
    int64_t currentStep;
    DebugSession() : active(false), currentStep(0) {}
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n)
{
    size_t totalRead = 0;
    while (totalRead < n)
    {
        ssize_t readBytes = recv(sock, buf + totalRead, n - totalRead, 0);
        if (readBytes <= 0) return false;
        totalRead += readBytes;
    }
    return true;
}

bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalSent = 0;
    while (totalSent < n)
    {
        ssize_t sent = send(sock, buf + totalSent, n - totalSent, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

int connectToHost(const char* host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

bool uploadSource(int sock, const char* sourcePath)
{
    FILE* f = fopen(sourcePath, "rb");
    if (!f) return false;

    char buffer[IO_BUFFER_SIZE];
    size_t bytesRead = 0;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        if (!sendAll(sock, buffer, bytesRead))
        {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    shutdown(sock, SHUT_WR);
    return true;
}

bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    uint8_t status = 0;
    if (!recvAll(sock, (char*)&status, 1))
    {
        outError = "Failed to receive response status";
        return false;
    }

    uint32_t runningCrc = calculateCRC32(&status, 1);
    vector<uint8_t> bodyBuffer;
    char chunk[4096];

    while (true)
    {
        ssize_t bytesRead = recv(sock, chunk, sizeof(chunk), 0);
        if (bytesRead <= 0) break;
        bodyBuffer.insert(bodyBuffer.end(), chunk, chunk + bytesRead);
    }

    if (bodyBuffer.size() < 4)
    {
        outError = "Response too short, corrupted stream";
        return false;
    }

    size_t payloadSize = bodyBuffer.size() - 4;
    uint32_t receivedCrc = 0;
    memcpy(&receivedCrc, bodyBuffer.data() + payloadSize, 4);

    runningCrc = calculateCRC32(bodyBuffer.data(), payloadSize, runningCrc);
    uint32_t computedCrc = finalizeCRC32(runningCrc);

    if (computedCrc != receivedCrc)
    {
        outError = "checksum mismatch, response corrupted";
        return false;
    }

    if (status == STATUS_ERROR)
    {
        outError.assign((char*)bodyBuffer.data(), payloadSize);
        return false;
    }

    FILE* out = fopen(outTdbgPath, "wb");
    if (!out)
    {
        outError = "Failed to write local .tdbg file";
        return false;
    }
    fwrite(bodyBuffer.data(), 1, payloadSize, out);
    fclose(out);

    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// =============================================================================================

bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    if (!r.file) return false;

    if (fread(r.header.magic, 1, 4, r.file) != 4
        || fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1
        || fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1
        || fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1
        || r.header.stepCount <= 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (memcmp(r.header.magic, "TTDB", 4) != 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    r.index.resize(r.header.stepCount);
    if (fseek(r.file, r.header.indexOffset, SEEK_SET) != 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (fread(r.index.data(), sizeof(int64_t), r.header.stepCount, r.file) != (size_t)r.header.stepCount)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    return true;
}

void closeTdbg(TdbgReader& r)
{
    if (r.file)
    {
        fclose(r.file);
        r.file = nullptr;
    }
    r.index.clear();
}

static bool readStr(FILE* f, char* dst, size_t cap)
{
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    if (len > 1024 * 1024) return false;
    size_t keep = (len < cap - 1) ? len : cap - 1;
    if (keep > 0 && fread(dst, 1, keep, f) != keep) return false;
    dst[keep] = '\0';
    if (len > keep && fseek(f, (long)(len - keep), SEEK_CUR) != 0) return false;
    return true;
}

static bool readVar(FILE* f, Variable& v)
{
    if (!readStr(f, v.name, sizeof(v.name))) return false;
    return fread(&v.value, sizeof(int32_t), 1, f) == 1;
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (!r.file || step < 0 || step >= r.header.stepCount) return nullptr;
    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0) return nullptr;

    Snapshot* s = new Snapshot();
    memset(s, 0, sizeof(Snapshot));
    FILE* f = r.file;
    bool ok = fread(&s->stackDepth, sizeof(int32_t), 1, f) == 1
        && s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;

    for (int i = 0; ok && i < s->stackDepth; ++i)
    {
        Frame& fr = s->callStack[i];
        ok = readStr(f, fr.func_name, sizeof(fr.func_name))
            && fread(&fr.argc, sizeof(int32_t), 1, f) == 1
            && fr.argc >= 0 && fr.argc <= MAX_VARS_PER_FRAME;
        for (int a = 0; ok && a < fr.argc; ++a) 
            ok = readVar(f, fr.argv[a]);
        ok = ok && fread(&fr.returnLine, sizeof(int32_t), 1, f) == 1
            && fread(&fr.localCount, sizeof(int32_t), 1, f) == 1
            && fr.localCount >= 0 && fr.localCount <= MAX_VARS_PER_FRAME;
        for (int l = 0; ok && l < fr.localCount; ++l)
            ok = readVar(f, fr.locals[l]);
    }

    if (!ok) { delete s; return nullptr; }
    return s;
}

// =============================================================================================
// PART C: SESSION MANAGEMENT
// =============================================================================================

void initSession(DebugSession& s)
{
    s.active = false;
    s.currentStep = 0;
}

void closeSession(DebugSession& s)
{
    if (s.active)
    {
        closeTdbg(s.reader);
        s.active = false;
    }
}

// =============================================================================================
// PART D: THE COMMANDS & PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "\n--- [Step " << (step + 1) << " of " << totalSteps << "] ---" << endl;
    cout << "Stack Depth: " << s.stackDepth << endl;

    for (int i = s.stackDepth - 1; i >= 0; --i)
    {
        const Frame& f = s.callStack[i];
        cout << "  Frame [" << i << "]: Function '" << f.func_name << "' (returnLine: " << f.returnLine << ")" << endl;

        if (f.argc > 0)
        {
            cout << "    Args: ";
            for (int a = 0; a < f.argc; ++a)
            {
                cout << f.argv[a].name << "=" << f.argv[a].value << (a + 1 < f.argc ? ", " : "");
            }
            cout << endl;
        }

        if (f.localCount > 0)
        {
            cout << "    Locals: ";
            for (int l = 0; l < f.localCount; ++l)
            {
                cout << f.locals[l].name << "=" << f.locals[l].value << (l + 1 < f.localCount ? ", " : "");
            }
            cout << endl;
        }
    }
    cout << "------------------------\n" << endl;
}

bool showCurrent(DebugSession& s)
{
    if (!s.active)
    {
        cout << "No active debugging session. Use 'start <source.bin>' first." << endl;
        return false;
    }

    Snapshot* snap = readSnapshotAt(s.reader, s.currentStep);
    if (!snap)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }

    printSnapshot(s.currentStep, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}

bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active)
    {
        cout << "No active debugging session." << endl;
        return false;
    }

    int64_t newStep = s.currentStep + delta;
    if (newStep < 0) newStep = 0;
    if (newStep >= s.reader.header.stepCount) newStep = s.reader.header.stepCount - 1;

    s.currentStep = newStep;
    return showCurrent(s);
}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (sock < 0)
    {
        cout << "Error: Could not connect to Load Balancer at " << LB_HOST << ":" << LB_PORT << endl;
        return false;
    }

    if (!uploadSource(sock, sourcePath.c_str()))
    {
        cout << "Error: Failed to upload source file: " << sourcePath << endl;
        close(sock);
        return false;
    }

    string errorMsg;
    if (!receiveResponse(sock, TDBG_FILE, errorMsg))
    {
        cout << "Error: " << errorMsg << endl;
        close(sock);
        return false;
    }
    close(sock);

    TdbgReader newReader;
    if (!openTdbg(TDBG_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    if (s.active)
    {
        closeTdbg(s.reader);
    }

    s.reader = newReader;
    s.currentStep = 0;
    s.active = true;

    return showCurrent(s);
}

bool cmdNext(DebugSession& s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession& s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession& s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession& s, int32_t n) { return moveBy(s, -n); }

void printHelp()
{
    cout << "\nAvailable Commands:" << endl;
    cout << "  start <source.bin>   - Upload program and begin debugging" << endl;
    cout << "  next                 - Move forward 1 step" << endl;
    cout << "  prv                  - Move backward 1 step" << endl;
    cout << "  jmp next <n>         - Jump forward n steps" << endl;
    cout << "  jmp prv <n>          - Jump backward n steps" << endl;
    cout << "  help                 - Show this help menu" << endl;
    cout << "  quit / exit          - Exit TTDB client\n" << endl;
}

// =============================================================================================
// PART F: INTERACTIVE SHELL & MAIN
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    stringstream ss(line);
    vector<string> words;
    string word;
    while (ss >> word) words.push_back(word);

    if (words.empty()) return true;

    if (words[0] == "ttdb")
    {
        words.erase(words.begin());
        if (words.empty()) return true;
    }

    string cmd = words[0];

    if (cmd == "start")
    {
        if (words.size() < 2)
        {
            cout << "Usage: start <source.bin>" << endl;
            return false;
        }
        return cmdStart(s, words[1]);
    }
    else if (cmd == "next")
    {
        return cmdNext(s);
    }
    else if (cmd == "prv")
    {
        return cmdPrv(s);
    }
    else if (cmd == "jmp")
    {
        if (words.size() < 3)
        {
            cout << "Usage: jmp next <n> OR jmp prv <n>" << endl;
            return false;
        }
        int32_t n = atoi(words[2].c_str());
        if (n <= 0 || n > MAX_JUMP)
        {
            cout << "Invalid jump count" << endl;
            return false;
        }
        if (words[1] == "next") return cmdJmpNext(s, n);
        if (words[1] == "prv") return cmdJmpPrv(s, n);

        cout << "Unknown jump direction: " << words[1] << endl;
        return false;
    }
    else if (cmd == "help")
    {
        printHelp();
        return true;
    }
    else if (cmd == "quit" || cmd == "exit")
    {
        quitRequested = true;
        return true;
    }

    cout << "unknown command. Type 'help' for options." << endl;
    return false;
}

int main(int argc, char* argv[])
{
    if (argc >= 2) LB_HOST = argv[1];
    if (argc >= 3) LB_PORT = atoi(argv[2]);

    DebugSession session;
    initSession(session);

    cout << "==================================================" << endl;
    cout << " Welcome to Time-Travel Debugger (TTDB) Client" << endl;
    cout << " Connected to Load Balancer: " << LB_HOST << ":" << LB_PORT << endl;
    cout << "==================================================" << endl;
    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested)
    {
        cout << PROMPT;
        if (!getline(cin, line)) break;
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    cout << "Exiting TTDB Client. Goodbye!" << endl;
    return 0;
}