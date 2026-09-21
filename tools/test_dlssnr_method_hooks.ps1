$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\dlssnr-method-hooks'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$source = Get-Content -Raw (Join-Path $repo 'OptiScaler\hooks\D3D12_Hooks.cpp')
$start = $source.IndexOf('using PFN_SetDescriptorHeaps =')
$end = $source.IndexOf('template <typename T> struct RootRestoreHook', $start)
if ($start -lt 0 -or $end -le $start) { throw 'State method type extraction failed' }
$generated = $source.Substring($start, $end - $start)
$start = $source.IndexOf('// Independent late-NR state capture:')
$end = $source.IndexOf('static ', $source.IndexOf('void D3D12Hooks::ReleaseLateCommandStateHooks()', $start) + 1)
# The next original static declaration terminates the complete production block.
if ($start -lt 0 -or $end -le $start) { throw 'State hook extraction failed' }
$generated += $source.Substring($start, $end - $start)
Set-Content -LiteralPath (Join-Path $output 'late-state-hooks-production.h') -Value $generated
$tracking = Get-Content -Raw (Join-Path $repo 'OptiScaler\resource_tracking\ResTrack_dx12.cpp')
$start = $tracking.IndexOf('using PFN_LateReset =')
$end = $tracking.IndexOf('static PFN_ExecuteCommandLists', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Lifecycle hook extraction failed' }
$lifecycle = $tracking.Substring($start, $end - $start)
$start = $tracking.IndexOf('using LateCreateListFn =')
$end = $tracking.IndexOf('void ResTrack_Dx12::HookDevice(', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Creation hook extraction failed' }
$lifecycle += $tracking.Substring($start, $end - $start)
Set-Content -LiteralPath (Join-Path $output 'late-lifecycle-hooks-production.h') -Value $lifecycle
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cpp = Join-Path $PSScriptRoot 'test_dlssnr_method_hooks.cpp'
$exe = Join-Path $output 'method-hooks-test.exe'
$obj = Join-Path $output 'method-hooks-test.obj'
$include = Join-Path $repo 'OptiScaler\include'
$nr = Join-Path $repo 'OptiScaler\upscalers\dlssnr'
$detours = Join-Path $repo 'OptiScaler\library\detours\detours.lib'
& $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl /nologo /EHsc /std:c++20 /O2 /Ob0 /W4 /I`"$include`" /I`"$nr`" /I`"$output`" `"$cpp`" /Fo`"$obj`" /Fe`"$exe`" /link /OPT:NOICF `"$detours`""
if ($LASTEXITCODE -ne 0) { throw 'Method hook fixture build failed' }
& $exe | Tee-Object -FilePath (Join-Path $output 'results.txt')
if ($LASTEXITCODE -ne 0) { throw 'Method hook fixture failed' }
