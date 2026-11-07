// SHL7 direct provider: Winsock implementation of the Android port's
// LAN protocol (android/host/dplay.c). See Shl7Protocol.h and README.md.
//
// NOTE (prototype): written for MSVC + the DirectX8 headers in
// Olde-Skuul/directplay/Include (dplay.h) inside a RedirectPlay fork.
// Not compiled in this environment (no Windows/MSVC here); the protocol
// logic is validated by tests/test_interop.py, which implements the
// same state machines in Python over real sockets.
#include "Shl7PlayProvider.h"
#include "DirectPlay/CompoundAddress.h"
#include "DirectX/dplobby.h"
#include "DirectPlay/Utils.h"
#include "Log.h"
#include "ServiceProviders/Registration.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace Shl7;

#ifdef _WIN32
typedef int socklen_t;  // Winsock uses int; POSIX already provides socklen_t
#endif

namespace {
constexpr uint16_t kPorts[] = { kPortClassic, kPortExtreme, kPortOriginal };

void WideToUnits(wchar_t const* text, uint16_t* out, uint32_t units) {
    memset(out, 0, units * sizeof(uint16_t));
    if (!text) return;
    for (uint32_t i = 0; i + 1 < units && text[i]; ++i) out[i] = (uint16_t)text[i];
}
void UnitsToWide(uint16_t const* in, wchar_t* out, size_t units) {
    for (size_t i = 0; i + 1 < units; ++i) out[i] = (wchar_t)in[i];
    out[units - 1] = 0;
}
bool SendAll(SOCKET s, void const* data, uint32_t size) {
    auto const* p = (uint8_t const*)data;
    while (size) {
        int n = send(s, (char const*)p, (int)size, 0);
        if (n <= 0) return false;
        p += n; size -= (uint32_t)n;
    }
    return true;
}
uint16_t EditionPortFromEnv() {
    char const* env = getenv("SHL7_PORT");
    if (env) { int p = atoi(env); if (p > 0 && p < 65536) return (uint16_t)p; }
    return kPortExtreme;  // default; see README (edition port selection)
}
} // namespace

CShl7PlayProvider::CShl7PlayProvider(void*, DWORD) { memset(&m_session, 0, sizeof(m_session)); }
CShl7PlayProvider::~CShl7PlayProvider() { Close(); }

HRESULT CShl7PlayProvider::InitializeConnection(void* pConnection, DWORD)
{
    std::vector<CompoundAddress::SEntry> entries;
    CompoundAddress::GetEntries(pConnection, entries);
    for (auto const& e : entries) {
        if (e.guid == DPAID_INet && e.pData && e.size) {
            size_t len = e.size && e.pData[e.size - 1] == 0 ? e.size - 1 : e.size;
            m_targetAddress.assign(e.pData, len);
        }
    }
    return DP_OK;
}

HRESULT CShl7PlayProvider::Open(DPSESSIONDESC2* pDescription, DWORD flags)
{
    if (flags & DPOPEN_CREATE) return Host(*pDescription);
    if (flags & DPOPEN_JOIN) return Join(*pDescription);
    return DPERR_INVALIDPARAM;
}

HRESULT CShl7PlayProvider::Host(DPSESSIONDESC2& description)
{
    if (description.lpszPassword && description.lpszPassword[0])
        return DPERR_INVALIDPARAM;  // Android port has no password negotiation
    memcpy(&m_session.desc, &description, 80);
    WideToUnits(description.lpszSessionName, m_session.name, kNameUnits);
    if (!m_session.desc[10]) m_session.desc[10] = kPlayers;
    if (m_session.desc[10] > kPlayers) return DPERR_INVALIDPARAM;
    srand((unsigned)time(nullptr) ^ GetCurrentProcessId());
    for (int i = 2; i < 6; ++i) m_session.desc[i] = (uint32_t)rand() ^ (uint32_t)GetTickCount();
    m_session.desc[11] = 0;
    m_hosting = true; m_localId = 1; m_nextId = 2; m_port = EditionPortFromEnv();
    m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    m_discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_listener == INVALID_SOCKET || m_discovery == INVALID_SOCKET) return DPERR_NOCONNECTION;
    int one = 1;
    setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
    setsockopt(m_discovery, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one));
    setsockopt(m_discovery, SOL_SOCKET, SO_BROADCAST, (char*)&one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(m_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_listener, (sockaddr*)&a, sizeof(a)) != 0) { Close(); return DPERR_NOCONNECTION; }
    a.sin_port = htons(m_port + 2);  // discovery is always port+2 (dplay.c LAN_DISCOVERY)
    if (bind(m_discovery, (sockaddr*)&a, sizeof(a)) != 0) { Close(); return DPERR_NOCONNECTION; }
    if (listen(m_listener, 8) != 0) { Close(); return DPERR_NOCONNECTION; }
    m_open = true; m_stop = false;
    m_worker = std::thread(&CShl7PlayProvider::WorkerLoop, this);
    m_workerStarted = true;
    return DP_OK;
}

