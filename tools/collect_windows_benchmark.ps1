[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ProcessId,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1, 60)][int]$IntervalSeconds = 5,
    [ValidateRange(1, 43200)][int]$DurationSeconds = 1800,
    [ValidateRange(0, 32)][int]$GpuIndex = 0
)

$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$encoding = [Text.UTF8Encoding]::new($false)
$target = Get-Process -Id $ProcessId
$processStarted = $target.StartTime.ToUniversalTime()
$gpuCommand = (Get-Command nvidia-smi.exe -ErrorAction Stop).Source
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
foreach ($name in @('telemetry.csv', 'threads.csv', 'collector.json')) {
    if ([IO.File]::Exists([IO.Path]::Combine($output, $name))) {
        throw "Capture file already exists: $name"
    }
}

$telemetry = $null
$threads = $null
$stopReason = 'duration_reached'
$failure = $null
$samples = 0
$startedUtc = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()
$previousAt = 0.0
$previousCpu = $target.TotalProcessorTime.TotalMilliseconds
$previousThreads = @{}
foreach ($thread in $target.Threads) {
    try { $previousThreads[$thread.Id] = $thread.TotalProcessorTime.TotalMilliseconds }
    catch {}
}
$processors = [Environment]::ProcessorCount

function Number([double]$value) {
    return $value.ToString('0.###', $culture)
}

try {
    $telemetry = [IO.StreamWriter]::new([IO.Path]::Combine($output, 'telemetry.csv'), $false, $encoding)
    $threads = [IO.StreamWriter]::new([IO.Path]::Combine($output, 'threads.csv'), $false, $encoding)
    $telemetry.WriteLine('utc,pid,cpuPercent,workingSetMiB,privateMiB,systemAvailableMiB,gpuPercent,gpuMemoryPercent,gpuMiB,gpuTotalMiB,gpuWatts,gpuCelsius')
    $threads.WriteLine('utc,thread,cpuCorePercent')
    while ($clock.Elapsed.TotalSeconds -lt $DurationSeconds) {
        $remaining = $DurationSeconds - $clock.Elapsed.TotalSeconds
        Start-Sleep -Milliseconds ([int]([Math]::Min($IntervalSeconds, $remaining) * 1000))
        $target.Refresh()
        if ($target.HasExited) { $stopReason = 'process_exited'; break }
        if ($target.StartTime.ToUniversalTime() -ne $processStarted) { throw 'Process identity changed' }
        $utc = [DateTime]::UtcNow.ToString('o', $culture)
        $at = $clock.Elapsed.TotalMilliseconds
        $elapsed = $at - $previousAt
        $cpu = $target.TotalProcessorTime.TotalMilliseconds
        $cpuPercent = ($cpu - $previousCpu) * 100 / ($elapsed * $processors)
        if ($cpuPercent -lt 0 -or $cpuPercent -gt 100.1) { throw 'Invalid process CPU delta' }
        $currentThreads = @{}
        foreach ($thread in $target.Threads) {
            try {
                $threadCpu = $thread.TotalProcessorTime.TotalMilliseconds
                $currentThreads[$thread.Id] = $threadCpu
                $prior = if ($previousThreads.ContainsKey($thread.Id)) { $previousThreads[$thread.Id] } else { 0.0 }
                if ($threadCpu -ge $prior) {
                    $corePercent = [Math]::Min(100.0, ($threadCpu - $prior) * 100 / $elapsed)
                    $threads.WriteLine("$utc,$($thread.Id),$(Number $corePercent)")
                }
            } catch {
                Write-Verbose 'Thread exited during sampling'
            }
        }
        $gpuLines = @(& $gpuCommand "--id=$GpuIndex" '--query-gpu=utilization.gpu,utilization.memory,memory.used,memory.total,power.draw,temperature.gpu' '--format=csv,noheader,nounits')
        if ($LASTEXITCODE -ne 0 -or $gpuLines.Count -ne 1) { throw 'NVIDIA telemetry query failed' }
        $gpu = $gpuLines[0].Split(',')
        if ($gpu.Count -ne 6) { throw 'Unexpected NVIDIA telemetry columns' }
        $gpuNumbers = @($gpu | ForEach-Object { [double]::Parse($_.Trim(), $culture) })
        $available = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024
        $values = @(
            $utc, $ProcessId, (Number ([Math]::Min(100.0, $cpuPercent))),
            (Number ($target.WorkingSet64 / 1MB)), (Number ($target.PrivateMemorySize64 / 1MB)),
            (Number $available)
        ) + @($gpuNumbers | ForEach-Object { Number $_ })
        $telemetry.WriteLine(($values -join ','))
        $telemetry.Flush()
        $threads.Flush()
        $samples++
        $previousCpu = $cpu
        $previousAt = $at
        $previousThreads = $currentThreads
    }
} catch {
    $stopReason = 'collector_failed'
    $failure = $_.Exception.Message
    throw
} finally {
    if ($null -ne $telemetry) { $telemetry.Dispose() }
    if ($null -ne $threads) { $threads.Dispose() }
    $summary = [ordered]@{
        processId = $ProcessId
        processStartedUtc = $processStarted.ToString('o', $culture)
        startedUtc = $startedUtc.ToString('o', $culture)
        finishedUtc = [DateTime]::UtcNow.ToString('o', $culture)
        samples = $samples
        intervalSeconds = $IntervalSeconds
        durationLimitSeconds = $DurationSeconds
        gpuIndex = $GpuIndex
        gpuMetricsScope = 'device_wide'
        cpuPercentScope = 'process_normalized_to_logical_processors'
        threadPercentScope = 'one_logical_processor'
        stopReason = $stopReason
        failure = $failure
    }
    [IO.File]::WriteAllText([IO.Path]::Combine($output, 'collector.json'), ($summary | ConvertTo-Json), $encoding)
    $target.Dispose()
}
