param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$WorkRoot, [switch]$LargeCab)
$ErrorActionPreference = 'Stop'
trap { Write-Error (($_ | Out-String) + $_.ScriptStackTrace) -ErrorAction Continue; exit 1 }
$Executable = [IO.Path]::GetFullPath($Executable)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$root = Join-Path $WorkRoot ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

function Com-Call($Object, [string]$Name, [object[]]$Values = @()) {
    for ($i = 0; $i -lt $Values.Count; $i++) { if ($null -ne $Values[$i]) { $Values[$i] = $Values[$i].PSObject.BaseObject } }
    $result = $Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::InvokeMethod, $null, $Object, $Values)
    if ($null -ne $result -and $result -isnot [DBNull]) { return $result }
}
function Com-Get($Object, [string]$Name, [object[]]$Values = @()) {
    for ($i = 0; $i -lt $Values.Count; $i++) { if ($null -ne $Values[$i]) { $Values[$i] = $Values[$i].PSObject.BaseObject } }
    $Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::GetProperty, $null, $Object, $Values)
}
function Com-Set($Object, [string]$Name, [object[]]$Values) {
    for ($i = 0; $i -lt $Values.Count; $i++) { if ($null -ne $Values[$i]) { $Values[$i] = $Values[$i].PSObject.BaseObject } }
    [void]$Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::SetProperty, $null, $Object, $Values)
}
function Sql($Database, [string]$Statement, [object[]]$Values = @()) {
    $view = Com-Call $Database 'OpenView' @($Statement)
    $record = $null
    try {
        if ($Values.Count) {
            $record = Com-Call $installer 'CreateRecord' @($Values.Count)
            for ($i = 0; $i -lt $Values.Count; $i++) {
                if ($null -eq $Values[$i]) { continue }
                $property = if ($Values[$i] -is [int]) { 'IntegerData' } else { 'StringData' }
                Com-Set $record $property @(($i + 1), $Values[$i])
            }
        }
        Com-Call $view 'Execute' @($record)
    } finally {
        if ($record) { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) | Out-Null }
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null
    }
}
function Embed-Cab($Database, [string]$Cabinet, [switch]$Update, [string]$Name = 'payload.cab') {
    $sqlText = if ($Update) { 'UPDATE `_Streams` SET `Data` = ? WHERE `Name` = ?' } else { 'INSERT INTO `_Streams` (`Data`, `Name`) VALUES (?, ?)' }
    $view = Com-Call $Database 'OpenView' @($sqlText)
    $record = Com-Call $installer 'CreateRecord' @(2)
    try {
        Com-Call $record 'SetStream' @(1, $Cabinet)
        Com-Set $record 'StringData' @(2, $Name)
        Com-Call $view 'Execute' @($record)
    } finally {
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) | Out-Null
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null
    }
}
function Variant([string]$Name, [scriptblock]$Change) {
    $path = Join-Path $root ($Name + '.msi')
    [IO.File]::Copy($package, $path)
    $database = Com-Call $installer 'OpenDatabase' @($path, 1)
    try { & $Change $database; Com-Call $database 'Commit' } finally {
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
    }
    return $path
}
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Make-Cab([string]$Name, [string[]]$Keys, [string]$Compression = 'MSZIP', [hashtable]$Aliases = @{}) {
    $ddf = @('.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on', ".Set CompressionType=$Compression",
        ".Set CabinetNameTemplate=$Name", '.Set MaxDiskSize=0', ('.Set DiskDirectoryTemplate="' + $root + '"'),
        ('.Set RptFileName="' + (Join-Path $root "$Name.rpt") + '"'), ('.Set InfFileName="' + (Join-Path $root "$Name.inf") + '"'))
    foreach ($key in $Keys) {
        $destination = if ($Aliases.ContainsKey($key)) { $Aliases[$key] } else { $key }
        $ddf += '"' + (Join-Path $source $key) + '" ' + $destination
    }
    $path = Join-Path $root "$Name.ddf"
    [IO.File]::WriteAllLines($path, [string[]]$ddf, [Text.Encoding]::Default)
    & "$env:SystemRoot\System32\makecab.exe" /F $path | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'makecab failed.'
    return (Join-Path $root $Name)
}
function Check-Content([string]$Name) {
    $directory = Join-Path $root ($Name + '_extracted')
    $report = Get-Content -LiteralPath (Join-Path $directory '_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($report.files.Count -eq 4 -and $report.status -eq 'complete') "$Name catalog or completion"
    foreach ($file in $report.files) {
        Assert ((Get-FileHash -LiteralPath (Join-Path $source $file.id)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $directory $file.path)).Hash) "$Name content mismatch"
        Assert $file.msiHashVerified "$Name MSI hash unchecked"
    }
}
function Run-App([string[]]$Arguments, [int]$Expected) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = ($Arguments | ForEach-Object { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        Assert ($process.Start()) 'Cannot start Extract.'
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Extract timed out.' }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        Assert ($process.ExitCode -eq $Expected) "Expected $Expected, received $($process.ExitCode). Arguments: $Arguments. Error: $stderr"
        [pscustomobject]@{ Out = $stdout; Error = $stderr }
    } finally { $process.Dispose() }
}

