[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$AssetDirectory,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw '无效的发布标签。' }
$AssetDirectory = (Resolve-Path -LiteralPath $AssetDirectory).Path
$assets = @()
$checksums = @()
foreach ($arch in @('x86', 'x64', 'arm64')) {
    $name = "Extract-$Tag-windows-$arch.exe"
    $path = Join-Path $AssetDirectory $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $line = "$hash  $name"
    if ((Get-Content -LiteralPath "$path.sha256" -Raw).Trim() -cne $line) { throw "$name 校验和不一致。" }
    $assets += $path
    $checksums += $line
}
$noticePath = Join-Path $AssetDirectory 'THIRD-PARTY-NOTICES.txt'
$notice = "Extract - Third-party notices`nKeep these notices when redistributing Extract.`n"
foreach ($relative in @('third_party/lzma/LICENSE.txt', 'third_party/zlib/LICENSE', 'third_party/bzip2/LICENSE',
    'third_party/notices/Inno-Setup-LICENSE.txt', 'third_party/notices/NSIS-LICENSE.txt')) {
    $notice += "`n===== $relative =====`n" + (Get-Content -LiteralPath (Join-Path $root $relative) -Raw) + "`n"
}
[IO.File]::WriteAllText($noticePath, $notice)
$assets += $noticePath
$checksums += (Get-FileHash -LiteralPath $noticePath -Algorithm SHA256).Hash.ToLowerInvariant() + '  THIRD-PARTY-NOTICES.txt'
$checksumPath = Join-Path $AssetDirectory 'SHA256SUMS.txt'
[IO.File]::WriteAllText($checksumPath, ($checksums -join "`n") + "`n")
$assets += $checksumPath
if ($PrepareOnly) { Write-Host '已准备并校验 3 个 EXE、许可和校验和；未发布。'; return }

# 本地只允许准备资产；发布必须来自推送版本标签触发的工作流。
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:GITHUB_EVENT_NAME -ne 'push' -or $env:GITHUB_REF -cne "refs/tags/$Tag") {
    throw '发布仅允许在版本标签 push 触发的 GitHub Actions 中进行。本地检查使用 -PrepareOnly。'
}
$notesPath = Join-Path $root "docs/releases/$Tag.md"
if (-not (Test-Path -LiteralPath $notesPath -PathType Leaf) -or
    [string]::IsNullOrWhiteSpace((Get-Content -LiteralPath $notesPath -Raw))) {
    throw '缺少本次版本的发布说明，拒绝创建 Release。'
}
& gh release view $Tag --json tagName 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw '此标签已有 Release（包括草稿），拒绝覆盖。请检查既有发布后处理。' }
& gh release create $Tag @assets --verify-tag --draft --title "Extract $Tag" --notes-file $notesPath
if ($LASTEXITCODE -ne 0) { throw '创建 Release 草稿或上传资产失败；请检查远程草稿。' }

# 上传后重新下载核对，三架构完整才公开；失败保留草稿供排查。
$verifyDirectory = Join-Path $AssetDirectory ('verify-' + [Guid]::NewGuid().ToString('N'))
& gh release download $Tag --dir $verifyDirectory
if ($LASTEXITCODE -ne 0) { throw 'Release 资产回读失败，已保留草稿。' }
$downloaded = @(Get-ChildItem -LiteralPath $verifyDirectory -File)
if ($downloaded.Count -ne $assets.Count) { throw 'Release 资产数量不完整，已保留草稿。' }
foreach ($path in $assets) {
    $remote = Join-Path $verifyDirectory ([IO.Path]::GetFileName($path))
    if ((Get-FileHash -LiteralPath $remote -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash) {
        throw 'Release 资产回读摘要不匹配，已保留草稿。'
    }
}
& gh release edit $Tag --draft=false
if ($LASTEXITCODE -ne 0) { throw '发布 Release 失败，检查远程草稿状态。' }
