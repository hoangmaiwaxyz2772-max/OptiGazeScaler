#pragma once
#include <d3d12.h>
#include <cstdint>

// Observational prerequisite for the NR/next-frame overlap experiment.
// No queue redirection, GPU wait, or inference policy change is made here.
namespace DLSSNRPipelineTrace
{
bool Active();
void Poll(ID3D12Device* device = nullptr);
void Mark(const char* kind, const void* object = nullptr, const void* resource = nullptr,
          uint64_t value = 0);
// Paired API observations preserve the requested value and HRESULT separately.
uint64_t Enter(const char* kind, const void* object = nullptr, const void* resource = nullptr,
               uint64_t value = 0);
void Leave(const char* kind, uint64_t call, const void* object = nullptr,
           const void* resource = nullptr, uint64_t value = 0, HRESULT result = S_OK);
// Queue discovery is deferred to Poll; never install Detours inside a queue call.
void ObserveQueue(ID3D12CommandQueue* queue);
void WatchResource(ID3D12Resource* resource);
void ResetList(ID3D12GraphicsCommandList* commands);
void AfterNR(ID3D12GraphicsCommandList* commands);
void Work(ID3D12GraphicsCommandList* commands);
void EndPresent(HRESULT result);
void Shutdown();
}
