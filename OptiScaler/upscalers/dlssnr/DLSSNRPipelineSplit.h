#pragma once
#include <d3d12.h>
struct NVSDK_NGX_Parameter;

// Physically separate the rendering prefix from the NR/tail. With PipelineAsync
// off both remain in one original batch. With it on, independent prefixes can
// advance on an auxiliary DIRECT queue and join before their original NR/tail.
// Original native FG waits and submission ownership remain in place.
namespace DLSSNRPipelineSplit
{
// Must run before the ordinary state/capture hooks on each implementation.
// The option is latched at device initialization and requires a restart.
void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* probe);
bool Active(); // Actual startup-latched routing state, independent of edited settings.
bool Observe(ID3D12GraphicsCommandList* commands);
// A native SDK can record through private driver interfaces or cached entry
// points, outside our public-method hooks. Give the WHOLE SDK call the physical
// recording target, and suppress logical capture/routing only for that scope.
// Keep lifetime acquisition, resource bookkeeping and callbacks on the logical
// list outside this scope. Closed prefixes and unsegmented lists pass through.
class NativeRecording
{
    ID3D12GraphicsCommandList* target;
    bool redirected = false;
    bool accessSuppressed = false;

public:
    explicit NativeRecording(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters = nullptr);
    ~NativeRecording();
    NativeRecording(const NativeRecording&) = delete;
    NativeRecording& operator=(const NativeRecording&) = delete;
    ID3D12GraphicsCommandList* Commands() const { return target; }
};
// Seal the prefix before any late NR command is recorded. Returns false when
// this recording cannot supply an exact NR boundary; ordinary NR still works.
bool BeforeNR(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene, UINT64 serial);
}