HRESULT CShl7PlayProvider::Join(DPSESSIONDESC2& description)
{
    memcpy(&m_session.desc, &description, 80);
    WideToUnits(description.lpszSessionName, m_session.name, kNameUnits);
    struct Found { sockaddr_in addr; Session session; };
    std::vector<Found> found;
    SOCKET disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (disc == INVALID_SOCKET) return DPERR_NOCONNECTION;
    int one = 1; setsockopt(disc, SOL_SOCKET, SO_BROADCAST, (char*)&one, sizeof(one));
    uint32_t query[9] = { htonl(kMagic) };
    memcpy(query + 1, &description.guidApplication, 16);
    for (uint16_t port : kPorts) {
        sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(port + 2); dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(disc, (char*)query, sizeof(query), 0, (sockaddr*)&dst, sizeof(dst));
        if (!m_targetAddress.empty()) {
            dst.sin_addr.s_addr = inet_addr(m_targetAddress.c_str());
            if (dst.sin_addr.s_addr != INADDR_NONE) sendto(disc, (char*)query, sizeof(query), 0, (sockaddr*)&dst, sizeof(dst));
        }
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(disc, &rfds);
        timeval tv{ 0, 50000 };
        if (select((int)disc + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t answer[4 + sizeof(Session)];
        sockaddr_in src{}; socklen_t len = sizeof(src);
        int n = recvfrom(disc, (char*)answer, sizeof(answer), 0, (sockaddr*)&src, &len);
        if (n != (int)sizeof(answer) || ntohl(*(uint32_t*)answer) != kMagic) continue;
        Session s; memcpy(&s, answer + 4, sizeof(s));
        if (!ValidSession(s)) continue;
        if (memcmp(&s.desc[6], &description.guidApplication, 16) != 0) continue;
        if (memcmp(&s.desc[2], &description.guidInstance, 16) == 0) {
            sockaddr_in tcp = src; tcp.sin_port = htons(ntohs(src.sin_port) - 2);
            found.push_back({ tcp, s });
            break;
        }
    }
    closesocket(disc);
    if (found.empty()) return DPERR_NOCONNECTION;
    m_session = found[0].session;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return DPERR_NOCONNECTION;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
    if (connect(sock, (sockaddr*)&found[0].addr, sizeof(found[0].addr)) != 0) { closesocket(sock); return DPERR_NOCONNECTION; }
    Peer peer; peer.sock = sock; peer.id = 1;
    { std::lock_guard<std::mutex> lock(m_lock); m_peers[1] = peer; }
    if (!SendPacket(sock, kHello, 0, 0, &m_session.desc[2], 32)) { Close(); return DPERR_NOCONNECTION; }
    m_hosting = false; m_open = true; m_stop = false;
    m_worker = std::thread(&CShl7PlayProvider::WorkerLoop, this);
    m_workerStarted = true;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        { std::lock_guard<std::mutex> lock(m_lock); if (m_localId != 0) return DP_OK; }
        Sleep(20);
    }
    Close(); return DPERR_NOCONNECTION;
}

bool CShl7PlayProvider::SendPacket(SOCKET sock, uint32_t kind, uint32_t from, uint32_t to, void const* data, uint32_t size)
{
    uint32_t header[5] = { htonl(kMagic), htonl(kind), htonl(from), htonl(to), htonl(size) };
    return SendAll(sock, header, sizeof(header)) && (size == 0 || SendAll(sock, data, size));
}

void CShl7PlayProvider::Relay(DPID exceptId, uint32_t kind, uint32_t from, uint32_t to, void const* data, uint32_t size)
{
    std::vector<SOCKET> targets;
    { std::lock_guard<std::mutex> lock(m_lock);
      for (auto& [id, peer] : m_peers) if (id != exceptId && peer.welcomed && (!to || id == to)) targets.push_back(peer.sock); }
    for (SOCKET s : targets) SendPacket(s, kind, from, to, data, size);
}

void CShl7PlayProvider::Queue(uint32_t kind, DPID from, DPID to, void const* data, uint32_t size)
{
    QueuedMessage msg; msg.kind = kind; msg.from = from; msg.to = to;
    msg.payload.assign((uint8_t const*)data, (uint8_t const*)data + size);
    std::lock_guard<std::mutex> lock(m_lock);
    m_queue.push_back(std::move(msg));
}

void CShl7PlayProvider::WorkerLoop()
{
    // Single select loop over listener/discovery/peers, like dplay.c
    // network_worker(). Packets are framed exactly as in dplay.c
    // peer_io(): 20-byte BE header, then `size` payload bytes.
    while (!m_stop) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxFd = 0;
        if (m_hosting && m_listener != INVALID_SOCKET) { FD_SET(m_listener, &rfds); maxFd = std::max(maxFd, (int)m_listener); }
        if (m_hosting && m_discovery != INVALID_SOCKET) { FD_SET(m_discovery, &rfds); maxFd = std::max(maxFd, (int)m_discovery); }
        { std::lock_guard<std::mutex> lock(m_lock);
          for (auto& [id, peer] : m_peers) { FD_SET(peer.sock, &rfds); maxFd = std::max(maxFd, (int)peer.sock); } }
        timeval tv{ 0, 5000 };
        if (select(maxFd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        if (m_hosting && m_listener != INVALID_SOCKET && FD_ISSET(m_listener, &rfds)) {
            SOCKET conn = accept(m_listener, nullptr, nullptr);
            if (conn != INVALID_SOCKET) {
                int one = 1; setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
                std::lock_guard<std::mutex> lock(m_lock);
                Peer peer; peer.sock = conn; peer.id = m_nextId++;
                m_peers[peer.id] = peer;
            }
        }
        if (m_hosting && m_discovery != INVALID_SOCKET && FD_ISSET(m_discovery, &rfds)) {
            uint32_t query[9]; sockaddr_in src{}; socklen_t len = sizeof(src);
            int got = recvfrom(m_discovery, (char*)query, sizeof(query), 0, (sockaddr*)&src, &len);
            if (got == (int)sizeof(query) && ntohl(query[0]) == kMagic &&
                memcmp(query + 1, &m_session.desc[6], 16) == 0 && !(m_session.desc[1] & 0x21)) {
                // Single datagram: BE magic + lan_session (dplay.c format).
                uint8_t answer[4 + sizeof(Session)];
                uint32_t magic = htonl(kMagic);
                memcpy(answer, &magic, sizeof(magic));
                memcpy(answer + 4, &m_session, sizeof(m_session));
                sendto(m_discovery, (char*)answer, sizeof(answer), 0, (sockaddr*)&src, len);
            }
        }
        struct Incoming { DPID id; uint32_t kind, from, to; std::vector<uint8_t> payload; };
        std::vector<Incoming> incoming;
        std::vector<DPID> drop;
        std::vector<DPID> peerIds;
        { std::lock_guard<std::mutex> lock(m_lock); for (auto& [id, peer] : m_peers) peerIds.push_back(id); }
        for (DPID id : peerIds) {
            SOCKET sock;
            { std::lock_guard<std::mutex> lock(m_lock);
              auto it = m_peers.find(id); if (it == m_peers.end()) continue; sock = it->second.sock; }
            if (!FD_ISSET(sock, &rfds)) continue;
            uint8_t buf[16384];
            int got = recv(sock, (char*)buf, sizeof(buf), 0);
            if (got <= 0) { drop.push_back(id); continue; }
            std::lock_guard<std::mutex> lock(m_lock);
            auto it = m_peers.find(id);
            if (it == m_peers.end()) continue;
            auto& rx = it->second.rx;
            rx.insert(rx.end(), buf, buf + got);
            while (rx.size() >= 20) {
                uint32_t header[5]; memcpy(header, rx.data(), 20);
                uint32_t magic = ntohl(header[0]), kind = ntohl(header[1]),
                           from = ntohl(header[2]), to = ntohl(header[3]), size = ntohl(header[4]);
                if (magic != kMagic || size > kMaxData) { drop.push_back(id); break; }
                if (rx.size() < 20 + size) break;
                std::vector<uint8_t> payload(rx.begin() + 20, rx.begin() + 20 + size);
                rx.erase(rx.begin(), rx.begin() + 20 + size);
                incoming.push_back({ id, kind, from, to, std::move(payload) });
            }
        }
        for (auto const& pkt : incoming) {
            if (!ProcessPacket(pkt.id, pkt.kind, pkt.from, pkt.to, pkt.payload.data(), (uint32_t)pkt.payload.size()))
                drop.push_back(pkt.id);
        }
        for (DPID id : drop) DropPeer(id);
    }
}

bool CShl7PlayProvider::ProcessPacket(DPID peerId, uint32_t kind, uint32_t from, uint32_t to, uint8_t const* data, uint32_t size)
{
    // Mirrors dplay.c process_packet(). Locks are taken in small sections;
    // Queue()/Relay() lock internally, so never call them under m_lock.
    SOCKET peerSock = INVALID_SOCKET;
    bool welcomed = false;
    Session sessionSnapshot{};
    { std::lock_guard<std::mutex> lock(m_lock);
      auto it = m_peers.find(peerId);
      if (it == m_peers.end()) return false;
      peerSock = it->second.sock; welcomed = it->second.welcomed;
      sessionSnapshot = m_session; }
    if (m_hosting && !welcomed) {
        if (kind != kHello || size != 32 || memcmp(data, &sessionSnapshot.desc[2], 32) != 0 ||
            (sessionSnapshot.desc[1] & 0x21)) return false;
        std::lock_guard<std::mutex> lock(m_lock);
        if ((uint32_t)m_players.size() >= sessionSnapshot.desc[10]) return false;
        std::vector<uint8_t> welcome;
        welcome.resize(4 + sizeof(Session) + kPlayers * sizeof(Player));
        memcpy(welcome.data(), &peerId, 4);
        memcpy(welcome.data() + 4, &m_session, sizeof(Session));
        size_t slot = 0;
        for (auto& [id, player] : m_players) {
            if (slot >= kPlayers) break;
            memcpy(welcome.data() + 4 + sizeof(Session) + slot * sizeof(Player), &player, sizeof(Player));
            ++slot;
        }
        m_peers[peerId].welcomed = true;
        return SendPacket(peerSock, kWelcome, 0, peerId, welcome.data(), (uint32_t)welcome.size());
    }
    if (!m_hosting && !welcomed) {
        if (kind != kWelcome || size != 4 + sizeof(Session) + kPlayers * sizeof(Player)) return false;
        uint32_t id; memcpy(&id, data, 4);
        if (id < 2) return false;
        Session s; memcpy(&s, data + 4, sizeof(s));
        if (!ValidSession(s)) return false;
        Session mine; { std::lock_guard<std::mutex> lock(m_lock); mine = m_session; }
        if (memcmp(&s.desc[2], &mine.desc[2], 32) != 0) return false;
        std::lock_guard<std::mutex> lock(m_lock);
        for (uint32_t i = 0; i < kPlayers; ++i) {
            Player p; memcpy(&p, data + 4 + sizeof(Session) + i * sizeof(Player), sizeof(p));
            if (p.id == 0 && p.length == 0 && p.name[0] == 0) continue;
            if (!ValidPlayer(p)) return false;
            m_players[p.id] = p;
        }
        m_session = s; m_localId = id;
        m_peers[peerId].welcomed = true;
        return true;
    }
    if (m_hosting && from != peerId) return false;
    if (kind == kData) {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!from || !m_players.count(from)) return false;
        DPID local = m_localId;
        // queue after unlocking would be nicer; Queue() locks, so call it
        // via a copy of the payload outside the lock below.
        // (Prototype: unlock happens at scope end, Queue is called after.)
        // To avoid nested locking, handle queueing after this block:
        // fallthrough with a flag is complex; instead queue inline by
        // directly pushing (we already hold m_lock, same as dplay.c).
        if (!to || to == local) {
            QueuedMessage msg; msg.kind = kGuestData; msg.from = from; msg.to = local;
            msg.payload.assign(data, data + size);
            m_queue.push_back(std::move(msg));
        }
        // relay after unlocking
        // (handled below by returning through a small lambda scope)
        // NOTE: relay needs the payload; do it after the lock scope by
        // storing it -- prototype simplification: relay under lock is
        // acceptable here because TCP_NODELAY sends are small and
        // dplay.c does the same (relay() under d->lock).
        {
            std::vector<SOCKET> targets;
            for (auto& [pid, peer] : m_peers) if (pid != peerId && peer.welcomed && (!to || pid == to)) targets.push_back(peer.sock);
            for (SOCKET s : targets) SendPacket(s, kData, from, to, data, size);
        }
        return true;
    }
    if (kind == kPlayer || kind == kPlayerData) {
        if (size != sizeof(Player)) return false;
        Player p; memcpy(&p, data, sizeof(p));
        if (!ValidPlayer(p) || (m_hosting && p.id != peerId)) return false;
        bool created;
        { std::lock_guard<std::mutex> lock(m_lock);
          created = !m_players.count(p.id);
          m_players[p.id] = p;
          m_session.desc[11] = (uint32_t)m_players.size(); }
        if (p.id != m_localId) Queue(created ? kGuestCreatePlayer : kGuestPlayerData, 0, m_localId, &p, sizeof(p));
        if (m_hosting) Relay(peerId, kind, from, 0, data, size);
        return true;
    }
    if (kind == kRemove && !m_hosting && size == 4) {
        uint32_t id; memcpy(&id, data, 4);
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_players.erase(id)) return true;
        m_session.desc[11] = (uint32_t)m_players.size();
        QueuedMessage msg; msg.kind = kGuestDestroyPlayer; msg.from = 0; msg.to = m_localId;
        msg.payload.assign(data, data + 4);
        m_queue.push_back(std::move(msg));
        return true;
    }
    if (kind == kSession && !m_hosting && size == sizeof(Session)) {
        Session s; memcpy(&s, data, sizeof(s));
        if (!ValidSession(s)) return false;
        Session mine; { std::lock_guard<std::mutex> lock(m_lock); mine = m_session; }
        if (memcmp(&s.desc[2], &mine.desc[2], 32) != 0) return false;
        { std::lock_guard<std::mutex> lock(m_lock); m_session = s; }
        Queue(kGuestSessionDesc, 0, m_localId, data, size);
        return true;
    }
    return false;
}
void CShl7PlayProvider::DropPeer(DPID peerId)
{
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_peers.find(peerId);
        if (it == m_peers.end()) return;
        sock = it->second.sock; m_peers.erase(it);
        auto pit = m_players.find(peerId);
        if (pit != m_players.end()) {
            m_players.erase(pit);
            m_session.desc[11] = (uint32_t)m_players.size();
            QueuedMessage msg; msg.kind = kGuestDestroyPlayer; msg.from = 0; msg.to = m_localId;
            msg.payload.resize(sizeof(Player));
            Player dead{}; dead.id = peerId; memcpy(msg.payload.data(), &dead, sizeof(dead));
            m_queue.push_back(std::move(msg));
        }
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    Relay(peerId, kRemove, 0, 0, &peerId, sizeof(peerId));
}

