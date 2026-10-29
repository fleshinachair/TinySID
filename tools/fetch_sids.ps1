# Downloads a hand-picked set of classic C64 tunes from an HVSC mirror into assets\ (git-ignored for *.sid).
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File tools\fetch_sids.ps1
# It reads each composer's folder listing, so exact filenames are matched, not guessed; anything it can't find is reported.
param(
    [string]$Base = 'https://hvsc.brona.dk/HVSC/C64Music/MUSICIANS',
    [string]$Out  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets')
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# folder under MUSICIANS, then the tune names to look for (matched case-insensitively against the .sid file names)
$wanted = @(
    @('H/Hubbard_Rob',   'Monty_on_the_Run', 'International_Karate', 'Delta', 'Sanxion', 'Crazy_Comets', 'Zoids', 'Thing_on_a_Spring', 'One_Man_and_His_Droid'),
    @('G/Galway_Martin', 'Wizball', 'Comic_Bakery', 'Parallax', 'Ocean_Loader_3', 'Arkanoid', 'Game_Over'),
    @('D/Daglish_Ben',   'Last_Ninja'),
    @('G/Gray_Matt',     'Last_Ninja_2'),
    @('H/Huelsbeck_Chris', 'Turrican', 'Great_Giana_Sisters'),
    @('T/Tel_Jeroen',    'Cybernoid', 'Hawkeye'),
    @('W/Walker_Martin', 'Armalyte')
)

New-Item -ItemType Directory -Force $Out | Out-Null
$got = 0; $missed = @()

foreach ($group in $wanted) {
    $folder = $group[0]
    $url = "$Base/$folder/"
    try {
        $page = (Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 60).Content
    } catch {
        Write-Host "!! cannot list $url : $($_.Exception.Message)" -ForegroundColor Red
        $group[1..($group.Count - 1)] | ForEach-Object { $missed += "$folder/$_" }
        continue
    }
    $files = [regex]::Matches($page, 'href="([^"?]+\.sid)"', 'IgnoreCase') |
        ForEach-Object { [Uri]::UnescapeDataString($_.Groups[1].Value) } | Select-Object -Unique

    foreach ($tune in $group[1..($group.Count - 1)]) {
        # exact name first, then the first file that starts with it
        $hit = $files | Where-Object { $_ -ieq "$tune.sid" } | Select-Object -First 1
        if (-not $hit) { $hit = $files | Where-Object { $_ -ilike "$tune*" } | Select-Object -First 1 }
        if (-not $hit) { Write-Host "-- not found: $folder/$tune" -ForegroundColor Yellow; $missed += "$folder/$tune"; continue }

        $dest = Join-Path $Out $hit
        if (Test-Path $dest) { Write-Host "== already have $hit"; continue }
        try {
            Invoke-WebRequest -UseBasicParsing -Uri ($url + [Uri]::EscapeDataString($hit)) -OutFile $dest -TimeoutSec 120
            $magic = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dest), 0, 4)
            if ($magic -ne 'PSID' -and $magic -ne 'RSID') { Remove-Item $dest; throw "not a SID file (starts '$magic')" }
            Write-Host "ok $hit ($((Get-Item $dest).Length) bytes)" -ForegroundColor Green
            $got++
        } catch {
            Write-Host "!! $hit : $($_.Exception.Message)" -ForegroundColor Red
            $missed += "$folder/$tune"
        }
    }
}

Write-Host ""
Write-Host "Downloaded $got file(s) into $Out"
if ($missed.Count) { Write-Host "Missed: $($missed -join ', ')" -ForegroundColor Yellow }
