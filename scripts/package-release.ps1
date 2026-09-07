[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('x86', 'x64', 'arm64')][string]$Architecture,
    [Parameter(Mandatory)][string]$Tag,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw '发布标签必须为 v主版本.次版本.修订版本，例如 v0.6.1。'
}
$version = $Tag.Substring(1)
$cmake = Get-Content -LiteralPath (Join-Path $root CMakeLists.txt) -Raw
if ($cmake -notmatch 'project\(Extract VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES' -or $Matches[1] -cne $version) {
    throw '发布标签与 CMake 项目版本不一致。'
}
$build = Join-Path $root "out/build/win-$Architecture-release"
$exe = Join-Path $build 'bin/Extract.exe'
$commands = @(Get-Content -LiteralPath (Join-Path $build compile_commands.json) -Raw | ConvertFrom-Json |
    Where-Object { [IO.Path]::GetExtension($_.file) -in @('.c', '.cpp', '.cc', '.cxx') })
if (-not $commands.Count) { throw '缺少编译命令，无法验证静态 CRT。' }
foreach ($entry in $commands) {
    if ($entry.command -cnotmatch '(?:^|\s)[/-]MT(?:\s|$)' -or $entry.command -cmatch '(?:^|\s)[/-](?:MDd?|MTd)(?:\s|$)') {
        throw "发现非 Release 静态 CRT 编译命令：$($entry.file)"
    }
}
$bytes = [IO.File]::ReadAllBytes($exe)
if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw '无效的 EXE。' }
$pe = [BitConverter]::ToInt32($bytes, 60)
if ($pe -lt 64 -or $pe -gt $bytes.Length - 96 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) {
    throw '无效的 PE 头。'
}
$machines = @{ x86 = 0x14c; x64 = 0x8664; arm64 = 0xaa64 }
if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne $machines[$Architecture]) { throw 'EXE 架构与发布名称不匹配。' }
if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) { throw 'EXE 必须使用 Windows GUI 子系统。' }
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($info.ProductVersion -cne $version -or $info.FileVersion -cne $version) { throw 'EXE 内置版本不匹配。' }

# dumpbin 同时报告普通导入及延迟导入。仅允许系统 DLL，防止意外附带运行库依赖。
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw '找不到用于依赖检查的 Visual Studio。' }
    $dumpbin = Get-ChildItem -Path (Join-Path $vs 'VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe') |
        Sort-Object { [version]$_.Directory.Parent.Parent.Parent.Name } -Descending | Select-Object -First 1
}
if (-not $dumpbin) { throw '找不到 dumpbin.exe。' }
$dependencies = & $dumpbin /nologo /dependents $exe
if ($LASTEXITCODE -ne 0) { throw 'PE 依赖检查失败。' }
$dlls = @($dependencies | ForEach-Object { if ($_ -match '^\s+([\w.\-]+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
if (-not $dlls.Count) { throw '未读到 PE 依赖，拒绝发布。' }
$systemDlls = @('kernel32.dll','user32.dll','gdi32.dll','advapi32.dll','shell32.dll','ole32.dll',
    'oleaut32.dll','propsys.dll','runtimeobject.dll','msi.dll','cabinet.dll','bcrypt.dll','ntdll.dll',
    'xmllite.dll','shlwapi.dll','combase.dll','uuid.dll')
foreach ($dll in $dlls) {
    if ($dll -notin $systemDlls -and $dll -notmatch '^api-ms-win-(?:core|security|eventing)-[\w-]+\.dll$') {
        throw "发现未认可的 DLL 依赖：$dll"
    }
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root "out/release/$Tag/$Architecture" }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$asset = Join-Path $OutputDirectory "Extract-$Tag-windows-$Architecture.exe"
Copy-Item -LiteralPath $exe -Destination $asset -Force
$hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$asset.sha256", "$hash  $([IO.Path]::GetFileName($asset))`n")
Write-Host "已验证并打包：$asset"
Write-Host "系统依赖：$($dlls -join ', ')"