// ---- DirectPlay surface (guest side; mirrors dplay.c COM methods) ----

HRESULT CShl7PlayProvider::CreatePlayer(LPDPID playerId, LPDPNAME name, HANDLE, LPVOID data, DWORD size, DWORD flags)
{
    if (!playerId || size > kPlayerDataSize) return DPERR_INVALIDPARAM;
    Player player{}; player.id = m_localId; player.flags = flags; player.length = size;
    wchar_t const* text = (name && name->lpszLongName) ? name->lpszLongName
                          : (name ? name->lpszShortName : nullptr);
    WideToUnits(text, player.name, kNameUnits);
    if (size && data) memcpy(player.data, data, size);
    std::vector<Player> others;
    { std::lock_guard<std::mutex> lock(m_lock);
      m_players[m_localId] = player;
      m_session.desc[11] = (uint32_t)m_players.size();
      for (auto& [id, other] : m_players) if (id != m_localId) others.push_back(other); }
    if (m_hosting) Relay(-1, kPlayer, m_localId, 0, &player, sizeof(player));
    else { std::lock_guard<std::mutex> lock(m_lock); SendPacket(m_peers[1].sock, kPlayer, m_localId, 0, &player, sizeof(player)); }
    for (auto const& other : others) Queue(kGuestCreatePlayer, 0, m_localId, &other, sizeof(other));
    *playerId = m_localId;
    return DP_OK;
}