$installer = New-Object -ComObject WindowsInstaller.Installer
try {
    $source = Join-Path $root 'source'
    [IO.Directory]::CreateDirectory($source) | Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'F1'), 'Known UTF-8 content: 中文路径与空格。', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllBytes((Join-Path $source 'F2'), [byte[]](0..255))
    [IO.File]::WriteAllBytes((Join-Path $source 'F3'), [byte[]]@())
    [IO.File]::WriteAllBytes((Join-Path $source 'F4'), [byte[]](0..4096 | ForEach-Object { $_ % 251 }))
    $ddf = @('.OPTION EXPLICIT', '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=MSZIP',
        '.Set CabinetNameTemplate=payload.cab', '.Set MaxDiskSize=0', ('.Set DiskDirectoryTemplate="' + $root + '"'),
        ('.Set RptFileName="' + (Join-Path $root 'cab.rpt') + '"'), ('.Set InfFileName="' + (Join-Path $root 'cab.inf') + '"'))
    foreach ($key in @('F1','F2','F3','F4')) { $ddf += '"' + (Join-Path $source $key) + '" ' + $key }
    $ddfPath = Join-Path $root 'cab.ddf'
    [IO.File]::WriteAllLines($ddfPath, [string[]]$ddf, [Text.Encoding]::Default)
    & "$env:SystemRoot\System32\makecab.exe" /F $ddfPath | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'makecab failed.'
    $cabinet = Join-Path $root 'payload.cab'
    Assert ([IO.File]::Exists($cabinet)) 'CAB was not generated.'

    $package = Join-Path $root 'fixture.msi'
    $database = Com-Call $installer 'OpenDatabase' @($package, 3)
    try {
        [IO.File]::WriteAllText((Join-Path $root '_ForceCodepage.idt'), "`r`n`r`n65001`t_ForceCodepage`r`n", [Text.Encoding]::ASCII)
        Com-Call $database 'Import' @($root, '_ForceCodepage.idt')
        Sql $database 'CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)'
        Sql $database 'CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL PRIMARY KEY `Component`)'
        Sql $database 'CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)'
        Sql $database 'CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `Cabinet` CHAR(255) PRIMARY KEY `DiskId`)'
        Sql $database 'CREATE TABLE `MsiFileHash` (`File_` CHAR(72) NOT NULL, `Options` SHORT NOT NULL, `HashPart1` LONG NOT NULL, `HashPart2` LONG NOT NULL, `HashPart3` LONG NOT NULL, `HashPart4` LONG NOT NULL PRIMARY KEY `File_`)'
        Sql $database 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('TARGETDIR', $null, 'SourceDir')
        Sql $database 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('ProgramFilesFolder', 'TARGETDIR', '.')
        Sql $database 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('APP', 'ProgramFilesFolder', 'TESTAP~1|测试 App')
        Sql $database 'INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES (?, ?, ?)' @('NESTED', 'APP', 'NESTED|子目录:source')
        Sql $database 'INSERT INTO `Component` (`Component`, `Directory_`) VALUES (?, ?)' @('C1', 'APP')
        Sql $database 'INSERT INTO `Component` (`Component`, `Directory_`) VALUES (?, ?)' @('C2', 'NESTED')
        Sql $database 'INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (?, ?, ?)' @(1, 4, '#payload.cab')
        $names = @('README~1.TXT|说明 文档.txt','data.bin','empty.dat','app.exe')
        for ($index = 1; $index -le 4; $index++) {
            $key = 'F' + $index
            $sourceFile = Join-Path $source $key
            $component = if ($index -eq 2) { 'C2' } else { 'C1' }
            Sql $database 'INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence`) VALUES (?, ?, ?, ?, ?, ?)' @($key, $component, $names[$index - 1], [int](Get-Item -LiteralPath $sourceFile).Length, 0, $index)
            $hash = Com-Call $installer 'FileHash' @($sourceFile, 0)
            try {
                $values = @($key, 0)
                for ($i = 1; $i -le 4; $i++) { $values += [int](Com-Get $hash 'IntegerData' @($i)) }
                Sql $database 'INSERT INTO `MsiFileHash` (`File_`, `Options`, `HashPart1`, `HashPart2`, `HashPart3`, `HashPart4`) VALUES (?, ?, ?, ?, ?, ?)' $values
            } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($hash) | Out-Null }
        }
        Embed-Cab $database $cabinet
        $summary = Com-Get $database 'SummaryInformation' @(20)
        try {
            Com-Set $summary 'Property' @(1, 65001)
            Com-Set $summary 'Property' @(2, 'Extract known-content fixture')
            Com-Set $summary 'Property' @(7, 'x64;1033')
            Com-Set $summary 'Property' @(9, ('{' + [Guid]::NewGuid().ToString().ToUpperInvariant() + '}'))
            Com-Set $summary 'Property' @(14, 200)
            Com-Set $summary 'Property' @(15, 2)
            Com-Call $summary 'Persist'
        } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) | Out-Null }
        Com-Call $database 'Commit'
    } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null }

    $originalHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    $listed = (Run-App @('--quiet','--list',$package) 0).Out | ConvertFrom-Json
    Assert ($listed.status -eq 'listed' -and $listed.files.Count -eq 4) 'File catalog incorrect.'
    Assert ($listed.files[0].path -eq 'ProgramFilesFolder/测试 App/说明 文档.txt') 'Unicode or short/long-name mapping incorrect.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $root 'fixture_extracted'))) '--list created an output directory.'
    Run-App @('--quiet',$package) 0 | Out-Null
    $output = Join-Path $root 'fixture_extracted'
    $report = Get-Content -LiteralPath (Join-Path $output '_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($file in $report.files) {
        $expected = (Get-FileHash -LiteralPath (Join-Path $source $file.id) -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath (Join-Path $output $file.path) -Algorithm SHA256).Hash
        Assert ($actual -eq $expected -and $actual -eq $file.sha256) "Hash mismatch: $($file.id)"
        Assert ($file.msiHashVerified) 'MSI hash was not checked.'
    }
    Run-App @('--quiet',$package) 0 | Out-Null
    Assert (Test-Path -LiteralPath (Join-Path $root 'fixture_extracted (2)')) 'Existing output was overwritten.'
    Assert ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -eq $originalHash) 'MSI input changed.'

    $unicodeRoot = Join-Path $root '中文 空格输入'
    [IO.Directory]::CreateDirectory($unicodeRoot) | Out-Null
    $unicodePackage = Join-Path $unicodeRoot '测试安装包.msi'
    [IO.File]::Copy($package, $unicodePackage)
    Run-App @('--quiet','--output',$unicodeRoot,$unicodePackage) 0 | Out-Null
    Assert (Test-Path -LiteralPath (Join-Path $unicodeRoot '测试安装包_extracted\ProgramFilesFolder\测试 App\说明 文档.txt')) 'Unicode input/output failed.'

    $duplicate = Variant 'duplicate' { param($db)
        Sql $db 'UPDATE `File` SET `FileName` = ?, `Component_` = ? WHERE `File` = ?' @('app.exe','C1','F2')
    }
    Run-App @('--quiet',$duplicate) 0 | Out-Null
    $duplicates = Get-Content -LiteralPath (Join-Path $root 'duplicate_extracted\_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ((@($duplicates.files | Where-Object { $_.path.StartsWith('_variants/') })).Count -eq 2) 'Conflicting files were not preserved.'

    $prefix = Variant 'prefix' { param($db)
        Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @('child', 'F1')
        Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @('child.txt', 'F4')
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('child', 'NESTED')
    }
    Run-App @('--quiet',$prefix) 0 | Out-Null
    $prefixReport = Get-Content -LiteralPath (Join-Path $root 'prefix_extracted\_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ((@($prefixReport.files | Where-Object { $_.path.StartsWith('_variants/') })).Count -eq 2) 'File/directory prefix collision not preserved.'

    $nohash = Variant 'nohash' { param($db) Sql $db 'DROP TABLE `MsiFileHash`' }
    Run-App @('--quiet',$nohash) 0 | Out-Null
    $nohashReport = Get-Content -LiteralPath (Join-Path $root 'nohash_extracted\_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ((@($nohashReport.files | Where-Object msiHashVerified)).Count -eq 0) 'Missing MSI hashes were reported as verified.'
    foreach ($file in $nohashReport.files) {
        Assert ((Get-FileHash -LiteralPath (Join-Path $source $file.id)).Hash -eq $file.sha256) 'SHA-256 without MsiFileHash incorrect.'
    }

    $occupied = Variant 'occupied' { param($db) }
    [IO.File]::WriteAllText((Join-Path $root 'occupied_extracted'), 'must not be overwritten')
    Run-App @('--quiet',$occupied) 0 | Out-Null
    Assert ([IO.File]::ReadAllText((Join-Path $root 'occupied_extracted')) -eq 'must not be overwritten') 'Existing file overwritten.'
    Assert (Test-Path -LiteralPath (Join-Path $root 'occupied_extracted (2)')) 'Existing file name was not avoided.'

    $external = Variant 'external' { param($db) Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('payload.cab') }
    Run-App @('--quiet',$external) 0 | Out-Null
    if ($LargeCab) {
        # 合法 CAB 尾部填充，不增加展开量；同时跨过旧 MSI/CAB 输入阈值。
        $largeCabPath = Join-Path $root 'large-padded.cab'
        [IO.File]::Copy($cabinet, $largeCabPath)
        $largeCabSize = 513L * 1024 * 1024
        $stream = [IO.File]::Open($largeCabPath, [IO.FileMode]::Open, [IO.FileAccess]::Write)
        try {
            $stream.SetLength($largeCabSize)
            $stream.Position = 8
            $lengthBytes = [BitConverter]::GetBytes([uint32]$largeCabSize)
            $stream.Write($lengthBytes, 0, $lengthBytes.Length)
        } finally { $stream.Dispose() }
        $largeEmbedded = Variant 'large-embedded' { param($db) Embed-Cab $db $largeCabPath -Update }
        Assert ((Get-Item $largeEmbedded).Length -gt 512L * 1024 * 1024) 'Large MSI fixture too small.'
        Run-App @('--quiet',$largeEmbedded) 0 | Out-Null
        Check-Content 'large-embedded'
        $largeExternal = Variant 'large-external' { param($db) Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('large-padded.cab') }
        Run-App @('--quiet',$largeExternal) 0 | Out-Null
        Check-Content 'large-external'
        Write-Output 'PASS: MSI >512 MiB and embedded/external CAB >512 MiB, payload hashes verified.'
        return # 扩展模式只运行大流用例；普通 CTest 独立覆盖余下反例。
    }
    Check-Content 'external'
    $cab1 = Make-Cab 'part1.cab' @('F2','F1')
    $cab2 = Make-Cab 'part2.cab' @('F4','F3') 'LZX'
    $multi = Variant 'multi' { param($db)
        Sql $db 'UPDATE `Media` SET `LastSequence` = ?, `Cabinet` = ?' @(2, '#part1.cab')
        Sql $db 'INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (?, ?, ?)' @(2, 4, '#part2.cab')
        Embed-Cab $db $cab1 -Name 'part1.cab'
        Embed-Cab $db $cab2 -Name 'part2.cab'
    }
    Run-App @('--quiet',$multi) 0 | Out-Null
    Check-Content 'multi'
    $multiExternal = Variant 'multi-external' { param($db)
        Sql $db 'UPDATE `Media` SET `LastSequence` = ?, `Cabinet` = ?' @(2, 'part1.cab')
        Sql $db 'INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (?, ?, ?)' @(2, 4, 'part2.cab')
    }
    Run-App @('--quiet',$multiExternal) 0 | Out-Null
    Check-Content 'multi-external'
    $sourceTree = Join-Path $root 'source-layout'
    [IO.Directory]::CreateDirectory((Join-Path $sourceTree 'source')) | Out-Null
    [IO.File]::Copy((Join-Path $source 'F1'), (Join-Path $sourceTree '说明 文档.txt'))
    [IO.File]::Copy((Join-Path $source 'F2'), (Join-Path $sourceTree 'source\data.bin'))
    [IO.File]::Copy((Join-Path $source 'F3'), (Join-Path $sourceTree 'empty.dat'))
    [IO.File]::Copy((Join-Path $source 'F4'), (Join-Path $sourceTree 'app.exe'))
    $loose = Variant 'loose' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:source-layout','APP')
        Sql $db 'UPDATE `File` SET `Attributes` = ?' @(8192)
    }
    Run-App @('--quiet',$loose) 0 | Out-Null
    Check-Content 'loose'
    $looseSequence = Variant 'loose-sequence' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:source-layout','APP')
        Sql $db 'UPDATE `File` SET `Attributes` = ?' @(8192)
        Sql $db 'UPDATE `File` SET `Sequence` = ?' @(1)
    }
    Run-App @('--quiet',$looseSequence) 0 | Out-Null
    Check-Content 'loose-sequence'
    $longKey = 'Long_' + ('identifier_' * 22)
    [IO.File]::Copy((Join-Path $source 'F1'), (Join-Path $source $longKey))
    $longCab = Make-Cab 'long-key.cab' @('F1','F2','F3','F4') -Aliases @{F1=$longKey}
    $longId = Variant 'long-key' { param($db)
        Sql $db 'DELETE FROM `File` WHERE `File` = ?' @('F1')
        Sql $db 'DELETE FROM `MsiFileHash` WHERE `File_` = ?' @('F1')
        Sql $db 'INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence`) VALUES (?, ?, ?, ?, ?, ?)' @($longKey,'C1',$names[0],[int](Get-Item -LiteralPath (Join-Path $source 'F1')).Length,0,1)
        Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @($names[0],'F4')
        $hash = Com-Call $installer 'FileHash' @((Join-Path $source 'F1'),0)
        try {
            $values = @($longKey,0)
            for ($i = 1; $i -le 4; $i++) { $values += [int](Com-Get $hash 'IntegerData' @($i)) }
            Sql $db 'INSERT INTO `MsiFileHash` (`File_`, `Options`, `HashPart1`, `HashPart2`, `HashPart3`, `HashPart4`) VALUES (?, ?, ?, ?, ?, ?)' $values
        } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($hash) | Out-Null }
        Embed-Cab $db $longCab -Update
    }
    Run-App @('--quiet',$longId) 0 | Out-Null
    Check-Content 'long-key'
    $empty = Variant 'configuration-only' { param($db)
        Sql $db 'DELETE FROM `File`'
        Sql $db 'DELETE FROM `MsiFileHash`'
        Sql $db 'DELETE FROM `Media`'
    }
    Run-App @('--quiet',$empty) 0 | Out-Null
    $emptyReport = Get-Content -LiteralPath (Join-Path $root 'configuration-only_extracted\_extract-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($emptyReport.files.Count -eq 0 -and $emptyReport.status -eq 'complete' -and $emptyReport.notes.Count -gt 0) 'Empty MSI was not explicitly reported.'
    $mixed = Variant 'mixed' { param($db)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:source-layout','APP')
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('#part1.cab')
        Embed-Cab $db $cab1 -Name 'part1.cab'
        Sql $db 'UPDATE `File` SET `Attributes` = ? WHERE `Sequence` > ?' @(8192,2)
    }
    Run-App @('--quiet',$mixed) 0 | Out-Null
    Check-Content 'mixed'
    $shortRoot = Join-Path $root 'SHORT'
    [IO.Directory]::CreateDirectory((Join-Path $shortRoot 'SRC')) | Out-Null
    foreach ($pair in @(@('F1','README~1.TXT'),@('F2','SRC\data.bin'),@('F3','empty.dat'),@('F4','app.exe'))) {
        [IO.File]::Copy((Join-Path $source $pair[0]), (Join-Path $shortRoot $pair[1]))
    }
    $short = Variant 'short-source' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:SHORT|not-the-source','APP')
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('子目录:SRC|other','NESTED')
        $summary = Com-Get $db 'SummaryInformation' @(1)
        try { Com-Set $summary 'Property' @(15,1); Com-Call $summary 'Persist' }
        finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) | Out-Null }
    }
    Run-App @('--quiet',$short) 0 | Out-Null
    Check-Content 'short-source'
    New-Item -ItemType Junction -Path (Join-Path $root 'loose-junction') -Target $sourceTree | Out-Null
    $failures = @()
    $failures += @{ Path = (Variant 'loose-reparse' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:loose-junction','APP')
        Sql $db 'UPDATE `File` SET `Attributes` = ?' @(8192)
    }); Code = 5 }
    $failures += @{ Path = (Variant 'loose-missing' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:missing-source','APP')
        Sql $db 'UPDATE `File` SET `Attributes` = ?' @(8192)
    }); Code = 30 }
    $failures += @{ Path = (Variant 'loose-badhash' { param($db)
        Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @($null)
        Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('测试 App:source-layout','APP')
        Sql $db 'UPDATE `File` SET `Attributes` = ?' @(8192)
        Sql $db 'UPDATE `MsiFileHash` SET `HashPart1` = ? WHERE `File_` = ?' @(0,'F1')
    }); Code = 13 }
    $failures += @{ Path = (Variant 'external-missing' { param($db) Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('absent.cab') }); Code = 30 }
    $failures += @{ Path = (Variant 'wrong-media' { param($db) Sql $db 'UPDATE `File` SET `Attributes` = ? WHERE `File` = ?' @(8192, 'F1') }); Code = 13 }
    $failures += @{ Path = (Variant 'source-traversal' { param($db) Sql $db 'UPDATE `Directory` SET `DefaultDir` = ? WHERE `Directory` = ?' @('safe:..\outside','APP') }); Code = 5 }
    $failures += @{ Path = (Variant 'external-traversal' { param($db) Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('..\payload.cab') }); Code = 5 }
    $failures += @{ Path = (Variant 'traversal' { param($db) Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @('..\escape.txt', 'F1') }); Code = 5 }
    $failures += @{ Path = (Variant 'device' { param($db) Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @('CON.txt', 'F1') }); Code = 5 }
    $failures += @{ Path = (Variant 'ads' { param($db) Sql $db 'UPDATE `File` SET `FileName` = ? WHERE `File` = ?' @('file:stream', 'F1') }); Code = 5 }
    $failures += @{ Path = (Variant 'cycle' { param($db) Sql $db 'UPDATE `Directory` SET `Directory_Parent` = ? WHERE `Directory` = ?' @('NESTED', 'APP') }); Code = 13 }
    $failures += @{ Path = (Variant 'badsize' { param($db) Sql $db 'UPDATE `File` SET `FileSize` = ? WHERE `File` = ?' @(12345, 'F1') }); Code = 13 }
    $failures += @{ Path = (Variant 'badhash' { param($db) Sql $db 'UPDATE `MsiFileHash` SET `HashPart1` = ? WHERE `File_` = ?' @(0, 'F1') }); Code = 13 }
    $failures += @{ Path = (Variant 'missing' { param($db) Sql $db 'UPDATE `Media` SET `Cabinet` = ?' @('#absent.cab') }); Code = 13 }
    $brokenCab = Join-Path $root 'broken.cab'
    $bytes = [IO.File]::ReadAllBytes($cabinet)
    $bytes[0] = 0
    [IO.File]::WriteAllBytes($brokenCab, $bytes)
    $failures += @{ Path = (Variant 'corruptcab' { param($db) Embed-Cab $db $brokenCab -Update }); Code = 13 }
    $truncatedCab = Join-Path $root 'truncated.cab'
    $bytes = [IO.File]::ReadAllBytes($cabinet)
    [IO.File]::WriteAllBytes($truncatedCab, $bytes[0..($bytes.Length - 5)])
    $failures += @{ Path = (Variant 'truncated' { param($db) Embed-Cab $db $truncatedCab -Update }); Code = 13 }
    foreach ($failure in $failures) {
        Run-App @('--quiet',$failure.Path) $failure.Code | Out-Null
        Assert (-not (Test-Path -LiteralPath (Join-Path $root ([IO.Path]::GetFileNameWithoutExtension($failure.Path) + '_extracted')))) 'Failure committed an output directory.'
    }
    Assert (@(Get-ChildItem -LiteralPath $root -Directory -Filter '.extract-*.tmp').Count -eq 0) 'Failure left temporary output.'
    Run-App @('--quiet',$package,$failures[0].Path) 299 | Out-Null
    Run-App @('--quiet','--output') 160 | Out-Null
    Run-App @('--quiet','--list',$package,$package) 160 | Out-Null
    Run-App @('--quiet','--output','',$package) 160 | Out-Null
    Run-App @('--quiet','--output',($unicodeRoot + '\'),$package) 0 | Out-Null
    Run-App @('--open-notification','hunlongyu-extract://job/../../outside') 5 | Out-Null
    Run-App @('--open-notification','hunlongyu-extract://job/00000000-0000-0000-0000-000000000000?path=C:') 5 | Out-Null

    $junction = Join-Path $root 'junction'
    New-Item -ItemType Junction -Path $junction -Target $unicodeRoot | Out-Null
    Run-App @('--quiet','--output',$junction,$package) 5 | Out-Null
    Run-App @('--quiet',(Join-Path $junction '测试安装包.msi')) 5 | Out-Null
    Assert ((Get-FileHash -LiteralPath $unicodePackage).Hash -eq $originalHash) 'Reparse test changed its target.'

    Write-Output "PASS: MSI embedded/external/multiple CABs, loose/mixed/short-name sources, MSZIP/LZX, listing, hashes, Unicode, collisions, repeat extraction, $($failures.Count) invalid packages, batch, reparse rejection and CLI boundaries. Fixtures: $root"
} finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null }
