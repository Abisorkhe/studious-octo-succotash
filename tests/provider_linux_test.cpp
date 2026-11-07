// Linux test harness for the real CShl7PlayProvider (Windows provider
// compiled natively via tests/linux_shim Winsock/WIN32 shims).
// Drives the provider like a game would: InitializeConnection, Open,
// CreatePlayer, Send/Receive, EnumSessions.
// Usage: provider_linux_test host|client
#include "Shl7PlayProvider.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

DWORD gdwDPlaySPRefCount = 0;

static DPSESSIONDESC2 g_foundDesc;
static bool g_found = false;
static BOOL CALLBACK EnumSessionsCb(LPCDPSESSIONDESC2 desc, LPVOID, DWORD, LPVOID)
{
    memcpy(&g_foundDesc, desc, sizeof(g_foundDesc));
    g_found = true;
    return TRUE;
}

static void MakeDesc(DPSESSIONDESC2& desc, wchar_t const* sessionName)
{
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = 80;  // 32-bit Windows DPSESSIONDESC2 size (Linux ptrs are 8B)
    desc.dwMaxPlayers = 8;
    for (int i = 0; i < 16; i++) {
        ((uint8_t*)&desc.guidInstance)[i] = 0xAA;
        ((uint8_t*)&desc.guidApplication)[i] = (uint8_t)i;
    }
    desc.lpszSessionName = const_cast<wchar_t*>(sessionName);
}

static void Pump(IDirectPlay4* dp, int seconds, bool reply)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        DPID from = 0, to = 0;
        DWORD size = 200000;
        static uint8_t out[200000];
        HRESULT r = dp->Receive(&from, &to, 1, out, &size);
        if (r == DP_OK) {
            uint32_t kind = from == 0 ? *(uint32_t*)out : 0;
            printf("WRECV kind=%u from=%u to=%u size=%u data=", kind, from, to, size);
            if (from != 0) fwrite(out, 1, size, stdout);
            putchar('\n'); fflush(stdout);
            if (reply && from != 0) {
                const char* pong = "pong from windows provider";
                dp->Send(1, from, 1, (void*)pong, (DWORD)strlen(pong));
            }
            continue;
        }
        if (r != DPERR_NOMESSAGES) { printf("WRECV_ERROR %08x\n", (unsigned)r); fflush(stdout); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int main(int argc, char** argv)
{
    if (argc != 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "client"))) return 2;
    bool isClient = !strcmp(argv[1], "client");
    auto* provider = new CShl7PlayProvider(nullptr, 0);
    IDirectPlay4* dp = provider;
    HRESULT r = dp->InitializeConnection(nullptr, 0);
    if (r != DP_OK) { printf("initconn failed %08x\n", (unsigned)r); return 1; }
    DPSESSIONDESC2 desc;
    MakeDesc(desc, L"WinSession");
    DPNAME name{};
    wchar_t playerName[] = L"WinPlayer";
    name.dwSize = sizeof(name);
    name.lpszShortName = playerName;
    name.lpszLongName = playerName;
    DPID playerId = 0;
    if (!isClient) {
        r = dp->Open(&desc, DPOPEN_CREATE);
        if (r != DP_OK) { printf("open host failed %08x\n", (unsigned)r); return 1; }
        r = dp->CreatePlayer(&playerId, &name, nullptr, nullptr, 0, 0);
        if (r != DP_OK) { printf("createplayer host failed %08x\n", (unsigned)r); return 1; }
        printf("WHOST_READY id=%u\n", playerId); fflush(stdout);
        Pump(dp, 12, true);
    } else {
        DPSESSIONDESC2 enumDesc = desc;
        enumDesc.lpszSessionName = nullptr;
        r = dp->EnumSessions(&enumDesc, 1000, EnumSessionsCb, nullptr, 0);
        if (r != DP_OK || !g_found) { printf("enumsessions failed %08x found=%d\n", (unsigned)r, (int)g_found); return 1; }
        printf("WFOUND\n"); fflush(stdout);
        // Join with the discovered session desc (guidInstance must match).
        DPSESSIONDESC2 joinDesc = g_foundDesc;
        joinDesc.lpszSessionName = const_cast<wchar_t*>(L"WinSession");
        r = dp->Open(&joinDesc, DPOPEN_JOIN);
        if (r != DP_OK) { printf("open join failed %08x\n", (unsigned)r); return 1; }
        r = dp->CreatePlayer(&playerId, &name, nullptr, nullptr, 0, 0);
        if (r != DP_OK) { printf("createplayer client failed %08x\n", (unsigned)r); return 1; }
        printf("WCLIENT_READY id=%u\n", playerId); fflush(stdout);
        const char* hello = "hello from windows provider";
        r = dp->Send(playerId, 1, 1, (void*)hello, (DWORD)strlen(hello));
        if (r != DP_OK) { printf("send failed %08x\n", (unsigned)r); return 1; }
        Pump(dp, 6, false);
    }
    dp->Close();
    provider->Release();
    puts(isClient ? "WCLIENT_DONE" : "WHOST_DONE"); fflush(stdout);
    return 0;
}
