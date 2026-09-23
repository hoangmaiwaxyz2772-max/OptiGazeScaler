param([switch]$Benchmark)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\input-regressions'
New-Item -ItemType Directory -Force -Path $output | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $output 'hooks') | Out-Null

# Compile the production input sources. Only the application state/logger and
# export proxy are isolated; the proxy still resolves the real Win32 exports.
@'
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct State {
    bool isRunningOnLinux = false;
    static State& Instance() { static State state; return state; }
};
'@ | Set-Content -LiteralPath (Join-Path $output 'pch.h') -Encoding utf8
@'
#pragma once
#include <windows.h>
struct KernelBaseProxy {
    static auto GetProcAddress_() { return &::GetProcAddress; }
};
'@ | Set-Content -LiteralPath (Join-Path $output 'hooks\Kernel_Hooks.h') -Encoding utf8

$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'OptiScaler\menu\input') -Filter '*.cpp' |
    ForEach-Object { '"' + $_.FullName + '"' })
$sources += '"' + (Join-Path $PSScriptRoot 'test_input_polling.cpp') + '"'
foreach ($source in @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'misc\freetype\imgui_freetype.cpp')) {
    $sources += '"' + (Join-Path $repo ('OptiScaler\include\imgui\' + $source)) + '"'
}
$exe = Join-Path $output 'input-test.exe'
$argsFile = Join-Path $output 'build.rsp'
$compileArgs = @('/nologo', '/EHsc', '/std:c++latest', '/O2', '/MD', '/Gy', '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR', '/W3')
foreach ($include in @($output, (Join-Path $repo 'OptiScaler'), (Join-Path $repo 'OptiScaler\include'), (Join-Path $repo 'external\freetype'))) {
    $compileArgs += '/I"' + $include + '"'
}
$compileArgs += $sources
$compileArgs += @('/Fe"' + $exe + '"', '/link', '/OPT:REF', '/OPT:ICF',
    '"' + (Join-Path $repo 'OptiScaler\library\detours\detours.lib') + '"',
    '"' + (Join-Path $repo 'external\freetype\freetype.lib') + '"',
    'user32.lib', 'gdi32.lib', 'imm32.lib', 'hid.lib', 'dxguid.lib')
$compileArgs | Set-Content -LiteralPath $argsFile -Encoding ascii
$dev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
Push-Location -LiteralPath $output
try {
    & $env:ComSpec /d /c "call `"$dev`" -arch=x64 -host_arch=x64 >nul && cl @`"$argsFile`""
    if ($LASTEXITCODE -ne 0) { throw 'Input regression compilation failed.' }
}
finally { Pop-Location }
if ($Benchmark) { & $exe --benchmark | Tee-Object -FilePath (Join-Path $output 'results.txt') }
else { & $exe | Tee-Object -FilePath (Join-Path $output 'results.txt') }
if ($LASTEXITCODE -ne 0) { throw 'Input regressions failed.' }