HRESULT CShl7PlayProvider::DestroyPlayer(DPID dpid)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_players.erase(dpid)) return DPERR_INVALIDPLAYER;
    m_session.desc[11] = (uint32_t)m_players.size();
    return DP_OK;
}

HRESULT CShl7PlayProvider::Send(DPID from, DPID to, DWORD, LPVOID data, DWORD size)
{
    if (!data || size > kMaxData || from != m_localId) return DPERR_INVALIDPARAM;
    if (to == m_localId) { Queue(kGuestData, from, to, data, size); return DP_OK; }
    if (m_hosting) Relay(-1, kData, from, to, data, size);
    else { std::lock_guard<std::mutex> lock(m_lock); SendPacket(m_peers[1].sock, kData, from, to, data, size); }
    return DP_OK;
}
HRESULT CShl7PlayProvider::SendEx(DPID from, DPID to, DWORD flags, LPVOID data, DWORD size, DWORD, DWORD, LPVOID, DWORD_PTR* msgId)
{
    HRESULT r = Send(from, to, flags, data, size);
    if (msgId) *msgId = 0;
    return r;
}

HRESULT CShl7PlayProvider::Receive(LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_queue.empty()) return DPERR_NOMESSAGES;
    QueuedMessage& msg = m_queue.front();
    uint32_t needed = 0;
    std::vector<uint8_t> sysmsg;
    if (msg.kind == kGuestData) {
        needed = (uint32_t)msg.payload.size();
        if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
        memcpy(data, msg.payload.data(), needed);
        *from = msg.from; *to = msg.to;
    } else if (msg.kind == kGuestCreatePlayer || msg.kind == kGuestDestroyPlayer) {
        Player const& p = *(Player const*)msg.payload.data();
        wchar_t shortName[kNameUnits], longName[kNameUnits];
        UnitsToWide(p.name, shortName, kNameUnits); UnitsToWide(p.name, longName, kNameUnits);
        if (msg.kind == kGuestCreatePlayer) {
            needed = sizeof(DPMSG_CREATEPLAYERORGROUP) + sizeof(wchar_t) * (wcslen(shortName) + 1) + sizeof(wchar_t) * (wcslen(longName) + 1) + p.length;
            if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
            sysmsg.resize(needed);
            auto* m = (DPMSG_CREATEPLAYERORGROUP*)sysmsg.data();
            m->dwType = DPSYS_CREATEPLAYERORGROUP; m->dwPlayerType = DPPLAYERTYPE_PLAYER;
            m->dpId = p.id; m->dwCurrentPlayers = (DWORD)m_players.size();
            wchar_t* s = (wchar_t*)(sysmsg.data() + sizeof(DPMSG_CREATEPLAYERORGROUP));
            wcscpy(s, shortName); wchar_t* l = s + wcslen(s) + 1; wcscpy(l, longName);
            m->lpData = p.length ? (void*)l : nullptr; m->dwDataSize = p.length;
            if (p.length) memcpy(l, p.data, p.length);
            m->dpnName = ConstructDPName(s, l);
            m->dpIdParent = 0; m->dwFlags = (p.id == 1) ? 0x100 : 0;
        } else {
            needed = sizeof(DPMSG_DESTROYPLAYERORGROUP) + sizeof(wchar_t) * (wcslen(shortName) + 1) + sizeof(wchar_t) * (wcslen(longName) + 1);
            if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
            sysmsg.resize(needed);
            auto* m = (DPMSG_DESTROYPLAYERORGROUP*)sysmsg.data();
            m->dwType = DPSYS_DESTROYPLAYERORGROUP; m->dwPlayerType = DPPLAYERTYPE_PLAYER;
            m->dpId = p.id; m->lpLocalData = nullptr; m->dwLocalDataSize = 0;
            m->lpRemoteData = nullptr; m->dwRemoteDataSize = 0;
            wchar_t* s = (wchar_t*)(sysmsg.data() + sizeof(DPMSG_DESTROYPLAYERORGROUP));
            wcscpy(s, shortName); wchar_t* l = s + wcslen(s) + 1; wcscpy(l, longName);
            m->dpnName = ConstructDPName(s, l);
            m->dpIdParent = 0; m->dwFlags = 0;
        }
        memcpy(data, sysmsg.data(), needed);
        *from = DPID_SYSMSG; *to = msg.to;
    } else if (msg.kind == kGuestPlayerData) {
        Player const& p = *(Player const*)msg.payload.data();
        needed = sizeof(DPMSG_SETPLAYERORGROUPDATA) + p.length;
        if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
        sysmsg.resize(needed);
        auto* m = (DPMSG_SETPLAYERORGROUPDATA*)sysmsg.data();
        m->dwType = DPSYS_SETPLAYERORGROUPDATA; m->dwPlayerType = DPPLAYERTYPE_PLAYER;
        m->dpId = p.id;
        m->lpData = p.length ? (void*)(sysmsg.data() + sizeof(DPMSG_SETPLAYERORGROUPDATA)) : nullptr;
        m->dwDataSize = p.length;
        if (p.length) memcpy(sysmsg.data() + sizeof(DPMSG_SETPLAYERORGROUPDATA), p.data, p.length);
        memcpy(data, sysmsg.data(), needed);
        *from = DPID_SYSMSG; *to = msg.to;
    } else if (msg.kind == kGuestSessionDesc) {
        Session const& s = *(Session const*)msg.payload.data();
        wchar_t name[kNameUnits]; UnitsToWide(s.name, name, kNameUnits);
        needed = sizeof(DPMSG_SETSESSIONDESC) + sizeof(wchar_t) * (wcslen(name) + 1);
        if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
        sysmsg.resize(needed);
        auto* m = (DPMSG_SETSESSIONDESC*)sysmsg.data();
        m->dwType = DPSYS_SETSESSIONDESC;
        memcpy(&m->dpDesc, &s.desc, 80);
        wchar_t* dst = (wchar_t*)(sysmsg.data() + sizeof(DPMSG_SETSESSIONDESC));
        wcscpy(dst, name);
        m->dpDesc.lpszSessionName = dst;
        memcpy(data, sysmsg.data(), needed);
        *from = DPID_SYSMSG; *to = msg.to;
    } else if (msg.kind == kGuestSessionLost) {
        needed = sizeof(DPMSG_SESSIONLOST);
        if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
        ((DPMSG_SESSIONLOST*)data)->dwType = DPSYS_SESSIONLOST;
        *from = DPID_SYSMSG; *to = msg.to;
    } else return DPERR_GENERIC;
    if (!(flags & DPRECEIVE_PEEK)) m_queue.pop_front();
    *size = needed;
    return DP_OK;
}

