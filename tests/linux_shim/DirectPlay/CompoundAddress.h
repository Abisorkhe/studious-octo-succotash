#pragma once
// Stub: Linux test harness sets the target via env, compound-address
// parsing is not exercised (see provider_linux_test.cpp).
#include <windows.h>
#include <vector>
struct CompoundAddress {
    struct SEntry { GUID guid; char const* pData; size_t size; };
    static void GetEntries(void*, std::vector<SEntry>&) {}
};
