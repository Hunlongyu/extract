[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw '找不到 vswhere.exe，请安装 Visual Studio C++ 桌面开发工具。'
}

$vsRoot = & $vswherePath -latest -prerelease -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsRoot)) {
    throw '未找到包含 x64 C++ 工具链的 Visual Studio。'
}
$vcvarsPath = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvarsPath)) {
    throw "找不到工具链初始化脚本：$vcvarsPath"
}

# 在子进程中初始化 MSVC，再导入当前构建进程；不写入用户或系统环境配置。
$setupCommand = 'call "{0}" >nul && set' -f $vcvarsPath
$toolchainEnvironment = & $env:ComSpec /d /s /c $setupCommand
if ($LASTEXITCODE -ne 0) {
    throw 'MSVC 环境初始化失败。'
}
foreach ($entry in $toolchainEnvironment) {
    $separator = $entry.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable(
            $entry.Substring(0, $separator), $entry.Substring($separator + 1), 'Process')
    }
}

$cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmakePath)) {
    $cmakePath = (Get-Command cmake.exe -ErrorAction Stop).Source
}
$ninjaDirectory = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path -LiteralPath (Join-Path $ninjaDirectory 'ninja.exe')) {
    $env:PATH = $ninjaDirectory + [IO.Path]::PathSeparator + $env:PATH
}

$preset = 'win-x64-' + $Configuration.ToLowerInvariant()
Push-Location -LiteralPath $projectRoot
try {
    & $cmakePath --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }

    & $cmakePath --build --preset $preset --parallel
    if ($LASTEXITCODE -ne 0) { throw '编译失败。' }

    if ($Install) {
        & $cmakePath --install (Join-Path $projectRoot "out/build/$preset")
        if ($LASTEXITCODE -ne 0) { throw '生成便携目录失败。' }
    }
} finally {
    Pop-Location
}
