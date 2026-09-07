# Run manually in Windows PowerShell 5.1 on a logged-in Windows desktop.
# Generates inert ZIP data and runs only Extract. Never executes extracted files.
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$WorkRoot,
    [ValidateSet('normal','dismiss','quiet','short','failure','batch')][string]$Mode = 'normal'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$null = [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
$history = [Windows.UI.Notifications.ToastNotificationManager]::History
$exe = (Resolve-Path -LiteralPath $Executable).Path
$root = Join-Path ([IO.Path]::GetFullPath($WorkRoot)) ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$name = 'progress-' + [IO.Path]::GetFileName($root) + '.zip'
$zip = Join-Path $root $name
$stream = [IO.File]::Create($zip)
$archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
$entry = $archive.CreateEntry('payload.bin', [IO.Compression.CompressionLevel]::Optimal).Open()
$chunk = New-Object byte[] (1024 * 1024)
$blocks = if ($Mode -in @('normal','dismiss')) { 1024 } else { 1 }
try { for ($i=0; $i -lt $blocks; ++$i) { $entry.Write($chunk, 0, $chunk.Length) } }
finally { $entry.Dispose(); $archive.Dispose(); $stream.Dispose() }
if ($Mode -eq 'failure') { [IO.File]::WriteAllText($zip, 'not an archive') }
$arguments = '"' + $zip + '"'
if ($Mode -eq 'quiet') { $arguments = '--quiet ' + $arguments }
if ($Mode -eq 'batch') {
    $second = Join-Path $root 'second.zip'
    Copy-Item -LiteralPath $zip -Destination $second
    $arguments += ' "' + $second + '"'
}
$before = @($history.GetHistory('Hunlongyu.Extract') | ForEach-Object { $_.Group })
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
$snapshots = @()
$group = $null
$removed = $false
$watch = [Diagnostics.Stopwatch]::StartNew()
while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt 120) {
    foreach ($toast in @($history.GetHistory('Hunlongyu.Extract'))) {
        if ($toast.Tag -ne 'progress' -or $toast.Group -in $before) { continue }
        $xml = $toast.Content.GetXml()
        # PowerShell enumerates the WinRT map into KeyValuePair objects.
        $fields = @{}
        if ($toast.Data) { foreach ($pair in $toast.Data.Values) { $fields[$pair.Key] = $pair.Value } }
        if (-not $group -and ($xml.Contains($name) -or ([string]$fields['context']).Contains($name))) { $group = $toast.Group }
        if (-not $group -or $toast.Group -ne $group) { continue }
        $snapshots += [pscustomobject]@{ Group=$group; Xml=$xml; Fields=$fields; Milliseconds=$watch.ElapsedMilliseconds }
        if ($Mode -eq 'dismiss' -and -not $removed -and $xml.Contains('<progress')) {
            $history.Remove('progress', $group, 'Hunlongyu.Extract')
            $removed = $true
        }
    }
    Start-Sleep -Milliseconds 100
    $process.Refresh()
}
if (-not $process.HasExited) { throw 'Extraction timed out; inspect the process before rerunning' }
$process.WaitForExit()
Start-Sleep -Milliseconds 300
$logs = @()
foreach ($directory in @((Join-Path (Split-Path $exe) 'log'), (Join-Path $env:LOCALAPPDATA 'Extract/log'))) {
    if (-not (Test-Path -LiteralPath $directory)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $directory -Filter '*.log' -File) {
        foreach ($line in Get-Content -LiteralPath $file.FullName -Encoding UTF8) {
            try { $record = $line | ConvertFrom-Json } catch { continue }
            if ($record.pid -eq $process.Id) { $logs += $record }
        }
    }
}
$jobEvent = $logs | Where-Object event -eq 'job.created' | Select-Object -First 1
if (-not $jobEvent -or $jobEvent.message -notmatch 'job=([A-Fa-f0-9-]{36})') { throw 'Job record not found' }
$group = $Matches[1]
$final = @($history.GetHistory('Hunlongyu.Extract') | Where-Object { $_.Tag -eq 'progress' -and $_.Group -eq $group })
$updates = @($logs | Where-Object event -eq 'notification.progress_update')
$resultMode = @($logs | Where-Object event -eq 'notification.result_mode')
$expectedExit = if ($Mode -eq 'failure') { 50 } else { 0 }
if ($process.ExitCode -ne $expectedExit) { throw "Unexpected exit code: $($process.ExitCode)" }
if ($Mode -eq 'quiet') {
    if ($final.Count -or $updates.Count -or $resultMode.Count) { throw 'Quiet mode sent notifications' }
} else {
    if ($final.Count -ne 1) { throw 'Expected exactly one final notification' }
    $finalXml = $final[0].Content.GetXml()
    if ($finalXml.Contains('<progress') -or -not $finalXml.Contains($name)) { throw 'Final result lacks package name or still has a progress bar' }
    if ($Mode -eq 'batch' -and -not $finalXml.Contains('second.zip')) { throw 'Batch result lacks filenames' }
    if ($Mode -in @('normal','dismiss')) {
        if ($resultMode[-1].message -ne 'updated-existing') { throw 'Final result would produce another popup' }
        if (-not @($snapshots | Where-Object { $_.Xml.Contains('<progress') }).Count) { throw 'No live progress observed' }
    }
    if ($Mode -eq 'normal') {
        $accepted = @($updates | Where-Object { $_.message -match 'result=0;' })
        if ($accepted.Count -lt 2 -or $accepted.Count -ne $updates.Count) { throw 'Progress updates were not accepted' }
    }
    if ($Mode -eq 'dismiss') {
        if (-not $removed -or -not @($logs | Where-Object event -eq 'notification.progress_stopped').Count) { throw 'Dismissal did not stop updates' }
        $starts = @($logs | Where-Object event -eq 'notification.progress_started')
        if ($starts.Count -ne 1) { throw 'Dismissed progress was shown again' }
    }
}
[pscustomobject]@{Mode=$Mode;ExitCode=$process.ExitCode;Job=$group;Snapshots=$snapshots;Updates=$updates;ResultMode=$resultMode;FinalXml=@($final | ForEach-Object { $_.Content.GetXml() })} |
    ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $root 'notification-validation.json') -Encoding UTF8
Write-Output "PASS: $Mode; evidence: $root"
