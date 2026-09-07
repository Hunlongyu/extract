[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Checksums,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
# 手动真实样本验证：只读取数据，不安装或运行载荷，也不下载文件。
$hashes = @{}
foreach ($line in Get-Content -LiteralPath $Checksums) {
    if ($line -match '^([a-fA-F0-9]{64})  (.+)$') { $hashes[$Matches[2]] = $Matches[1] }
}
$packageKey = 'w64/' + [IO.Path]::GetFileName($Package)
if ((Get-FileHash -LiteralPath $Package).Hash -ne $hashes[$packageKey]) { throw 'MSI does not match official SHA-256.' }
$report = Get-Content -LiteralPath (Join-Path $OutputDirectory '_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($report.status -ne 'complete' -or $report.files.Count -ne 10) { throw 'Unexpected PuTTY 0.85 file catalog.' }
$verified = 0
foreach ($file in $report.files) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory $file.path)).Hash
    if ($actual -ne $file.sha256) { throw "Report mismatch: $($file.path)" }
    $name = [IO.Path]::GetFileName($file.path)
    $officialKey = if ($name.EndsWith('.exe')) { "w64/$name (installer version)" } elseif ($name -eq 'putty.chm') { 'putty.chm' } else { $null }
    if ($officialKey) {
        if ($actual -ne $hashes[$officialKey]) { throw "Official hash mismatch: $name" }
        ++$verified
    } elseif (-not $file.msiHashVerified) { throw "No MSI hash verification for $name" }
}
if ($verified -ne 7) { throw 'Expected six executables and one CHM verified against published hashes.' }
Write-Output 'PASS: PuTTY 0.85 MSI official SHA-256; 10 output hashes; 6 EXE + CHM official hashes; remaining 3 files checked via MsiFileHash.'
