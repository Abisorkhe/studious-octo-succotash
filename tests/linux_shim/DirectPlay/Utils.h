#pragma once
#include "DirectX/dplay.h"
#include <string>

inline DPNAME ConstructDPName(char* shortName, char* longName)
{
    DPNAME result;
    result.dwSize = sizeof(DPNAME);
    result.dwFlags = 0;
    result.lpszShortNameA = shortName;
    result.lpszLongNameA = longName;
    return result;
}
inline DPNAME ConstructDPName(wchar_t* shortName, wchar_t* longName)
{
    return DPNAME { sizeof(DPNAME), 0, shortName, longName };
}
constexpr bool IsDPIDValidFrom(DPID from) { return (from > DPID_RESERVEDRANGE && from < DPID_UNKNOWN) || from == DPID_SERVERPLAYER; }
constexpr bool IsDPIDValidTo(DPID to)     { return (to > DPID_RESERVEDRANGE && to < DPID_UNKNOWN) || to <= DPID_SERVERPLAYER; }
inline wchar_t* ToWCharArray(wchar_t const* wca) { return const_cast<wchar_t*>(wca); }
inline char* ToCharArray(char const* ca) { return const_cast<char*>(ca); }
