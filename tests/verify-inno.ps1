[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('6.5.4','6.6.1','7.1.0-x64')][string]$Version,
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$ReferenceFiles
)
$ErrorActionPreference = 'Stop'
# 只读真实样本验证；不下载、不运行安装器或导出的载荷。
$samples = @{
    '6.5.4' = @('fa73bf47a4da250d185d07561c2bfda387e5e20db77e4570004cf6a133cc10b1', '6.5.2', 115, 20134579)
    '6.6.1' = @('d243ce440c02705530699554fb9612b9b2bd7a2a90629cdb7f41e66f5faeb91f', '6.6.1', 119, 25982592)
    '7.1.0-x64' = @('0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f', '7.0.0.3', 135, 52674963)
}
$expected = $samples[$Version]
if ((Get-FileHash -LiteralPath $Package).Hash -ne $expected[0]) { throw 'Official package digest mismatch.' }
$report = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $OutputDirectory '_extract-report.json') | ConvertFrom-Json
if ($report.format -ne 'Inno Setup' -or $report.formatVersion -ne $expected[1] -or $report.status -ne 'complete' -or
    $report.files.Count -ne $expected[2] -or $report.totalBytes -ne $expected[3]) { throw 'Unexpected file catalog.' }
$referenceMatches = 0
foreach ($file in $report.files) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $OutputDirectory $file.path)).Hash
    if ($actual -ne $file.sha256 -or -not $file.sourceHashVerified) { throw "File hash verification failed: $($file.path)" }
    if ($ReferenceFiles -and $file.path.StartsWith('app/')) {
        $reference = Join-Path $ReferenceFiles $file.path.Substring(4)
        if (Test-Path -LiteralPath $reference -PathType Leaf) {
            if ($actual -ne (Get-FileHash -LiteralPath $reference).Hash) { throw "Producer file differs: $reference" }
            $referenceMatches++
        }
    }
}
if ($ReferenceFiles -and $Version -eq '7.1.0-x64' -and $referenceMatches -ne 69) { throw 'Expected 69 producer source-file matches.' }
[pscustomobject]@{Version=$Version;Files=$report.files.Count;Bytes=$report.totalBytes;Verified=$true;ProducerFileMatches=$referenceMatches} | ConvertTo-Json -Compress
