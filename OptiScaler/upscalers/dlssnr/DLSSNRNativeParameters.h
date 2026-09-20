#pragma once
struct NVSDK_NGX_Parameter;

namespace DLSSNRNativeParameters
{
// Register before returning an allocated native table to its caller. Capability
// tables initially contain SDK capabilities/callbacks, not application resources.
void Register(NVSDK_NGX_Parameter* parameters, bool fresh);
void Forget(NVSDK_NGX_Parameter* parameters);
}
