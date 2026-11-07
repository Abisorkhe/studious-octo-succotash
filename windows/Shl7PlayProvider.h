#pragma once
// Direct (non-Steam) DirectPlay service provider speaking the SHL7
// protocol of the Android port (android/host/dplay.c).
//
// Drop-in replacement for the transport half of RedirectPlay's
// CSteamPlayProvider: same IDirectPlay4 subset, same sysmsg synthesis,
// but Winsock TCP/UDP instead of SteamNetworkingSockets. Register it
// alongside (or instead of) the Steam provider in a RedirectPlay fork;
// see README.md in this folder's parent for build steps.
#include "COM/ComObject.h"
#include "DirectX/dplay.h"
#include "Shl7Protocol.h"

#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class CShl7PlayProvider final : public CComObject<IDirectPlay4>
{
public:
    CShl7PlayProvider(void* connection, DWORD flags);
    ~CShl7PlayProvider() override;

    // Implemented (mirrors the Android port's dp_methods subset that
    // Stronghold uses; everything else returns E_NOTIMPL, like
    // CSteamPlayProvider).
    virtual HRESULT WINAPI InitializeConnection(void* connection, DWORD flags) override;
    virtual HRESULT WINAPI Open(DPSESSIONDESC2* description, DWORD flags) override;
    virtual HRESULT WINAPI EnumSessions(DPSESSIONDESC2* enumDesc, DWORD timeout,
        LPDPENUMSESSIONSCALLBACK2 callback, void* context, DWORD flags) override;
    virtual HRESULT WINAPI CreatePlayer(LPDPID playerId, LPDPNAME name, HANDLE event,
        LPVOID data, DWORD size, DWORD flags) override;
    virtual HRESULT WINAPI DestroyPlayer(DPID dpid) override;
    virtual HRESULT WINAPI Send(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size) override;
    virtual HRESULT WINAPI SendEx(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size,
        DWORD priority, DWORD timeout, LPVOID context, DWORD_PTR* msgId) override;
    virtual HRESULT WINAPI Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size) override;
    virtual HRESULT WINAPI GetSessionDesc(LPVOID data, LPDWORD size) override;
    virtual HRESULT WINAPI SetSessionDesc(LPDPSESSIONDESC2 description, DWORD flags) override;
    virtual HRESULT WINAPI GetPlayerName(DPID dpid, LPVOID data, LPDWORD size) override;
    virtual HRESULT WINAPI GetPlayerData(DPID dpid, LPVOID data, LPDWORD size, DWORD flags) override;
    virtual HRESULT WINAPI SetPlayerData(DPID dpid, LPVOID data, DWORD size, DWORD flags) override;
    virtual HRESULT WINAPI GetCaps(LPDPCAPS caps, DWORD size) override;
    virtual HRESULT WINAPI GetPlayerCaps(DPID dpid, LPDPCAPS caps, DWORD size) override;
    virtual HRESULT WINAPI GetPlayerFlags(DPID dpid, LPDWORD flags) override;
    virtual HRESULT WINAPI GetMessageCount(DPID dpid, LPDWORD count) override;
    virtual HRESULT WINAPI CancelMessage(DWORD msgId, DWORD flags) override;
    virtual HRESULT WINAPI Close(void) override;

    virtual HRESULT WINAPI AddPlayerToGroup(DPID, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI CreateGroup(LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI DeletePlayerFromGroup(DPID, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI DestroyGroup(DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI EnumGroupPlayers(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI EnumGroups(LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI EnumPlayers(LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupData(DPID, LPVOID, LPDWORD, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupName(DPID, LPVOID, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetPlayerAddress(DPID, LPVOID, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupConnectionSettings(DWORD, DPID, LPVOID, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI Initialize(LPGUID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SetGroupData(DPID, LPVOID, DWORD, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SetGroupName(DPID, LPDPNAME, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SetPlayerName(DPID, LPDPNAME, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI AddGroupToGroup(DPID, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI CreateGroupInGroup(DPID, LPDPID, LPDPNAME, LPVOID, DWORD, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI DeleteGroupFromGroup(DPID, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI EnumConnections(LPCGUID, LPDPENUMCONNECTIONSCALLBACK, LPVOID, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI EnumGroupsInGroup(DPID, LPGUID, LPDPENUMPLAYERSCALLBACK2, LPVOID, DWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SecureOpen(LPCDPSESSIONDESC2, DWORD, LPCDPSECURITYDESC, LPCDPCREDENTIALS) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SendChatMessage(DPID, DPID, DWORD, LPDPCHAT) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SetGroupConnectionSettings(DWORD, DPID, LPDPLCONNECTION) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI StartSession(DWORD, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupFlags(DPID, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupParent(DPID, LPDPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetPlayerAccount(DPID, DWORD, LPVOID, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetGroupOwner(DPID, LPDPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI SetGroupOwner(DPID, DPID) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI GetMessageQueue(DPID, DPID, DWORD, LPDWORD, LPDWORD) override { return E_NOTIMPL; }
    virtual HRESULT WINAPI CancelPriority(DWORD, DWORD, DWORD) override { return E_NOTIMPL; }

private:
    struct QueuedMessage {
        uint32_t kind;   // Shl7::GuestKind
        DPID from;
        DPID to;
        std::vector<uint8_t> payload;  // raw data, or serialized Shl7::Player / Session
    };
    struct Peer {
        SOCKET sock = INVALID_SOCKET;
        bool welcomed = false;
        DPID id = 0;
        std::vector<uint8_t> rx;  // unparsed receive buffer
    };

    HRESULT Host(DPSESSIONDESC2& description);
    HRESULT Join(DPSESSIONDESC2& description);
    void WorkerLoop();
    bool ProcessPacket(DPID peerId, uint32_t kind, uint32_t from, uint32_t to,
        uint8_t const* data, uint32_t size);
    bool SendPacket(SOCKET sock, uint32_t kind, uint32_t from, uint32_t to,
        void const* data, uint32_t size);
    void Relay(DPID exceptId, uint32_t kind, uint32_t from, uint32_t to,
        void const* data, uint32_t size);
    void DropPeer(DPID peerId);
    void Queue(uint32_t kind, DPID from, DPID to, void const* data, uint32_t size);
    void RefreshSessionCount();
    DPID FindFreePlayerId() const;

    std::string m_targetAddress;  // DPAID_INet from compound address, if any
    bool m_hosting = false;
    bool m_open = false;
    bool m_stop = false;
    DPID m_localId = 0;
    DPID m_nextId = 2;
    Shl7::Session m_session{};
    std::unordered_map<DPID, Shl7::Player> m_players;

    SOCKET m_listener = INVALID_SOCKET;
    SOCKET m_discovery = INVALID_SOCKET;
    uint16_t m_port = Shl7::kPortExtreme;
    std::unordered_map<DPID, Peer> m_peers;
    std::deque<QueuedMessage> m_queue;
    std::mutex m_lock;
    std::thread m_worker;
    bool m_workerStarted = false;
};
