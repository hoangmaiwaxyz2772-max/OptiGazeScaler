$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\descriptor-tracking-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$resource = Get-Content -Raw (Join-Path $repo 'OptiScaler\hudfix\Hudfix_Dx12.h')
$begin = $resource.IndexOf('enum ResourceType')
$end = $resource.IndexOf('typedef struct HudlessInfo', $begin)
$generated = $resource.Substring($begin, $end-$begin)
$heap = Get-Content -Raw (Join-Path $repo 'OptiScaler\resource_tracking\ResTrack_dx12.h')
$begin = $heap.IndexOf('#define USE_SPINLOCK_MUTEX')
$end = $heap.IndexOf('struct ResourceHeapInfo', $begin)
$generated += $heap.Substring($begin, $end-$begin)
Set-Content -LiteralPath (Join-Path $output 'descriptor-heap-production.h') -Value $generated
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cpp = Join-Path $PSScriptRoot 'test_descriptor_tracking.cpp'
$exe = Join-Path $output 'descriptor-test.exe'
$obj = Join-Path $output 'descriptor-test.obj'
$include = Join-Path $repo 'external\unordered_dense\include'
& $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl /nologo /EHsc /std:c++20 /O2 /W4 /I`"$include`" /I`"$output`" `"$cpp`" /Fo`"$obj`" /Fe`"$exe`""
if ($LASTEXITCODE -ne 0) { throw 'Descriptor tracking fixture build failed' }
& $exe | Tee-Object -FilePath (Join-Path $output 'benchmark.txt')
if ($LASTEXITCODE -ne 0) { throw 'Descriptor tracking regression failed' }
