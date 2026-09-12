param([switch]$Adaptive)
$ErrorActionPreference = 'Stop'
$workspace = 'C:\Users\sayal\OneDrive\Desktop\research'
$recordDirectory = if ($Adaptive) { 'adaptive_competitors_20260906' } else { 'current_competitors_20260906' }
$runner = if ($Adaptive) { 'run_adaptive_ledger.sh' } else { 'run_ledger_revision.sh' }
$logBase = Join-Path $workspace "results_q1\$recordDirectory\ledger_session"
foreach ($suffix in @('.stdout.txt', '.stderr.txt', '.host.csv')) {
    if (Test-Path -LiteralPath ($logBase + $suffix)) { throw "Existing ledger log: $logBase$suffix" }
}
try {
    & wsl -d kali-linux -u root -- systemctl stop docker.service docker.socket containerd.service cron.service
    if ($LASTEXITCODE -ne 0) { throw 'Could not pause guest background services' }
    $ledgerProcess = Start-Process -FilePath 'wsl.exe' -WindowStyle Hidden -PassThru `
        -ArgumentList @('-d','kali-linux','--','bash',"/mnt/c/Users/sayal/OneDrive/Desktop/research/scripts/$runner") `
        -RedirectStandardOutput ($logBase + '.stdout.txt') -RedirectStandardError ($logBase + '.stderr.txt')
    $ledgerHandle = $ledgerProcess.Handle
    do {
        $memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
        [pscustomobject]@{
            Utc = [DateTime]::UtcNow.ToString('o')
            AvailableMiB = $memory.AvailableMBytes
            PagesInputPerSecond = $memory.PagesInputPersec
            PagesOutputPerSecond = $memory.PagesOutputPersec
            PercentCommitted = $memory.PercentCommittedBytesInUse
        } | Export-Csv -LiteralPath ($logBase + '.host.csv') -NoTypeInformation -Encoding utf8 -Append
    } while (-not $ledgerProcess.WaitForExit(5000))
    $ledgerProcess.WaitForExit()
    if ($ledgerProcess.ExitCode -ne 0) { throw "Ledger session exited $($ledgerProcess.ExitCode); records retained" }
    Write-Output 'All planned ledger processes completed.'
} finally {
    & wsl --shutdown
}
