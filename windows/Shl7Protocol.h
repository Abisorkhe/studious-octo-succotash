#pragma once
// SHL7 wire protocol -- shared spec for the direct bridge.
//
// This header is the single source of truth for the Windows provider.
// It must stay byte-compatible with the Android port's host
// implementation in pc_decomp_strhld/android/host/dplay.c:
//   * packet header: 5 x uint32 big-endian (magic, kind, from, to, size)
//   * all payload structs below are native little-endian (Android ARM64
//     and Windows x86/x64 are both little-endian, matching dplay.c which
//     memcpy's them without byte-swapping)
//   * session desc: 80 bytes, [0]=dwSize, [1]=dwFlags, [2..5]=guidInstance,
//     [6..9]=guidApplication, [10]=dwMaxPlayers, [11]=dwCurrentPlayers
//     (offsets in bytes: flags@4, guidInstance@8, guidApplication@24,
//      maxPlayers@40, currentPlayers@44)
//   * discovery: UDP to port+2, 36-byte query (BE magic + app GUID +
//     16 bytes padding, as in dplay.c uint32 query[9]), answer is
//     BE magic + lan_session (340 bytes)
//   * TCP session port per edition: Classic 47624, Extreme 47625,
//     Stronghold HD 47628 (dplay.c LAN_PORT)
#include <cstdint>
#include <winsock2.h>

namespace Shl7 {

constexpr uint32_t kMagic      = 0x53484C37u;
constexpr uint16_t kPortClassic  = 47624;
constexpr uint16_t kPortExtreme  = 47625;
constexpr uint16_t kPortOriginal = 47628;
constexpr uint32_t kMaxData    = 1024u * 1024u;
constexpr uint32_t kPlayers    = 8;
constexpr uint32_t kNameUnits  = 128;
constexpr uint32_t kPlayerDataSize = 4096;

enum PacketKind : uint32_t {
    kHello = 1, kWelcome, kPlayer, kRemove, kData, kSession, kPlayerData,
};

// Guest-visible message kinds: these are DirectPlay DPSYS_* values
// (Include/dplay.h in Olde-Skuul/directplay) and are what the Android
// port queues into Receive(): 3=CREATEPLAYERORGROUP, 5=DESTROYPLAYERORGROUP,
// 0x31=SESSIONLOST, 0x102=SETPLAYERORGROUPDATA, 0x104=SETSESSIONDESC.
enum GuestKind : uint32_t {
    kGuestData = 0, kGuestCreatePlayer = 3, kGuestDestroyPlayer = 5,
    kGuestSessionLost = 0x31, kGuestPlayerData = 0x102, kGuestSessionDesc = 0x104,
};

#pragma pack(push, 1)
struct Session {
    uint32_t desc[20];              // 80 bytes DPSESSIONDESC2 header
    uint16_t name[kNameUnits];      // UTF-16 session name
};
struct Player {
    uint32_t id;
    uint32_t flags;
    uint32_t length;
    uint16_t name[kNameUnits];      // UTF-16 player name
    uint8_t  data[kPlayerDataSize];
};
#pragma pack(pop)

static_assert(sizeof(Session) == 80 + kNameUnits * 2, "lan_session size");
static_assert(sizeof(Player) == 12 + kNameUnits * 2 + kPlayerDataSize, "lan_player size");

inline bool ValidSession(Session const& s) {
    return s.desc[0] == 80 && s.desc[10] > 0 && s.desc[10] <= kPlayers &&
           s.desc[11] <= kPlayers && s.name[kNameUnits - 1] == 0;
}
inline bool ValidPlayer(Player const& p) {
    return p.id > 0 && p.length <= kPlayerDataSize && p.name[kNameUnits - 1] == 0;
}

} // namespace Shl7
