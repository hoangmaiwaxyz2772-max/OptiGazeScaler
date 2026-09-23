$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\dxgi-adapter-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cpp = Join-Path $PSScriptRoot 'test_dxgi_adapter_selection.cpp'
$exe = Join-Path $output 'adapter-test.exe'
$obj = Join-Path $output 'adapter-test.obj'
& $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl /nologo /EHsc /std:c++20 /O2 /W4 `"$cpp`" /Fo`"$obj`" /Fe`"$exe`""
if ($LASTEXITCODE -ne 0) { throw 'DXGI adapter test build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'DXGI adapter selection failed' }
