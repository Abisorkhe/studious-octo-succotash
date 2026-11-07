#pragma once
// Winsock -> POSIX socket shim (Linux test build only).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET ((SOCKET)-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define shutdown shutdown

struct WSAData { uint16_t wVersion; int dummy; };
typedef WSAData WSADATA;
inline int WSAStartup(uint16_t, WSAData*) { return 0; }
inline int WSACleanup() { return 0; }
#define MAKEWORD(a, b) ((uint16_t)(((uint16_t)(a)) | ((uint16_t)(b) << 8)))
