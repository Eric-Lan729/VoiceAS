param(
    [string]$Executable = "$PSScriptRoot\..\Release\VoiceAS.exe",
    [int]$Seconds = 30,
    [int]$ExistingProcessId = 0
)

$ErrorActionPreference = 'Stop'
if ($Seconds -lt 5 -or $Seconds -gt 50) { throw 'Use a sample duration of 5 to 50 seconds.' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$configPath = Join-Path $PSScriptRoot "perf-priority-$PID.txt"
$process = $null
$ownsProcess = $false
try {
    if ($ExistingProcessId -gt 0) {
        $process = Get-Process -Id $ExistingProcessId
        if ($process.ProcessName -ne 'VoiceAS') { throw 'Existing process must be VoiceAS.' }
        $Executable = $process.Path
        $mode = 'Read-only sampling of existing listener; existing process is left running'
    }
    else {
    # A temporary priority list; preview mode never modifies default audio routing.
    [System.IO.File]::WriteAllText($configPath, @"
VoiceAS priority v1
communications=0
device={0.0.0.00000000}.{415b1fc3-bce0-4287-b3f9-a4e6e66dad3f}
device={0.0.0.00000000}.{5cb033c0-de9a-4524-86c4-46a54a2dcaed}
"@, [System.Text.UTF8Encoding]::new($false))
    $info = [System.Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    foreach ($argument in @('--monitor', '--foreground', '--dry-run', '--config', $configPath)) { $info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $ownsProcess = $true
    $mode = 'Preview; full enumeration and real HID queries; no routing changes'
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw ("Monitor exited: " + $stderr.GetAwaiter().GetResult()) }
    }

    $process.Refresh()
    $initialCpuMs = $process.TotalProcessorTime.TotalMilliseconds
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $samples = @()
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 1000
        $process.Refresh()
        if ($process.HasExited) { throw 'Monitor stopped during sampling.' }
        $samples += [pscustomobject]@{
            WorkingSetMiB = $process.WorkingSet64 / 1MB
            PrivateBytesMiB = $process.PrivateMemorySize64 / 1MB
            Handles = $process.HandleCount
            Threads = $process.Threads.Count
        }
    }
    $elapsedMs = $timer.Elapsed.TotalMilliseconds
    $cpuMs = $process.TotalProcessorTime.TotalMilliseconds - $initialCpuMs
    $logicalCpus = [Environment]::ProcessorCount
    $cpuPercentOneCore = 100 * $cpuMs / $elapsedMs
    $workingSet = $samples | Measure-Object WorkingSetMiB -Minimum -Maximum -Average
    $privateBytes = $samples | Measure-Object PrivateBytesMiB -Minimum -Maximum -Average
    $handles = $samples | Measure-Object Handles -Minimum -Maximum
    $threads = $samples | Measure-Object Threads -Minimum -Maximum

    $log = ''
    $errors = ''
    if ($ownsProcess) {
        $process.Kill()
        $process.WaitForExit()
        $log = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
    }
    [pscustomobject]@{
        Executable = $Executable
        Mode = $mode
        SampleSeconds = [Math]::Round($elapsedMs / 1000, 2)
        LogicalProcessors = $logicalCpus
        CpuTimeMs = [Math]::Round($cpuMs, 2)
        CpuPercentOneLogicalCore = [Math]::Round($cpuPercentOneCore, 4)
        CpuPercentTotalMachine = [Math]::Round($cpuPercentOneCore / $logicalCpus, 4)
        WorkingSetMiB = @{ Min = [Math]::Round($workingSet.Minimum, 2); Max = [Math]::Round($workingSet.Maximum, 2); Average = [Math]::Round($workingSet.Average, 2) }
        PrivateBytesMiB = @{ Min = [Math]::Round($privateBytes.Minimum, 2); Max = [Math]::Round($privateBytes.Maximum, 2); Average = [Math]::Round($privateBytes.Average, 2) }
        Handles = @{ Min = $handles.Minimum; Max = $handles.Maximum }
        Threads = @{ Min = $threads.Minimum; Max = $threads.Maximum }
        MonitorLog = $log
        MonitorErrors = $errors
    } | ConvertTo-Json -Depth 4
}
finally {
    if ($ownsProcess -and $process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    if ($ownsProcess -and (Test-Path -LiteralPath $configPath)) { Remove-Item -LiteralPath $configPath }
}
