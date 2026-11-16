<#
.SYNOPSIS
    Pre-flight for a game dump: does it contain the exact paths the guest asks for?

.DESCRIPTION
    The diagnostic run records every path the guest could not open. Those paths ARE the specification
    of the build's data. Scanning a candidate dump for them takes seconds against the archives and
    minutes against the .pak files, and it decides whether a run is worth spending before installing
    anything on the console.

.EXAMPLE
    .\dump_preflight.ps1 -Dump 'C:\...\CUSA02320'
    .\dump_preflight.ps1 -Dump 'C:\...\CUSA02320' -Full
#>
param(
    [Parameter(Mandatory = $true)][string]$Dump,
    [switch]$Full,
    [string[]]$Paths,
    # Retail floor is a property of the GAME, not of the port: the next title has another date.
    [datetime]$RetailFloor = '2015-10-01',
    [switch]$Strict

)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Dump)) { throw "dump not found: $Dump" }

if (-not $Paths) {
    $Paths = @(
        'u1data/build/main/bpdata1/particle.json'
        'u1data/build/main/effect1/particle-ingame.bin'
        'u1data/build/main/effect1/particle.bin'
        'u1data/build/main/effect1/proto.bin'
        'u1data/build/main/effect1/texturedict1/chunk-1-tstrm-1.bin'
        'u1data/build/main/effect1/texturedict1/chunk-1-tstrm.bin'
        'u1data/build/main/effect1/world-0-ingame.bin'
        'u1data/build/main/effect1/world-0.bin'
        'u1data/build/main/effect1/world-1-ingame.bin'
        'u1data/build/main/effect1/world-1.bin'
        'u1data/build/main/lut0/default-color-gray.dds'
        'u1data/build/main/movie1/texture.txt'
    )
}

# Every container worth searching: the archives first (cheap), the .pak payloads only with -Full.
$containers = Get-ChildItem $Dump -Recurse -File -Include *.psarc
if ($Full) {
    $containers += Get-ChildItem $Dump -Recurse -File -Include *.pak
}

$bytesPerContainer = @{}
Write-Host ("scanning {0} containers ({1:N1} GB)" -f $containers.Count,
            (($containers | Measure-Object Length -Sum).Sum / 1GB))

$buffer = New-Object byte[] (8 * 1024 * 1024)
foreach ($container in $containers) {
    $stream = [IO.File]::OpenRead($container.FullName)
    try {
        $carry = 0
        while (($read = $stream.Read($buffer, $carry, $buffer.Length - $carry)) -gt 0) {
            $total = $carry + $read
            $text = [Text.Encoding]::ASCII.GetString($buffer, 0, $total)
            foreach ($path in $Paths) {
                if (-not $bytesPerContainer.ContainsKey($path) -and $text.Contains($path)) {
                    $bytesPerContainer[$path] = $container.FullName
                }
            }
            $carry = [Math]::Min(64, $total)
            [Array]::Copy($buffer, $total - $carry, $buffer, 0, $carry)
        }
    } finally {
        $stream.Dispose()
    }
}

$rows = foreach ($path in $Paths) {
    $loose = Test-Path (Join-Path $Dump ($path -replace '/', '\'))
    [pscustomobject]@{
        Path     = $path
        Loose    = $loose
        InArchive = if ($bytesPerContainer.ContainsKey($path)) { Split-Path $bytesPerContainer[$path] -Leaf } else { '' }
        Ok       = $loose -or $bytesPerContainer.ContainsKey($path)
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host
$missing = @($rows | Where-Object { -not $_.Ok })

# The names are the SAMPLE the run log revealed, so 12/12 is necessary but not sufficient: a dump from
# another build can carry them and still differ elsewhere. The build date is the second, independent
# check. param.sfo carries the app version and c_date; gameversion.txt carries the internal build id.
$retailFloor = $RetailFloor
$dataDate = $null
$appDate = $null
$gameVersion = Join-Path $Dump 'u1data\build\main\gameversion.txt'
if (Test-Path $gameVersion) {
    $text = Get-Content $gameVersion -Raw
    if ($text -match '(20\d\d)_(\d\d)_(\d\d)') {
        $dataDate = [datetime]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    }
}
$sfo = Join-Path $Dump 'sce_sys\param.sfo'
if (Test-Path $sfo) {
    $raw = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($sfo))
    if ($raw -match 'c_date=(\d{4})(\d\d)(\d\d)') {
        $appDate = [datetime]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    }
}

$buildOk = $true
$delta = ''
if ($dataDate) {
    if ($appDate) {
        $days = [int]($dataDate - $appDate).TotalDays
        $delta = if ($days -ge 0) { "+${days}d" } else { "${days}d" }
    }
    if ($dataDate -lt $retailFloor) { $buildOk = $false }
}

$namesOk = $missing.Count -eq 0
$floorDefault = [datetime]'2015-10-01' # the value above
$floorNote = if ($retailFloor -ne $floorDefault) { ' (override)' } else { '' }
$summary = 'strings {0}/{1}, build {2}, floor={3}{4}, delta={5} -> {6}' -f
    ($Paths.Count - $missing.Count), $Paths.Count,
    $(if ($dataDate) { $dataDate.ToString('yyyy_MM_dd') } else { 'unknown' }),
    $retailFloor.ToString('yyyy_MM_dd'), $floorNote,
    $(if ($delta) { $delta } else { 'n/a' }),
    $(if ($namesOk -and $buildOk) { 'PASS' } else { 'FAIL' })

if ($missing.Count) {
    Write-Host "MISSING:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host ("  " + $_.Path) }
}
if (-not $buildOk) {
    Write-Host ("data build {0} predates the retail release ({1}); eboot says {2}" -f
                $dataDate.ToString('yyyy-MM-dd'), $retailFloor.ToString('yyyy-MM-dd'),
                $(if ($appDate) { $appDate.ToString('yyyy-MM-dd') } else { 'unknown' })) -ForegroundColor Yellow
}
Write-Host $summary -ForegroundColor $(if ($namesOk -and $buildOk) { 'Green' } else { 'Yellow' })
# Measured on the OFFICIAL package of this title: the retail data itself reports 0/12 and a September 2015
# build string, because those twelve paths are optional probes the game tolerates and c_date is when the
# patch was submitted, not when the content was built. So by default the tool reports and warns; only
# -Strict turns it into a gate.
if (-not ($namesOk -and $buildOk)) {
    Write-Host "note: retail data of this title also reports 0/12 and an old build string; use -Strict to fail on this" -ForegroundColor DarkGray
    if ($Strict) { exit 1 }
}
exit 0