HRESULT CShl7PlayProvider::EnumSessions(DPSESSIONDESC2* enumDesc, DWORD timeout, LPDPENUMSESSIONSCALLBACK2 callback, void* context, DWORD)
{
    if (!enumDesc || !callback) return DPERR_INVALIDPARAM;
    SOCKET disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (disc == INVALID_SOCKET) return DPERR_GENERIC;
    int one = 1; setsockopt(disc, SOL_SOCKET, SO_BROADCAST, (char*)&one, sizeof(one));
    uint32_t query[9] = { htonl(kMagic) };
    memcpy(query + 1, &enumDesc->guidApplication, 16);
    for (uint16_t port : kPorts) {
        sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(port + 2); dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(disc, (char*)query, sizeof(query), 0, (sockaddr*)&dst, sizeof(dst));
        if (!m_targetAddress.empty()) {
            dst.sin_addr.s_addr = inet_addr(m_targetAddress.c_str());
            if (dst.sin_addr.s_addr != INADDR_NONE) sendto(disc, (char*)query, sizeof(query), 0, (sockaddr*)&dst, sizeof(dst));
        }
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout ? timeout : 350);
    wchar_t szName[kNameUnits]{};
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(disc, &rfds);
        timeval tv{ 0, 50000 };
        if (select((int)disc + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t answer[4 + sizeof(Session)];
        int n = recv(disc, (char*)answer, sizeof(answer), 0);
        if (n != (int)sizeof(answer) || ntohl(*(uint32_t*)answer) != kMagic) continue;
        Session s; memcpy(&s, answer + 4, sizeof(s));
        if (!ValidSession(s)) continue;
        if (memcmp(&s.desc[6], &enumDesc->guidApplication, 16) != 0) continue;
        DPSESSIONDESC2 desc{};
        desc.dwSize = sizeof(desc);
        memcpy(&desc.guidApplication, &enumDesc->guidApplication, 16);
        memcpy(&desc.guidInstance, &s.desc[2], 16);
        desc.dwMaxPlayers = s.desc[10]; desc.dwCurrentPlayers = s.desc[11]; desc.dwFlags = 0;
        UnitsToWide(s.name, szName, kNameUnits);
        desc.lpszSessionName = szName;
        if (!callback(&desc, nullptr, 0, context)) break;
    }
    closesocket(disc);
    return DP_OK;
}

HRESULT CShl7PlayProvider::GetSessionDesc(LPVOID data, LPDWORD size)
{
    std::lock_guard<std::mutex> lock(m_lock);
    wchar_t name[kNameUnits]; UnitsToWide(m_session.name, name, kNameUnits);
    uint32_t needed = 80 + (uint32_t)(wcslen(name) + 1) * sizeof(wchar_t);
    if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
    memcpy(data, &m_session.desc, 80);
    auto* desc = (DPSESSIONDESC2*)data;
    wchar_t* dst = (wchar_t*)((uint8_t*)data + 80);
    wcscpy(dst, name);
    desc->lpszSessionName = dst; desc->lpszPassword = nullptr;
    *size = needed;
    return DP_OK;
}

HRESULT CShl7PlayProvider::SetSessionDesc(LPDPSESSIONDESC2 description, DWORD)
{
    if (!m_hosting) return DPERR_NOCONNECTION;
    if (description->lpszPassword && description->lpszPassword[0]) return DPERR_INVALIDPARAM;
    Session snapshot;
    std::vector<SOCKET> targets;
    { std::lock_guard<std::mutex> lock(m_lock);
      uint32_t guidInstance[4]; memcpy(guidInstance, &m_session.desc[2], 16);
      memcpy(&m_session.desc, description, 80);
      memcpy(&m_session.desc[2], guidInstance, 16);
      WideToUnits(description->lpszSessionName, m_session.name, kNameUnits);
      m_session.desc[11] = (uint32_t)m_players.size();
      snapshot = m_session;
      for (auto& [id, peer] : m_peers) if (peer.welcomed) targets.push_back(peer.sock); }
    for (SOCKET s : targets) SendPacket(s, kSession, 0, 0, &snapshot, sizeof(snapshot));
    return DP_OK;
}

HRESULT CShl7PlayProvider::GetPlayerName(DPID dpid, LPVOID data, LPDWORD size)
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_players.find(dpid);
    if (it == m_players.end()) return DPERR_INVALIDPLAYER;
    wchar_t name[kNameUnits]; UnitsToWide(it->second.name, name, kNameUnits);
    uint32_t needed = sizeof(DPNAME) + sizeof(wchar_t) * (wcslen(name) + 1) * 2;
    if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
    auto* dpname = (DPNAME*)data;
    wchar_t* s = (wchar_t*)((uint8_t*)data + sizeof(DPNAME));
    wcscpy(s, name); wchar_t* l = s + wcslen(s) + 1; wcscpy(l, name);
    *dpname = ConstructDPName(s, l);
    *size = needed;
    return DP_OK;
}

