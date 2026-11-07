#pragma once
// Minimal registration shim for Linux test builds: the registry is a
// no-op, the test harness instantiates CShl7PlayProvider directly.
#include "DirectX/dplay.h"
#include <memory>
#include <string>
#include <unordered_map>

struct IServiceProviderData {
    virtual ~IServiceProviderData() = default;
    virtual LPVOID GetConnection() = 0;
    virtual DWORD GetConnectionSize() const = 0;
    virtual LPCDPNAME GetName() const = 0;
    virtual DWORD GetFlags() const = 0;
};
struct IServiceProviderFactory {
    virtual ~IServiceProviderFactory() = default;
    virtual IDirectPlay4* NewServiceProvider(void*, DWORD) = 0;
    virtual IServiceProviderData* NewData(GUID const*, DWORD) = 0;
    virtual bool IsServiceAddress(char const*) const = 0;
};
class CServiceProviderRegistry {
public:
    using TFactoryPtr = std::unique_ptr<IServiceProviderFactory>;
    using TProviders = std::unordered_map<GUID, TFactoryPtr>;
    static void Register(GUID const&, TFactoryPtr) {}
    static IServiceProviderFactory* Find(GUID const&) { return nullptr; }
    static TProviders const& GetProviders() { static TProviders s; return s; }
};
struct CServiceProviderData : public IServiceProviderData {
    CServiceProviderData(DWORD, wchar_t const*, wchar_t const*) {}
    LPVOID GetConnection() override { return nullptr; }
    DWORD GetConnectionSize() const override { return 0; }
    LPCDPNAME GetName() const override { return nullptr; }
    DWORD GetFlags() const override { return 0; }
};
template<typename T>
class CServiceProviderFactory : public IServiceProviderFactory {
public:
    CServiceProviderFactory(DWORD, std::wstring, std::wstring) {}
    CServiceProviderFactory(DWORD, std::wstring) {}
    virtual ~CServiceProviderFactory() override = default;
    virtual IDirectPlay4* NewServiceProvider(void* pConnection, DWORD flags) override { return new T(pConnection, flags); }
    virtual IServiceProviderData* NewData(GUID const*, DWORD) override { return new CServiceProviderData(0, L"", L""); }
    virtual bool IsServiceAddress(char const*) const override { return false; }
};
struct CServiceProviderRegistrar {
    CServiceProviderRegistrar() = default;
    CServiceProviderRegistrar(GUID const&, CServiceProviderRegistry::TFactoryPtr) {}
};
#define REGISTER_SERVICEPROVIDER_FACTORY_IFAVAILABLE(factoryType, guid, ...) \
    static CServiceProviderRegistrar const s_serviceProviderRegistrar_##factoryType = \
        factoryType::IsAvailable() ? CServiceProviderRegistrar(guid, std::make_unique<factoryType>(__VA_ARGS__)) : CServiceProviderRegistrar();
#define REGISTER_SERVICEPROVIDER(providerType, guid, ...) \
    static CServiceProviderRegistrar const s_serviceProviderRegistrar_##providerType = CServiceProviderRegistrar(guid, std::make_unique<CServiceProviderFactory<providerType>>(__VA_ARGS__));
