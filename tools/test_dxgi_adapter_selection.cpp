#include <windows.h>
#include <dxgi1_6.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "../OptiScaler/hooks/DxgiAdapterSelection.h"

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Adapter
{
    LUID luid {};
    int refs = 1;
    int adds = 0;
    int releases = 0;
    HRESULT GetDesc(DXGI_ADAPTER_DESC* desc) const
    {
        desc->AdapterLuid = luid;
        return S_OK;
    }
    ULONG AddRef() { ++adds; return ++refs; }
    ULONG Release() { ++releases; return --refs; }
};

struct Factory
{
    bool current = true;
    bool hasProof = false;
    DxgiAdapterSelection::Validation proof {};
    BOOL IsCurrent() const { return current; }
    HRESULT GetPrivateData(REFGUID, UINT* size, void* data)
    {
        if (!hasProof || *size < sizeof(proof)) return DXGI_ERROR_NOT_FOUND;
        std::memcpy(data, &proof, sizeof(proof));
        *size = sizeof(proof);
        return S_OK;
    }
    HRESULT SetPrivateData(REFGUID, UINT size, const void* data)
    {
        hasProof = size == sizeof(proof);
        if (hasProof) std::memcpy(&proof, data, sizeof(proof));
        return S_OK;
    }
};

int main()
{
    Factory factory;
    Adapter preferred {{1, 2}}, wrong {{3, 4}}, fallback {{1, 2}};
    Adapter* returned = nullptr;
    int preferenceCalls = 0, luidCalls = 0;
    Adapter* candidate = &preferred;
    bool failPreference = false;
    auto byPreference = [&](Adapter** out) {
        ++preferenceCalls;
        if (failPreference) return E_NOINTERFACE;
        *out = candidate;
        candidate->AddRef();
        return S_OK;
    };
    auto byLuid = [&](Adapter** out) {
        ++luidCalls;
        *out = &fallback;
        fallback.AddRef();
        return S_OK;
    };
    auto enumerate = [&] {
        return DxgiAdapterSelection::Enumerate(&factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                               preferred.luid, &returned, byPreference, byLuid);
    };
    Require(enumerate() == S_OK && returned == &preferred && factory.hasProof && luidCalls == 0,
            "Preferred ordinal is verified and cached");
    returned->Release();
    Require(enumerate() == S_OK && returned == &preferred && preferenceCalls == 1 && luidCalls == 0,
            "Stable factory returns retained adapter without native enumeration");
    returned->Release();
    factory.current = false;
    candidate = &wrong;
    Require(enumerate() == S_OK && returned == &fallback && wrong.releases == 1 && luidCalls == 1,
            "Stale factory rejects wrong adapter and falls back by LUID");
    returned->Release();
    Require(preferred.refs == 1, "Stale factory released cached adapter");
    factory.current = true;
    factory.hasProof = false;
    failPreference = true;
    Require(enumerate() == S_OK && returned == &fallback && luidCalls == 2,
            "Unsupported preference API falls back by LUID");
    returned->Release();
    failPreference = false;
    Adapter replacement {{1, 2}};
    candidate = &replacement;
    Require(enumerate() == S_OK && returned == &replacement && preferenceCalls == 4,
            "A new valid adapter refreshes the cache");
    returned->Release();
    Require(enumerate() == S_OK && returned == &replacement && preferenceCalls == 4,
            "Refreshed adapter is reused without native enumeration");
    returned->Release();
    DxgiAdapterSelection::AdapterCache<Factory, Adapter>::Invalidate(&factory);
    Require(replacement.refs == 1, "Cache invalidation releases retained adapter");
    std::cout << "DXGI adapter identity, retained reuse, stale factory and LUID fallback passed.\n";
}
