[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x86', 'x64', 'arm64')]
    [string]$Architecture = 'x64',
    [switch]$WithoutTests,
    [switch]$Install,
    [switch]$Test,
    [string]$NsisCompiler,
    [string]$SevenZipTestTool,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
if ($WithoutTests -and $Test) { throw '-WithoutTests 与 -Test 不能同时使用。' }
if ($Test -and $Architecture -eq 'arm64' -and $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') {
    throw 'ARM64 测试必须在 ARM64 Windows 上运行；交叉编译请使用 -WithoutTests。'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw '找不到 vswhere.exe，请安装 Visual Studio C++ 桌面开发工具。'
}

$requiredComponent = if ($Architecture -eq 'arm64') {
    'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
} else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$vsRoot = & $vswherePath -latest -prerelease -products '*' `
    -requires $requiredComponent -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsRoot)) {
    throw "未找到包含 $Architecture C++ 工具链的 Visual Studio（$requiredComponent）。"
}
$vcvarsPath = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvarsPath)) {
    throw "找不到工具链初始化脚本：$vcvarsPath"
}

# 在子进程中初始化 MSVC，再导入当前构建进程；不写入用户或系统环境配置。
$toolchainTarget = @{ x86 = 'amd64_x86'; x64 = 'amd64'; arm64 = 'amd64_arm64' }[$Architecture]
$setupCommand = 'call "{0}" {1} >nul && set' -f $vcvarsPath, $toolchainTarget
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

$preset = "win-$Architecture-" + $Configuration.ToLowerInvariant()
Push-Location -LiteralPath $projectRoot
try {
    $configureArguments = @('--preset', $preset)
    $configureArguments += '-DBUILD_TESTING=' + $(if ($WithoutTests) { 'OFF' } else { 'ON' })
    if ($Fresh) { $configureArguments += '--fresh' }
    if ($NsisCompiler) {
        $resolvedCompiler = (Resolve-Path -LiteralPath $NsisCompiler -ErrorAction Stop).Path
        $configureArguments += "-DEXTRACT_NSIS_COMPILER=$resolvedCompiler"
    }
    if ($SevenZipTestTool) {
        $resolvedArchiveTool = (Resolve-Path -LiteralPath $SevenZipTestTool -ErrorAction Stop).Path
        $configureArguments += "-DEXTRACT_7Z_TEST_TOOL=$resolvedArchiveTool"
    }
    & $cmakePath @configureArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }

    & $cmakePath --build --preset $preset --parallel
    if ($LASTEXITCODE -ne 0) { throw '编译失败。' }

    if ($Test) {
        $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
        & $ctestPath --test-dir (Join-Path $projectRoot "out/build/$preset") --output-on-failure `
            --no-tests=error --output-junit (Join-Path $projectRoot "out/build/$preset/test-results.xml")
        if ($LASTEXITCODE -ne 0) { throw '测试失败。' }
    }

    if ($Install) {
        & $cmakePath --install (Join-Path $projectRoot "out/build/$preset")
        if ($LASTEXITCODE -ne 0) { throw '生成便携目录失败。' }
    }
} finally {
    Pop-Location
}
