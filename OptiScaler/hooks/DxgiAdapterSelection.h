#pragma once
#include <dxgi1_6.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace DxgiAdapterSelection
{
// The factory stores identity proof. A bounded process cache retains the proven
// adapter without making the factory itself own an adapter reference.
inline constexpr GUID validationKey {
    0x437c0de8, 0x13ae, 0x4379, {0x9d, 0xb9, 0x92, 0x5f, 0x91, 0xa5, 0x33, 0xcb}};
struct Validation
{
    UINT ordinal = 0;
    DXGI_GPU_PREFERENCE preference = DXGI_GPU_PREFERENCE_UNSPECIFIED;
    LUID luid {};
};
inline bool SameLuid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

template<class Factory, class Adapter>
struct AdapterCache
{
    struct Entry
    {
        Factory* factory;
        UINT ordinal;
        DXGI_GPU_PREFERENCE preference;
        LUID luid;
        std::shared_ptr<Adapter> adapter;
    };
    inline static std::mutex mutex;
    inline static std::vector<Entry> entries;
    static constexpr size_t capacity = 8;

    static bool Get(Factory* factory, UINT ordinal, DXGI_GPU_PREFERENCE preference, LUID luid,
                    Adapter** output)
    {
        std::scoped_lock lock(mutex);
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            if (it->factory != factory || it->ordinal != ordinal || it->preference != preference ||
                !SameLuid(it->luid, luid)) continue;
            it->adapter->AddRef();
            *output = it->adapter.get();
            if (it + 1 != entries.end()) std::rotate(it, it + 1, entries.end());
            return true;
        }
        return false;
    }

    static void Put(Factory* factory, UINT ordinal, DXGI_GPU_PREFERENCE preference, LUID luid,
                    Adapter* adapter)
    {
        adapter->AddRef();
        std::shared_ptr<Adapter> owned(adapter, [](Adapter* p) { p->Release(); });
        std::scoped_lock lock(mutex);
        std::erase_if(entries, [&](const Entry& e) {
            return e.factory == factory && e.ordinal == ordinal && e.preference == preference;
        });
        if (entries.size() == capacity) entries.erase(entries.begin());
        entries.push_back({factory, ordinal, preference, luid, std::move(owned)});
    }

    static void Invalidate(Factory* factory)
    {
        std::scoped_lock lock(mutex);
        std::erase_if(entries, [factory](const Entry& e) { return e.factory == factory; });
    }
};

template<class Factory, class Adapter, class ByPreference, class ByLuid>
HRESULT Enumerate(Factory* factory, UINT ordinal, DXGI_GPU_PREFERENCE preference, LUID luid,
                  Adapter** output, ByPreference&& byPreference, ByLuid&& byLuid)
{
    if (!output) return byLuid(output);
    *output = nullptr;
    const bool current = factory->IsCurrent() != FALSE;
    if (!current) AdapterCache<Factory, Adapter>::Invalidate(factory);
    Validation cached {};
    UINT size = sizeof(cached);
    const bool validated = current &&
        factory->GetPrivateData(validationKey, &size, &cached) == S_OK && size == sizeof(cached) &&
        cached.ordinal == ordinal && cached.preference == preference && SameLuid(cached.luid, luid);
    if (validated && AdapterCache<Factory, Adapter>::Get(factory, ordinal, preference, luid, output))
        return S_OK;

    Adapter* adapter = nullptr;
    const auto result = byPreference(&adapter);
    if (result == S_OK && adapter)
    {
        DXGI_ADAPTER_DESC desc {};
        if (validated || (adapter->GetDesc(&desc) == S_OK && SameLuid(desc.AdapterLuid, luid)))
        {
            bool cacheable = validated;
            if (!validated && current)
            {
                const Validation proof {ordinal, preference, luid};
                cacheable = factory->SetPrivateData(validationKey, sizeof(proof), &proof) == S_OK;
            }
            if (cacheable) AdapterCache<Factory, Adapter>::Put(factory, ordinal, preference, luid, adapter);
            *output = adapter;
            return S_OK;
        }
    }
    if (adapter) adapter->Release();
    // A stale factory, filtered discovery list, unsupported preference API, or
    // changed ordering must still select the exact GPU requested by the caller.
    if (validated) factory->SetPrivateData(validationKey, 0, nullptr);
    return byLuid(output);
}
}
