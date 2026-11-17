# Watch the FM-1 on USB-MIDI: every -Every seconds, ask it who it is (fm1_install.py --info) and log the answer.
#   .\tools\watch_fm1.ps1                 every 30 s until Ctrl+C; the log: build\watch_fm1.log
#   .\tools\watch_fm1.ps1 -Every 10       more often
# A line per check: the time, OK (and its identity), or LOST with the installer's reason (no MIDI port at all = the
# FM-1 has dropped off USB; the port listed but no answer = it is on USB but not replying). Close Live and anything
# else holding the FM-1's MIDI port first: the check needs the port for a moment each time.
param([int]$Every = 30)
Set-Location (Split-Path $PSScriptRoot -Parent)
New-Item -ItemType Directory -Force build | Out-Null
$log = "build\watch_fm1.log"
$py = if (Get-Command py.exe -ErrorAction SilentlyContinue) { "py.exe" } else { "python.exe" }
Write-Host "watching the FM-1 every $Every s (Ctrl+C to stop); log: $log"
$lost = 0; $n = 0
while ($true) {
    $n++
    $out = & $py tools\fm1_install.py --info 2>&1 | Out-String
    $t = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    if ($LASTEXITCODE -eq 0) { $line = "$t  OK    $($out.Trim())" }
    else { $lost++; $line = "$t  LOST  " + (($out -split "`r?`n" | Where-Object { $_ -match "\S" }) -join " | ") }
    Add-Content $log $line
    Write-Host ("[{0}/{1} lost] {2}" -f $lost, $n, $line) -ForegroundColor $(if ($LASTEXITCODE -eq 0) { "Green" } else { "Red" })
    Start-Sleep -Seconds $Every
}
