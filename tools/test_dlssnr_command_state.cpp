#include "../OptiScaler/upscalers/dlssnr/DLSSNRCommandState.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace DLSSNRCommandState;
    static_assert(sizeof(Root) <= 32);
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(0x1000);
    auto* pipeline = reinterpret_cast<ID3D12PipelineState*>(0x2000);
    auto* signature = reinterpret_cast<ID3D12RootSignature*>(0x3000);

    CaptureEnabled(true);
    Reset(list, pipeline, true);
    Heaps(list, 0, nullptr);
    Signature(list, false, signature);
    Value(list, false, 0, Kind::Table, 0x1234);
    const UINT initial[] = {11, 22};
    Constants(list, false, 1, 2, initial, 3);

    const char* reason = nullptr;
    auto snapshot = Snapshot(list, &reason);
    assert(snapshot && reason == nullptr);
    assert(snapshot->graphics.roots.at(0).kind == Kind::Table);
    assert(!snapshot->graphics.roots.at(0).constants);
    assert(snapshot->graphics.roots.at(1).constants->known == (3u << 3));
    assert((*snapshot->graphics.roots.at(1).constants)[3] == 11);

    // A later recording must not alter a saved state used for NR restoration.
    const UINT changed = 99;
    Constants(list, false, 1, 1, &changed, 3);
    assert((*snapshot->graphics.roots.at(1).constants)[3] == 11);
    assert((*states.at(list).graphics.roots.at(1).constants)[3] == 99);

    Value(list, false, 1, Kind::SRV, 0x4567);
    assert(states.at(list).graphics.roots.at(1).kind == Kind::SRV);
    assert(!states.at(list).graphics.roots.at(1).constants);
    Signature(list, false, reinterpret_cast<ID3D12RootSignature*>(0x4000));
    assert(states.at(list).graphics.roots.empty());
    assert(snapshot->graphics.roots.at(1).kind == Kind::Constants);

    Reset(list);
    std::cout << "DLSSNR command state compact-root tests passed\n";
}
