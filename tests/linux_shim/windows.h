#pragma once
// Minimal Windows types shim for compiling the SHL7 provider on Linux.
// Only what Shl7PlayProvider + the DirectX shim below use.
#include <cstdint>
#include <ctime>
#include <cstring>
#include <string>
#include <unistd.h>

typedef uint32_t DWORD;
typedef DWORD* LPDWORD;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int32_t  HRESULT;
typedef void*    HANDLE;
typedef void*    LPVOID;
typedef uint32_t DWORD_PTR;
typedef int      BOOL;
#define TRUE 1
#define FALSE 0
typedef uint16_t WORD;
typedef wchar_t* LPWSTR;
typedef char*    LPSTR;
typedef const wchar_t* LPCWSTR;
typedef const char* LPCSTR;
typedef wchar_t  WCHAR;

#define WINAPI
#define CALLBACK
#define FAR
#define VOL
#define __stdcall
#define _In_
#define _Outptr_
#define _In_opt_

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_FAIL ((HRESULT)0x80004005)
#define NOERROR ((HRESULT)0)

struct GUID {
    uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8];
    bool operator==(GUID const& o) const {
        return Data1 == o.Data1 && Data2 == o.Data2 && Data3 == o.Data3 &&
               memcmp(Data4, o.Data4, 8) == 0;
    }
    bool operator!=(GUID const& o) const { return !(*this == o); }
};
typedef GUID IID;
namespace std { template<> struct hash<GUID> { size_t operator()(GUID const& g) const noexcept { return (size_t)g.Data1 ^ ((size_t)g.Data2 << 16) ^ g.Data3; } }; }
typedef GUID CLSID;
typedef GUID REFIID;

#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

struct IUnknown {
    virtual ~IUnknown() = default;
    virtual HRESULT WINAPI QueryInterface(REFIID, void**) = 0;
    virtual ULONG WINAPI AddRef() = 0;
    virtual ULONG WINAPI Release() = 0;
};
static const IID IID_IUnknown = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_fetch_and_add(p, 1) + 1; }
inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_fetch_and_sub(p, 1) - 1; }

inline DWORD GetTickCount() { return (DWORD)(clock() * 1000 / CLOCKS_PER_SEC); }
inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
inline void Sleep(DWORD ms) { usleep(ms * 1000); }
