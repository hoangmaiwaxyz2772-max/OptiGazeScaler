$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\dlssnr-command-state-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cpp = Join-Path $PSScriptRoot 'test_dlssnr_command_state.cpp'
$exe = Join-Path $output 'command-state-test.exe'
$obj = Join-Path $output 'command-state-test.obj'
$include = Join-Path $repo 'OptiScaler'
& $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl /nologo /EHsc /std:c++20 /O2 /W4 /I`"$include`" `"$cpp`" /Fo`"$obj`" /Fe`"$exe`" /link d3d12.lib dxgi.lib"
if ($LASTEXITCODE -ne 0) { throw 'Command state fixture build failed' }
& $exe | Tee-Object -FilePath (Join-Path $output 'results.txt')
if ($LASTEXITCODE -ne 0) { throw 'Command state fixture failed' }