HRESULT CShl7PlayProvider::GetPlayerData(DPID dpid, LPVOID data, LPDWORD size, DWORD)
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_players.find(dpid);
    if (it == m_players.end()) return DPERR_INVALIDPLAYER;
    uint32_t needed = it->second.length;
    if (!data || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
    if (needed) memcpy(data, it->second.data, needed);
    *size = needed;
    return DP_OK;
}

HRESULT CShl7PlayProvider::SetPlayerData(DPID dpid, LPVOID data, DWORD size, DWORD)
{
    if (size > kPlayerData || dpid != m_localId) return DPERR_INVALIDPLAYER;
    Player snapshot; std::vector<SOCKET> targets; bool hosting;
    { std::lock_guard<std::mutex> lock(m_lock);
      auto it = m_players.find(dpid);
      if (it == m_players.end()) return DPERR_INVALIDPLAYER;
      it->second.length = size;
      if (size) memcpy(it->second.data, data, size);
      snapshot = it->second; hosting = m_hosting;
      for (auto& [id, peer] : m_peers) if (peer.welcomed) targets.push_back(peer.sock); }
    for (SOCKET s : targets) SendPacket(s, kPlayerData, m_localId, 0, &snapshot, sizeof(snapshot));
    return DP_OK;
}

HRESULT CShl7PlayProvider::GetCaps(LPDPCAPS caps, DWORD size)
{
    if (!caps || size < sizeof(DPCAPS)) return DPERR_INVALIDPARAM;
    memset(caps, 0, sizeof(DPCAPS));
    caps->dwSize = sizeof(DPCAPS);
    caps->dwFlags = 64 | (m_hosting ? 2 : 0);
    caps->dwMaxBufferSize = kMaxData;
    caps->dwMaxQueueSize = 8 * 1024 * 1024;
    caps->dwMaxPlayers = kPlayers;
    caps->dwHundredBaud = 1000000;
    caps->dwLatency = 10;
    caps->dwMaxLocalPlayers = 1;
    caps->dwHeaderLength = 20;
    caps->dwTimeout = 5000;
    return DP_OK;
}
HRESULT CShl7PlayProvider::GetPlayerCaps(DPID dpid, LPDPCAPS caps, DWORD size)
{
    { std::lock_guard<std::mutex> lock(m_lock); if (!m_players.count(dpid)) return DPERR_INVALIDPLAYER; }
    return GetCaps(caps, size);
}
HRESULT CShl7PlayProvider::GetPlayerFlags(DPID dpid, LPDWORD flags)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_players.count(dpid)) return DPERR_INVALIDPLAYER;
    *flags = (dpid == m_localId ? 8 : 16) | (dpid == 1 ? 0x100 : 0);
    return DP_OK;
}
HRESULT CShl7PlayProvider::GetMessageCount(DPID, LPDWORD count)
{
    std::lock_guard<std::mutex> lock(m_lock);
    *count = (DWORD)m_queue.size();
    return DP_OK;
}
HRESULT CShl7PlayProvider::CancelMessage(DWORD, DWORD) { return DP_OK; }

