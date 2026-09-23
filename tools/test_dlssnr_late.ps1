$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\dlssnr-late'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cpp = Join-Path $PSScriptRoot 'test_dlssnr_late.cpp'
$exe = Join-Path $output 'late-test.exe'
$obj = Join-Path $output 'late-test.obj'
$include = Join-Path $repo 'OptiScaler\include'
$nr = Join-Path $repo 'OptiScaler\upscalers\dlssnr'
$source = Get-Content -Raw (Join-Path $nr 'DLSSNRLatePass.cpp')
$start = $source.IndexOf('std::string DescribeGuide(')
$end = $source.IndexOf('void DropPending(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Production guide helper extraction failed' }
Set-Content -LiteralPath (Join-Path $output 'late-guide-production.h') -Value $source.Substring($start, $end - $start)
$start = $source.IndexOf('bool Enabled()')
$end = $source.IndexOf('bool Pending()', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Descriptor policy extraction failed' }
$policy = "namespace DLSSNRLatePass {`n" + $source.Substring($start, $end - $start) + "}`n"
$tracking = Get-Content -Raw (Join-Path $repo 'OptiScaler/resource_tracking/ResTrack_dx12.cpp')
$start = $tracking.IndexOf('void ResTrack_Dx12::hkCopyDescriptors(')
$end = $tracking.IndexOf('#pragma endregion', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Descriptor hook extraction failed' }
$descriptorSource = $policy + $tracking.Substring($start, $end - $start)
$start = $tracking.IndexOf('bool ResTrack_Dx12::CheckResource(')
$end = $tracking.IndexOf('inline static IID streamlineRiid', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Metadata admission extraction failed' }
$descriptorSource += $tracking.Substring($start, $end - $start)
$start = $tracking.IndexOf('void ResTrack_Dx12::hkCreateRenderTargetView(')
$end = $tracking.IndexOf('#pragma endregion', $start)
if ($start -lt 0 -or $end -le $start) { throw 'View hook extraction failed' }
$descriptorSource += $tracking.Substring($start, $end - $start)
Set-Content -LiteralPath (Join-Path $output 'late-descriptors-production.h') -Value $descriptorSource
$inputs = Get-Content -Raw (Join-Path $repo 'OptiScaler/inputs/FG/Upscaler_Inputs_Dx12.cpp')
$start = $inputs.IndexOf('void UpscalerInputsDx12::UpscaleEnd(')
if ($start -lt 0) { throw 'Upscale lifecycle extraction failed' }
$lifecycle = $inputs.Substring($start)
$hudfix = Get-Content -Raw (Join-Path $repo 'OptiScaler/hudfix/Hudfix_Dx12.cpp')
$start = $hudfix.IndexOf('void Hudfix_Dx12::UpscaleEnd(')
$end = $hudfix.IndexOf('bool Hudfix_Dx12::SkipHudlessChecks()', $start)
if ($start -lt 0 -or $end -le $start) { throw 'HUDfix lifecycle extraction failed' }
Set-Content -LiteralPath (Join-Path $output 'late-lifecycle-production.h') -Value ($lifecycle + $hudfix.Substring($start, $end - $start))
& $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl /nologo /EHsc /std:c++20 /O2 /W4 /I`"$include`" /I`"$nr`" /I`"$output`" `"$cpp`" /Fo`"$obj`" /Fe`"$exe`" /link d3d12.lib dxgi.lib d3dcompiler.lib"
if ($LASTEXITCODE -ne 0) { throw 'Late-scene fixture build failed' }
& $exe | Tee-Object -FilePath (Join-Path $output 'gpu-results.txt')
if ($LASTEXITCODE -ne 0) { throw 'Late-scene GPU fixture failed' }
