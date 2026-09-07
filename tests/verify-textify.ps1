[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
# 用户回归样本：仅核对数据与提取结果，不执行安装器或程序。
if ((Get-FileHash -LiteralPath $Package).Hash -ne 'cef497c5237f4b10b09fd4d45b051f3777f9705debb2bb6bf7dcf7ffe400b01e') {
    throw 'Not the recorded Textify regression sample.'
}
# 这些 SHA-1 在实现兼容分支之前从原始位置表独立读取。
$expected = @{
    'app/Textify.exe' = 'b50560cef7c81562eea7bb0b261afbbdea49a37c'
    'app/data/config.json' = '964d03dfe08aa36d8aec77b69189cc055e32059b'
    'app/data/icons/baidu.ico' = 'af9841b6f0923f890f41feec52c94a0cd68f01d8'
    'app/data/icons/bdfy.ico' = '0eabbbf9058f607246ad3f31778d0dac2f0b3844'
    'app/data/icons/close.ico' = '977e06ba32800845a2f2c11dcde582171eadb66d'
    'app/data/icons/copy.ico' = '6600f8550c3b8fa69ca979b49a989cde3b230316'
}
$report=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $OutputDirectory '_extract-report.json') | ConvertFrom-Json
if ($report.formatVersion -ne '6.1.0 (u)' -or $report.status -ne 'complete' -or $report.files.Count -ne 6 -or $report.totalBytes -ne 236025) {
    throw 'Unexpected Textify file catalog.'
}
$seen=@{}
foreach ($file in $report.files) {
    if (-not $expected.ContainsKey($file.path) -or $seen.ContainsKey($file.path)) { throw 'Unexpected/duplicate file.' }
    $seen[$file.path]=$true
    $path=Join-Path $OutputDirectory $file.path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA1).Hash -ne $expected[$file.path] -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256 -or
        $file.sourceHashAlgorithm -ne 'SHA-1' -or -not $file.sourceHashVerified) { throw "Verification failed: $($file.path)" }
}
Write-Output 'PASS: Textify input unchanged; six files match independent SHA-1 baselines and report SHA-256.'
