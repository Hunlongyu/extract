param([Parameter(Mandatory)][string]$Spec)
$ErrorActionPreference = 'Stop'
# 仅复用测试的 COM 表写入辅助函数，不执行 integration.ps1 的测试主体。
$helpers = Get-Content (Join-Path $PSScriptRoot 'integration.ps1') -Raw
$begin = $helpers.IndexOf('function Com-Call(')
$end = $helpers.IndexOf('function Variant(')
. ([scriptblock]::Create($helpers.Substring($begin, $end - $begin)))
$data = Get-Content -LiteralPath $Spec -Raw | ConvertFrom-Json
$installer = New-Object -ComObject WindowsInstaller.Installer
try {
    $db = Com-Call $installer 'OpenDatabase' @($data.output, 3)
    try {
        Sql $db 'CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)'
        Sql $db 'CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL PRIMARY KEY `Component`)'
        Sql $db 'CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)'
        Sql $db 'CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `Cabinet` CHAR(255) PRIMARY KEY `DiskId`)'
        Sql $db 'CREATE TABLE `MsiFileHash` (`File_` CHAR(72) NOT NULL, `Options` SHORT NOT NULL, `HashPart1` LONG NOT NULL, `HashPart2` LONG NOT NULL, `HashPart3` LONG NOT NULL, `HashPart4` LONG NOT NULL PRIMARY KEY `File_`)'
        Sql $db 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('TARGETDIR', $null, 'SourceDir')
        Sql $db 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('APP', 'TARGETDIR', 'app')
        Sql $db 'INSERT INTO `Component` (`Component`, `Directory_`) VALUES (?, ?)' @('C1', 'APP')
        $index = 0
        foreach ($file in $data.files) {
            $index++
            Sql $db 'INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence`) VALUES (?, ?, ?, ?, ?, ?)' @($file.name, 'C1', $file.name, [int](Get-Item -LiteralPath $file.source).Length, 0, $index)
            $hash = Com-Call $installer 'FileHash' @($file.source, 0)
            try {
                $values = @($file.name, 0)
                for ($i=1; $i -le 4; $i++) { $values += [int](Com-Get $hash 'IntegerData' @($i)) }
                Sql $db 'INSERT INTO `MsiFileHash` (`File_`, `Options`, `HashPart1`, `HashPart2`, `HashPart3`, `HashPart4`) VALUES (?, ?, ?, ?, ?, ?)' $values
            } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($hash) | Out-Null }
        }
        $disk = 0
        foreach ($cab in $data.cabs) {
            $disk++
            $name = if ($cab.embedded) { '#' + $cab.name } else { $cab.name }
            Sql $db 'INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (?, ?, ?)' @($disk, [int]$cab.lastSequence, $name)
            if ($cab.embedded) { Embed-Cab $db $cab.source -Name $cab.name }
        }
        $summary = Com-Get $db 'SummaryInformation' @(20)
        try {
            Com-Set $summary 'Property' @(1, 1252)
            Com-Set $summary 'Property' @(7, 'Intel;1033')
            Com-Set $summary 'Property' @(9, ('{' + [Guid]::NewGuid().ToString().ToUpperInvariant() + '}'))
            Com-Set $summary 'Property' @(14, 200)
            Com-Set $summary 'Property' @(15, 2)
            Com-Call $summary 'Persist'
        } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) | Out-Null }
        Com-Call $db 'Commit'
    } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($db) | Out-Null }
} finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null }
