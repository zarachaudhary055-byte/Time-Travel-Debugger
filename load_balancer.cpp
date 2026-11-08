// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================

// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <vector>
#include <ctime>
#include <signal.h>

// Windows vs Linux Header Handling
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int ssize_t;
#define SHUT_WR SD_SEND
#define MSG_NOSIGNAL 0
#define SOCK_CLOEXEC 0
#define usleep(us) Sleep((us)/1000)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "helper.h"
using namespace std;

// ---- Constants ----
const char* SERVER_PATH = "./server";
const int IO_BUFFER_SIZE = 64 * 1024;
const int LISTEN_BACKLOG = 16;
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

// ---- Custom Queue Implementation ----
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
        Node(const T& val) : data(val), next(nullptr) {}
    };
    Node* front, * rear;
    int count;

public:
    Queue() : front(nullptr), rear(nullptr), count(0) {}

    ~Queue()
    {
        while (!isEmpty())
        {
            dequeue();
        }
    }

    void enqueue(const T& val)
    {
        Node* newNode = new Node(val);
        if (rear == nullptr)
        {
            front = rear = newNode;
        }
        else
        {
            rear->next = newNode;
            rear = newNode;
        }
        count++;
    }

    T dequeue()
    {
        if (isEmpty())
        {
            throw runtime_error("Queue is empty");
        }
        Node* temp = front;
        T val = temp->data;
        front = front->next;
        if (front == nullptr)
        {
            rear = nullptr;
        }
        delete temp;
        count--;
        return val;
    }

    bool isEmpty()
    {
        return count == 0;
    }

    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;
    string address;
    int port;
    time_t arrivedAt;
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;

// ---- Basic Socket Helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalSent = 0;
    while (totalSent < n)
    {
        ssize_t sent = send(sock, buf + totalSent, n - totalSent, MSG_NOSIGNAL);
        if (sent <= 0)
            return false;
        totalSent += sent;
    }
    return true;
}

bool sendError(int sock, const string& message)
{
    uint8_t status = STATUS_ERROR; // 1
    uint32_t crc = calculateCRC32(&status, 1);
    crc = calculateCRC32((const uint8_t*)message.c_str(), message.length(), crc);
    crc = finalizeCRC32(crc);

    if (!sendAll(sock, (const char*)&status, 1)) return false;
    if (!sendAll(sock, message.c_str(), message.length())) return false;
    if (!sendAll(sock, (const char*)&crc, sizeof(crc))) return false;

    return true;
}

int createListenSocket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt SO_REUSEADDR failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("Bind failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        perror("Listen failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    return sock;
}

int connectToServer(const char* host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
    int res = system(cmd.c_str());
    return (res == 0);
}

int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0) return sock;

    if (!startServer(serverPort)) return -1;

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; ++i)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0) return sock;
    }

    return -1;
}

// =============================================================================================
// PART 2: PASSING THE DATA AND REPLY
// =============================================================================================

void relayOneClient(ClientInfo& c)
{
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }

    // 1. Client -> Server forwarding
    vector<char> buffer(IO_BUFFER_SIZE);
    while (true)
    {
        ssize_t bytesRead = recv(c.sock, buffer.data(), IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0) break;
        if (!sendAll(serverSock, buffer.data(), bytesRead)) break;
    }

    shutdown(serverSock, SHUT_WR);

    // 2. Server -> Client forwarding
    while (true)
    {
        ssize_t bytesRead = recv(serverSock, buffer.data(), IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0) break;
        if (!sendAll(c.sock, buffer.data(), bytesRead)) break;
    }

    close(serverSock);
    close(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(int listenSock)
{
    while (flag)
    {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSock < 0)
        {
            if (!flag) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr, INET_ADDRSTRLEN);

        ClientInfo info;
        info.sock = clientSock;
        info.address = string(ipStr);
        info.port = ntohs(clientAddr.sin_port);
        info.arrivedAt = time(nullptr);

        lock_guard<mutex> lock(queueMutex);
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            sendError(clientSock, "server busy, try again later");
            close(clientSock);
        }
        else
        {
            pendingClients.enqueue(info);
        }
    }
}

void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo c;
        bool hasClient = false;

        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty())
            {
                c = pendingClients.dequeue();
                hasClient = true;
            }
        }

        if (hasClient)
        {
            if (time(nullptr) - c.arrivedAt > 30)
            {
                sendError(c.sock, "request timed out in queue");
                close(c.sock);
            }
            else
            {
                relayOneClient(c);
            }
        }
        else
        {
            usleep(10000); // Sleep 10ms to prevent CPU spinning
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char* argv[])
{
    if (argc >= 3)
    {
        serverHost = argv[1];
        serverPort = atoi(argv[2]);
    }
    if (argc >= 4)
    {
        lbPort = atoi(argv[3]);
    }

    signal(SIGINT, handler);

    int listenSock = createListenSocket(lbPort);
    cout << "[Load Balancer] Listening on port " << lbPort << "..." << endl;

    thread accThread(acceptLoop, listenSock);
    accThread.detach();

    dispatcherLoop();

    close(listenSock);
    cout << "\n[Load Balancer] Shutting down cleanly." << endl;
    return 0;
}