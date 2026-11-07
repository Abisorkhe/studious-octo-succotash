#pragma once
// Simplified CComObject for Linux test builds (no LibRelay/IPtr).
#include <windows.h>
#include <atomic>

extern DWORD gdwDPlaySPRefCount;

template<typename T>
class CComObject : public T
{
public:
    using TInterface = T;
    CComObject() : m_refCount(0) { ++gdwDPlaySPRefCount; }
    CComObject(CComObject const&) = delete;
    CComObject& operator=(CComObject const&) = delete;
    virtual ~CComObject() { --gdwDPlaySPRefCount; }
    virtual HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_INVALIDARG;
        *ppvObject = GetInterfaceFromIID(riid);
        if (!*ppvObject) return E_NOINTERFACE;
        AddRef();
        return NOERROR;
    }
    virtual ULONG WINAPI AddRef() override { return (ULONG)++m_refCount; }
    virtual ULONG WINAPI Release() override
    {
        ULONG count = (ULONG)--m_refCount;
        if (!count) delete this;
        return count;
    }
protected:
    virtual IUnknown* GetInterfaceFromIID(IID const& iid)
    {
        if (iid == IID_IUnknown) return static_cast<IUnknown*>(this);
        return nullptr;
    }
private:
    std::atomic<long> m_refCount;
};

template<typename T>
class CComCopy : public CComObject<T>
{
public:
    CComCopy(T& original) : m_pOriginal(&original) {}
    T& GetOriginal() const { return *m_pOriginal; }
private:
    T* m_pOriginal;
};
#define COMCOPY_NOCTOR(className, iid) public: static IID const& GetIID() { return iid; }
#define COMCOPY(className, iid) COMCOPY_NOCTOR(className, iid) className(TInterface& original) : CComCopy(original) {}
