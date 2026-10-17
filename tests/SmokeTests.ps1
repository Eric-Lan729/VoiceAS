param([string]$Executable = "$PSScriptRoot\..\x64\Debug\VoiceAS.exe")

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testDirectory = Join-Path $PSScriptRoot "smoke-output-$PID"
$configPath = Join-Path $testDirectory 'priority order.txt'
$monitor = $null
$duplicate = $null
$backgroundStarted = $false
$backgroundProcess = $null
$checks = 0

function Assert-True([bool]$Condition, [string]$Description) {
    if (-not $Condition) { throw $Description }
    $script:checks++
}

function Start-App([string[]]$AppArguments, [string]$InputText = '') {
    $info = [System.Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    foreach ($argument in $AppArguments) { $info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.Write($InputText)
    $process.StandardInput.Close()
    return @{ Process = $process; Stdout = $stdout; Stderr = $stderr }
}

function Complete-App($Invocation) {
    if (-not $Invocation.Process.WaitForExit(10000)) { throw 'Application did not exit in time.' }
    return @{ Code = $Invocation.Process.ExitCode; Out = $Invocation.Stdout.GetAwaiter().GetResult(); Err = $Invocation.Stderr.GetAwaiter().GetResult() }
}

try {
    New-Item -ItemType Directory -Path $testDirectory | Out-Null
    $list = Complete-App (Start-App @('--list'))
    Assert-True ($list.Code -eq 0) 'Device enumeration failed.'
    $active = @([regex]::Matches($list.Out, '(?m)^\[\d+\] (?<name>[^\r\n]+)\r?\n\s+ID: (?<id>[^\r\n]+)\r?\n\s+State: Active\r?$'))
    Assert-True ($active.Count -gt 0) 'Smoke test needs an active output device.'
    $order = @(1..$active.Count)
    [array]::Reverse($order)
    $inputText = "0`n$($order -join ',')`nn`n"
    $configured = Complete-App (Start-App @('--configure', '--config', $configPath) $inputText)
    Assert-True ($configured.Code -eq 0) "Configuration failed: $($configured.Err)"
    Assert-True (-not $configured.Out.Contains('State: Not present')) 'Configuration must not show inactive devices.'
    foreach ($match in [regex]::Matches($list.Out, '(?m)^\[\d+\] [^\r\n]+\r?\n\s+ID: (?<id>[^\r\n]+)\r?\n\s+State: (?!Active)[^\r\n]+')) {
        Assert-True (-not $configured.Out.Contains($match.Groups['id'].Value)) 'An inactive endpoint was shown in configuration.'
    }
    $saved = [System.IO.File]::ReadAllText($configPath)
    $expectedIds = @($order | ForEach-Object { 'device=' + $active[$_ - 1].Groups['id'].Value })
    $actualIds = @($saved -split "`n" | Where-Object { $_.StartsWith('device=') })
    Assert-True (($actualIds -join '|') -eq ($expectedIds -join '|')) 'Saved IDs did not follow requested order.'
    Assert-True ($saved.Contains('communications=0')) 'Default configuration must preserve independent communication routing.'
    $cancelled = Complete-App (Start-App @('--configure', '--config', $configPath) "`n")
    Assert-True ($cancelled.Code -eq 0 -and [System.IO.File]::ReadAllText($configPath) -eq $saved) 'Cancel must preserve saved order.'

    $preview = Complete-App (Start-App @('--monitor', '--dry-run', '--once', '--config', $configPath))
    Assert-True ($preview.Code -eq 0 -and $preview.Out.Contains($active[$order[0] - 1].Groups['name'].Value)) 'Monitor must select the first configured Active endpoint.'
    $missing = Complete-App (Start-App @('--monitor', '--config', (Join-Path $testDirectory 'missing.txt')))
    Assert-True ($missing.Code -eq 1 -and $missing.Err.Contains('--configure')) 'Missing configuration must explain how to configure.'
    $invalid = Complete-App (Start-App @('--once'))
    Assert-True ($invalid.Code -eq 1) 'Invalid command combinations must fail.'
    $menu = Complete-App (Start-App @('--config', $configPath) "0`n")
    Assert-True ($menu.Code -eq 0 -and $menu.Out.Contains('1.') -and $menu.Out.Contains('2.')) 'Both menu entries must be available.'

    $monitor = Start-App @('--monitor', '--foreground', '--dry-run', '--config', $configPath)
    Start-Sleep -Milliseconds 500
    Assert-True (-not $monitor.Process.HasExited) 'Listener must keep running.'
    $duplicate = Start-App @('--monitor', '--dry-run', '--once', '--config', $configPath)
    $duplicateResult = Complete-App $duplicate
    Assert-True ($duplicateResult.Code -eq 1 -and $duplicateResult.Err.Contains('already running')) 'Duplicate monitor must be rejected.'

    # Reconfigure while the listener runs: its next check must reload the new order.
    $updated = Complete-App (Start-App @('--configure', '--config', $configPath) "1`ny`n")
    Assert-True ($updated.Code -eq 0) 'Reconfiguration while listening failed.'
    Start-Sleep -Milliseconds 2500
    $validContent = [System.IO.File]::ReadAllText($configPath)
    [System.IO.File]::WriteAllText($configPath, 'broken configuration')
    Start-Sleep -Milliseconds 2500
    [System.IO.File]::WriteAllText($configPath, $validContent)
    Start-Sleep -Milliseconds 2500
    Assert-True (-not $monitor.Process.HasExited) 'Listener must survive a malformed configuration and recover.'
    $stopForeground = Complete-App (Start-App @('--stop'))
    Assert-True ($stopForeground.Code -eq 0) 'Remote stop must shut down the foreground listener cleanly.'
    $stopped = Complete-App $monitor
    Assert-True ($stopped.Code -eq 0) 'Listener must release resources and exit successfully on stop.'
    Assert-True ($stopped.Out.Contains('Loaded updated priority order')) 'Listener did not reload the configuration.'
    Assert-True ($stopped.Err.Contains('keeping the last valid configuration')) 'Malformed configuration must retain the last valid order.'
    Assert-True ($stopped.Out.Contains('Configuration reading recovered')) 'Listener did not recover after configuration was repaired.'

    $help = Complete-App (Start-App @('--help'))
    Assert-True ($help.Code -eq 0 -and $help.Out.Contains('Start monitoring in the background')) 'Help must describe background mode in English.'
    Assert-True ($help.Out -notmatch '\p{IsCJKUnifiedIdeographs}') 'Built-in help must be in English.'
    $background = Complete-App (Start-App @('--monitor', '--dry-run', '--config', $configPath))
    Assert-True ($background.Code -eq 0 -and $background.Out.Contains('Background monitor started')) "Background launch failed: $($background.Err)"
    $backgroundStarted = $true
    $workerPid = [int][regex]::Match($background.Out, 'PID (\d+)').Groups[1].Value
    $backgroundProcess = Get-Process -Id $workerPid
    Assert-True (-not $backgroundProcess.HasExited) 'Worker must outlive the launcher.'
    $running = Complete-App (Start-App @('--status'))
    Assert-True ($running.Code -eq 0 -and $running.Out.Contains('is running')) 'Status must report the detached worker.'
    $logPath = Join-Path $testDirectory 'monitor.log'
    Assert-True (Test-Path -LiteralPath $logPath) 'Background output must be saved to a log.'
    $logStream = [System.IO.File]::Open($logPath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $logReader = [System.IO.StreamReader]::new($logStream, [System.Text.Encoding]::UTF8)
    try { $logContent = $logReader.ReadToEnd() }
    finally { $logReader.Dispose() }
    Assert-True ($logContent.Contains('Background mode: no console window.')) 'The worker must have no console window.'
    Assert-True ($logContent.Contains('Monitor started')) 'Worker startup must be logged before the launcher reports success.'
    $secondBackground = Complete-App (Start-App @('--monitor', '--dry-run', '--config', $configPath))
    Assert-True ($secondBackground.Code -eq 1 -and $secondBackground.Err.Contains('already running')) 'Starting twice must not create a second worker.'
    $stopBackground = Complete-App (Start-App @('--stop'))
    Assert-True ($stopBackground.Code -eq 0) 'Background worker must support graceful remote stop.'
    Assert-True ($backgroundProcess.WaitForExit(10000)) 'Background worker must exit after stop.'
    $backgroundStarted = $false
    $notRunning = Complete-App (Start-App @('--status'))
    Assert-True ($notRunning.Code -eq 1) 'Status must report that monitoring has stopped.'
    $stopAgain = Complete-App (Start-App @('--stop'))
    Assert-True ($stopAgain.Code -eq 0 -and $stopAgain.Out.Contains('No audio monitor')) 'Stopping twice must be safe.'
    $invalidWorker = Complete-App (Start-App @('--monitor-worker'))
    Assert-True ($invalidWorker.Code -eq 1) 'Internal worker mode requires a startup handshake.'
    Write-Output "PASS: $checks CLI and listener checks."
}
finally {
    if ($backgroundStarted) { $null = Complete-App (Start-App @('--stop')) }
    foreach ($invocation in @($monitor, $duplicate)) {
        if ($invocation -and -not $invocation.Process.HasExited) { $invocation.Process.Kill(); $invocation.Process.WaitForExit() }
    }
    # Remove only the known test file and its now-empty directory; never recurse.
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath }
    $logPath = Join-Path $testDirectory 'monitor.log'
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
    if (Test-Path -LiteralPath $testDirectory) { Remove-Item -LiteralPath $testDirectory }
}
