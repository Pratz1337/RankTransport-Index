$ErrorActionPreference = 'Stop'
$workspace = 'C:\Users\sayal\OneDrive\Desktop\research'
$logBase = Join-Path $workspace 'results_q1\current_competitors_20260906\session'
foreach ($suffix in @('.stdout.txt', '.stderr.txt', '.host.csv')) {
    if (Test-Path -LiteralPath ($logBase + $suffix)) { throw "Existing session log: $logBase$suffix" }
}
try {
    & wsl -d kali-linux -u root -- systemctl stop docker.service docker.socket containerd.service cron.service
    if ($LASTEXITCODE -ne 0) { throw 'Could not pause guest background services' }
    $benchmarkProcess = Start-Process -FilePath 'wsl.exe' -WindowStyle Hidden -PassThru `
        -ArgumentList @('-d','kali-linux','--','bash','/mnt/c/Users/sayal/OneDrive/Desktop/research/scripts/run_current_competitors.sh') `
        -RedirectStandardOutput ($logBase + '.stdout.txt') -RedirectStandardError ($logBase + '.stderr.txt')
    $benchmarkHandle = $benchmarkProcess.Handle
    Write-Output "Current specialist session PID $($benchmarkProcess.Id)"
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
    if ($benchmarkProcess.ExitCode -ne 0) { throw "Session exited $($benchmarkProcess.ExitCode); records retained" }
    Write-Output 'All planned specialist processes completed.'
} finally {
    & wsl --shutdown
}
