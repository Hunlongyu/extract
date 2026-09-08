# Run manually in Windows PowerShell 5.1 on a logged-in Windows desktop.
# Generates inert ZIP data and runs only Extract. Never executes extracted files.
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$WorkRoot,
    [ValidateSet('normal','dismiss','quiet','short','failure','batch','partial','cancel')][string]$Mode = 'normal'
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
$blocks = if ($Mode -in @('normal','dismiss','cancel')) { 1024 } else { 1 }
try { for ($i=0; $i -lt $blocks; ++$i) { $entry.Write($chunk, 0, $chunk.Length) } }
finally { $entry.Dispose(); $archive.Dispose(); $stream.Dispose() }
if ($Mode -eq 'failure') { [IO.File]::WriteAllText($zip, 'not an archive') }
$arguments = '"' + $zip + '"'
if ($Mode -eq 'quiet') { $arguments = '--quiet ' + $arguments }
if ($Mode -in @('batch','partial')) {
    $second = Join-Path $root 'second.zip'
    Copy-Item -LiteralPath $zip -Destination $second
    if ($Mode -eq 'partial') { [IO.File]::WriteAllText($second, 'not an archive') }
    $arguments += ' "' + $second + '"'
}
$before = @($history.GetHistory('Hunlongyu.Extract') | ForEach-Object { $_.Group })
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
$snapshots = @()
$group = $null
$removed = $false
$cancelRequested = $false
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
        if ($Mode -eq 'cancel' -and -not $cancelRequested -and $xml.Contains('{fraction}')) {
            [xml]$live = $xml
            $cancelAction = $live.SelectSingleNode('/toast/actions/action')
            $cancelLabel = '"\u53d6\u6d88\u4efb\u52a1"' | ConvertFrom-Json
            if (-not $cancelAction -or $cancelAction.GetAttribute('content') -ne $cancelLabel -or
                $cancelAction.GetAttribute('activationType') -ne 'protocol' -or
                $cancelAction.GetAttribute('arguments') -ne "hunlongyu-extract://cancel/$group") { throw 'Missing cancel action' }
            $cancelProcess = Start-Process -FilePath $exe -ArgumentList "--open-notification hunlongyu-extract://cancel/$group" -WindowStyle Hidden -PassThru -Wait
            if ($cancelProcess.ExitCode -ne 0) { throw 'Cancel protocol activation failed' }
            $cancelRequested = $true
        }
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
        foreach ($line in Get-Content -LiteralPath $file.FullName -Encoding UTF8 -ErrorAction SilentlyContinue) {
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
$expectedExit = if ($Mode -eq 'failure') { 50 } elseif ($Mode -eq 'partial') { 299 } elseif ($Mode -eq 'cancel') { 1223 } else { 0 }
if ($process.ExitCode -ne $expectedExit) { throw "Unexpected exit code: $($process.ExitCode)" }
if ($Mode -eq 'quiet') {
    if ($final.Count -or $updates.Count -or $resultMode.Count) { throw 'Quiet mode sent notifications' }
} else {
    if ($final.Count -ne 1) { throw 'Expected exactly one final notification' }
    $finalXml = $final[0].Content.GetXml()
    [xml]$document = $finalXml
    $action = $document.SelectSingleNode('/toast/actions/action')
    $openFolder = '"\u6253\u5f00\u6587\u4ef6\u5939"' | ConvertFrom-Json
    $viewResult = '"\u67e5\u770b\u7ed3\u679c"' | ConvertFrom-Json
    $expectedAction = if ($Mode -in @('failure','partial','batch','cancel')) { $viewResult } else { $openFolder }
    if (-not $action -or $action.GetAttribute('content') -ne $expectedAction -or
        $action.GetAttribute('activationType') -ne 'protocol' -or
        $action.GetAttribute('arguments') -ne $document.DocumentElement.GetAttribute('launch')) {
        throw 'Result action is missing or does not open the existing job target'
    }
    $bar = $document.SelectSingleNode('/toast/visual/binding/progress')
    if ($bar -and ($bar.HasAttribute('title') -or $bar.GetAttribute('status'))) { throw 'Completed progress repeats status text' }
    if ($document.SelectSingleNode('/toast/visual/binding/text').InnerText.StartsWith('Extract')) { throw 'Result repeats app identity' }
    if (-not $finalXml.Contains($name)) { throw 'Final result lacks package name' }
    if ($Mode -in @('failure','partial','cancel')) {
        if ($finalXml.Contains('<progress')) { throw 'Failure shows a completed progress bar' }
    } elseif (-not $finalXml.Contains('value="1"') -or -not $finalXml.Contains('100%')) { throw 'Success did not retain a full progress bar' }
    if ($Mode -in @('batch','partial') -and -not $finalXml.Contains('second.zip')) { throw 'Batch result lacks filenames' }
    if ($Mode -eq 'cancel' -and -not $cancelRequested) { throw 'No cancel activation tested' }
    if ($Mode -in @('normal','dismiss','cancel')) {
        if ($resultMode[-1].message -ne 'updated-existing') { throw 'Final result would produce another popup' }
        if (-not @($snapshots | Where-Object { $_.Xml.Contains('<progress') }).Count) { throw 'No live progress observed' }
    }
    if ($Mode -eq 'normal') {
        $accepted = @($updates | Where-Object { $_.message -match 'result=0;' })
        if ($accepted.Count -lt 2 -or $accepted.Count -ne $updates.Count) { throw 'Progress updates were not accepted' }
        $previousFraction = 0.0
        $fileUpdates = 0
        foreach ($snapshot in $snapshots) {
            if (-not $snapshot.Fields.ContainsKey('fraction') -or $snapshot.Fields['fraction'] -eq 'indeterminate') { continue }
            $fraction = [double]::Parse($snapshot.Fields['fraction'], [Globalization.CultureInfo]::InvariantCulture)
            if ($fraction -lt $previousFraction -or $fraction -ge 1.0) { throw 'Live file progress regressed or claimed final completion' }
            $previousFraction = $fraction
            ++$fileUpdates
        }
        if (-not $fileUpdates) { throw 'No cumulative file progress observed' }
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
