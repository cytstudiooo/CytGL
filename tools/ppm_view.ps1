<#
    SPDX-License-Identifier: Apache-2.0
    Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>

    ppm_view.ps1 -- "view" a render result when there is no image viewer to hand
    (or you do not want to switch windows)

    Downsamples the PPM snapshots produced by tests/render_host_test.c into
    character art and prints it straight to the console.
    Uses:
      - confirm layout, font size and dot matrix in a plain terminal / CI
      - compare animation frame by frame (the frames where the ball cuts in are
        the most revealing)
      - use -Sample to change the sampling density; use -Threshold to show only
        the bright pixels (dot matrix / ball) and ignore the floor

    Usage (from the project root):
      powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1
      powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -File build/frame02.ppm
      powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Threshold 200
      powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Sample 3 -Width 200

    Parameters:
      -File      PPM to view (default: view every build/frame*.ppm in turn)
      -Sample    each character stands for N x N pixels (default 2)
      -Width     above this character width, downsample further (default 170)
      -Threshold brightness threshold (0-255); when greater than 0 only pixels
                 brighter than this value are shown, which is how the dot matrix
                 is "picked out" of the floor

    This file used to have to be saved as UTF-8 WITH THE BOM: without it Windows
    PowerShell 5.1 decoded the Chinese comments as ANSI and reported a syntax
    error. It is pure ASCII now, so the BOM is no longer needed.
#>
[CmdletBinding()]
param(
    [string]$File = '',
    [int]$Sample = 2,
    [int]$Width = 170,
    [int]$Threshold = 0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Read-Ppm {
    param([string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $i = 0
    $tokens = @()
    # Read the first 4 header fields: magic / width / height / maxval
    while ($tokens.Count -lt 4) {
        while ($i -lt $bytes.Length -and $bytes[$i] -in 32, 10, 13, 9) { $i++ }
        if ($i -lt $bytes.Length -and $bytes[$i] -eq 0x23) {   # '#': comment runs to the end of the line
            while ($i -lt $bytes.Length -and $bytes[$i] -ne 10) { $i++ }
            continue
        }
        $sb = New-Object System.Text.StringBuilder
        while ($i -lt $bytes.Length -and $bytes[$i] -notin 32, 10, 13, 9) {
            [void]$sb.Append([char]$bytes[$i]); $i++
        }
        if ($sb.Length -gt 0) { $tokens += $sb.ToString() }
    }
    $i++   # Exactly one whitespace byte between the header and the pixel data
    return @{
        Width  = [int]$tokens[1]
        Height = [int]$tokens[2]
        Data   = $bytes
        Offset = $i
    }
}

function Show-Ppm {
    param([string]$Path)

    $img = Read-Ppm -Path $Path
    $w = $img.Width
    $h = $img.Height
    $data = $img.Data
    $off = $img.Offset

    # Sampling stride: satisfy -Sample, but also stay within -Width characters
    $step = $Sample
    while ([math]::Ceiling($w / $step) -gt $Width) { $step++ }

    Write-Host ''
    Write-Host ("=== {0}  ({1}x{2}, {3}x{3} px per char, {4} columns) ===" -f `
        (Split-Path $Path -Leaf), $w, $h, $step, [math]::Ceiling($w / $step)) -ForegroundColor Cyan

    # Brightness ramp: floor / grid / shadow / dots each get a level, so layers are visible at a glance
    $ramp = ' .:-=+*#%@'

    for ($by = 0; $by -lt $h; $by += $step) {
        $line = New-Object System.Text.StringBuilder
        for ($bx = 0; $bx -lt $w; $bx += $step) {
            $sum = 0
            $n = 0
            $peak = 0
            for ($y = $by; $y -lt [math]::Min($by + $step, $h); $y++) {
                for ($x = $bx; $x -lt [math]::Min($bx + $step, $w); $x++) {
                    $o = $off + (($y * $w) + $x) * 3
                    # PPM is RGB; the eye is most sensitive to green, so weighted luminance is enough
                    $lum = [int](0.299 * $data[$o] + 0.587 * $data[$o + 1] + 0.114 * $data[$o + 2])
                    $sum += $lum
                    $n++
                    if ($lum -gt $peak) { $peak = $lum }
                }
            }
            if ($Threshold -gt 0) {
                # Threshold mode: use the brightest value in the block, so a 1 px dot is not averaged away
                [void]$line.Append($(if ($peak -ge $Threshold) { '#' } else { ' ' }))
            }
            else {
                $avg = [int]($sum / [math]::Max($n, 1))
                $idx = [int]([math]::Round($avg / 255.0 * ($ramp.Length - 1)))
                [void]$line.Append($ramp[$idx])
            }
        }
        Write-Host $line.ToString()
    }
}

# --------------------------------------------------------------------------

if ($File -ne '') {
    $p = if ([System.IO.Path]::IsPathRooted($File)) { $File } else { Join-Path $root $File }
    if (-not (Test-Path $p)) { throw "Cannot find $p (run render_test first to generate the snapshots)" }
    Show-Ppm -Path $p
}
else {
    $files = Get-ChildItem -Path (Join-Path $root 'build') -Filter 'frame*.ppm' -ErrorAction SilentlyContinue |
             Sort-Object Name
    if (-not $files) { throw 'No frame*.ppm under build/, run render_test first' }
    foreach ($f in $files) { Show-Ppm -Path $f.FullName }
}