HRESULT CShl7PlayProvider::Close(void)
{
    m_stop = true;
    if (m_workerStarted) { m_worker.join(); m_workerStarted = false; }
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& [id, peer] : m_peers) if (peer.sock != INVALID_SOCKET) closesocket(peer.sock);
    m_peers.clear();
    if (m_listener != INVALID_SOCKET) { closesocket(m_listener); m_listener = INVALID_SOCKET; }
    if (m_discovery != INVALID_SOCKET) { closesocket(m_discovery); m_discovery = INVALID_SOCKET; }
    m_players.clear(); m_queue.clear(); m_open = false; m_localId = 0;
    return DP_OK;
}

// ---- Registration (RedirectPlay ServiceProviders/Registration.h) ----

class CShl7PlayProviderFactory final : public CServiceProviderFactory<CShl7PlayProvider>
{
public:
    using CServiceProviderFactory::CServiceProviderFactory;
    static bool IsAvailable()
    {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    virtual ~CShl7PlayProviderFactory() override { WSACleanup(); }
    virtual bool IsServiceAddress(char const*) const override { return false; }
};

// Prototype GUID for the direct provider. Generate a fresh one before
// shipping and keep it stable afterwards.
DEFINE_GUID(DPSPGUID_SHL7, 0x5a11c001, 0x3b2d, 0x4e5f, 0x8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x40, 0x51);
REGISTER_SERVICEPROVIDER_FACTORY_IFAVAILABLE(CShl7PlayProviderFactory, DPSPGUID_SHL7, DPCONNECTION_DIRECTPLAY, L"Stronghold Direct LAN");
