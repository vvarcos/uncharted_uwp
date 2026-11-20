<#
.SYNOPSIS
Open a run journal folder under runs/ from the template.
.DESCRIPTION
Creates runs/<slug>-DD-MM-HHmm/ with run.md and captures/, and a row in runs/INDEX.md.
The folder name is the state of the moment plus day, month and hour, e.g.
runs/status-do-momento-19-11-21h47/.
.EXAMPLE
./new-run.ps1 -Title 'status do momento' -Mode entry -Switches 'BB_SKIP_COPY=1'
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Title,
    [string]$Mode = '',
    [string]$Switches = '',
    [string]$Package = '',
    [string]$RunsDir = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
if ([string]::IsNullOrWhiteSpace($RunsDir)) { $RunsDir = Join-Path $root 'runs' }
if (!(Test-Path -LiteralPath $RunsDir)) { throw "Run journal not found: $RunsDir" }

function ConvertTo-Slug([string]$text) {
    $slug = $text.ToLowerInvariant() -replace '[^a-z0-9]+', '-'
    return $slug.Trim('-')
}

$today = Get-Date -Format 'dd-MM'
$slug = ConvertTo-Slug $Title
if ([string]::IsNullOrWhiteSpace($slug)) { throw 'Title produces an empty slug.' }

# One folder per run: <state of the moment>-DD-MM-HHmm.
$stamp = Get-Date -Format 'HHmm'
$name = "$slug-$today-$stamp"
$suffix = 2
while (Test-Path -LiteralPath (Join-Path $RunsDir $name)) {
    $name = "$slug-$today-$stamp-$suffix"
    $suffix++
}
$runDir = Join-Path $RunsDir $name
$entry = Join-Path $runDir 'run.md'
$captures = Join-Path $runDir 'captures'

if ($DryRun) {
    "folder:   $runDir"
    "entry:    $entry"
    "captures: $captures"
    exit 0
}

$commit = 'unknown'
try { $commit = (& git -C $root rev-parse --short HEAD 2>$null) } catch { }
if ([string]::IsNullOrWhiteSpace($commit)) { $commit = 'unknown' }

$template = Get-Content -LiteralPath (Join-Path $RunsDir '_TEMPLATE.md') -Raw
$values = [ordered]@{
    '{{ID}}'       = $name
    '{{TITLE}}'    = $Title
    '{{DATE}}'     = (Get-Date -Format 'yyyy-MM-dd HH:mm')
    '{{COMMIT}}'   = $commit
    '{{PACKAGE}}'  = $(if ($Package) { $Package } else { 'not recorded' })
    '{{MODE}}'     = $(if ($Mode) { $Mode } else { 'not recorded' })
    '{{SWITCHES}}' = $(if ($Switches) { $Switches } else { 'none' })
}
$text = $template
foreach ($key in $values.Keys) { $text = $text.Replace($key, $values[$key]) }
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
Set-Content -LiteralPath $entry -Value $text -Encoding utf8NoBOM
New-Item -ItemType Directory -Path $captures -Force | Out-Null

$indexPath = Join-Path $RunsDir 'INDEX.md'
$index = Get-Content -LiteralPath $indexPath -Raw
$row = "| [$name]($name/run.md) | $today $stamp | $Mode | not run yet | not measured | not measured | not measured | |"
if ($index -match '<!-- RUNS -->') {
    $index = $index.Replace('<!-- RUNS -->', "<!-- RUNS -->`n$row")
} else {
    $index = $index.TrimEnd() + "`n$row`n"
}
Set-Content -LiteralPath $indexPath -Value $index -Encoding utf8NoBOM

"folder:   $runDir"
"entry:    $entry"
"captures: $captures"
