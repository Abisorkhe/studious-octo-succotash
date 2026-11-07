#pragma once
// No-op logging shim for Linux test builds.
struct Log {
    template<typename... T> static void Info(char const*, T...) {}
    template<typename... T> static void InfoServer(char const*, T...) {}
    template<typename... T> static void InfoClient(char const*, T...) {}
    template<typename... T> static void Error(char const*, T...) {}
    template<typename... T> static void ErrorServer(char const*, T...) {}
    template<typename... T> static void ErrorClient(char const*, T...) {}
    template<typename... T> static void Debug(char const*, T...) {}
    template<typename... T> static void DebugServer(char const*, T...) {}
    template<typename... T> static void DebugClient(char const*, T...) {}
    template<typename... T> static void Warn(char const*, T...) {}
    template<typename... T> static void WarnServer(char const*, T...) {}
    template<typename... T> static void WarnClient(char const*, T...) {}
};
