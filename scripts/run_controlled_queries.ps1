param([int]$FirstTrial = 1, [int]$LastTrial = 1)
$ErrorActionPreference = 'Stop'
$workspace = 'C:\Users\sayal\OneDrive\Desktop\research'
$logBase = Join-Path $workspace "results_q1\reassessment_20260905\controlled_${FirstTrial}_to_${LastTrial}"
foreach ($suffix in @('.stdout.txt', '.stderr.txt', '.host.csv')) {
    if (Test-Path -LiteralPath ($logBase + $suffix)) { throw "Existing session log: $logBase$suffix" }
}
$benchmarkProcess = $null
try {
    & wsl -d kali-linux -u root -- bash -lc 'systemctl stop docker.service docker.socket containerd.service cron.service'
    if ($LASTEXITCODE -ne 0) { throw 'Could not pause guest background services' }
    $benchmarkProcess = Start-Process -FilePath 'wsl.exe' -WindowStyle Hidden -PassThru `
        -ArgumentList @('-d','kali-linux','--','bash','/mnt/c/Users/sayal/OneDrive/Desktop/research/scripts/run_controlled_queries.sh',"$FirstTrial","$LastTrial") `
        -RedirectStandardOutput ($logBase + '.stdout.txt') -RedirectStandardError ($logBase + '.stderr.txt')
    $processHandle = $benchmarkProcess.Handle
    Write-Output "Started controlled trials $FirstTrial through $LastTrial; WSL launcher PID $($benchmarkProcess.Id)"
    do {
        $memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
        [pscustomobject]@{
            Utc = [DateTime]::UtcNow.ToString('o')
            AvailableMiB = $memory.AvailableMBytes
            PagesInputPerSecond = $memory.PagesInputPersec
            PagesOutputPerSecond = $memory.PagesOutputPersec
            PercentCommitted = $memory.PercentCommittedBytesInUse
        } | Export-Csv -LiteralPath ($logBase + '.host.csv') -NoTypeInformation -Encoding utf8 -Append
    } while (-not $benchmarkProcess.WaitForExit(5000))
    $benchmarkProcess.WaitForExit()
    $benchmarkExit = $benchmarkProcess.ExitCode
    Get-Content -LiteralPath ($logBase + '.stdout.txt')
    Get-Content -LiteralPath ($logBase + '.stderr.txt')
    if ($benchmarkExit -ne 0) { throw "Controlled run exited $benchmarkExit; retain logs for diagnosis" }
} finally {
    & wsl --shutdown
}
