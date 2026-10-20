# ============================================================
# Runner game asset one-click prepare script (ASCII only)
# 1) Download Kenney "New Platformer Pack" (CC0, OpenGameArt)
# 2) Recursively find all PNGs, pick player/obstacle/coin sprites
# 3) Generate 262x928 vertical parallax backgrounds
# 4) Output to tietu/paoku/{player,obstacle,collect,bg}
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File download_assets.ps1
# ============================================================
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Add-Type -AssemblyName System.Drawing

# 输出到脚本所在目录，便于他人 clone 后直接运行
$root = Join-Path $PSScriptRoot "tietu\paoku"
$work = Join-Path $PSScriptRoot ".tmp_assets"
$zip  = Join-Path $work "kenney_new-platformer-pack-1.0.zip"
$url  = "https://opengameart.org/sites/default/files/kenney_new-platformer-pack-1.0.zip"

foreach ($d in @('player','obstacle','collect','bg')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $root $d) | Out-Null
}
New-Item -ItemType Directory -Force -Path $work | Out-Null

# ---------- 1. download ----------
if (-not (Test-Path $zip)) {
    Write-Output ">> downloading CC0 pack (~3.5MB)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
} else {
    Write-Output ">> zip exists, skip download"
}
Write-Output ("    zip-size=" + (Get-Item $zip).Length)

# ---------- 2. extract ----------
$dest = Join-Path $work "extract"
if (-not (Get-ChildItem $dest -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    Expand-Archive -Path $zip -DestinationPath $dest -Force
}
Write-Output "=== top level ==="
Get-ChildItem $dest | ForEach-Object { Write-Output ("    " + $_.Name) }

# ---------- 3. pick sprites by name keyword + size limits ----------
function Is-Allowed([string]$kw, [int]$w, [int]$h, [int]$maxW, [int]$maxH) {
    return ($w -gt 0 -and $h -gt 0 -and $w -le $maxW -and $h -le $maxH -and $kw.Length -gt 0)
}

$playerRun  = @()
$playerJump = @()
$playerDead = @()
$obstacles  = @()
$coins      = @()

$allPng = Get-ChildItem $dest -Recurse -Filter *.png -ErrorAction SilentlyContinue
if (-not $allPng) { throw "no png found under $dest" }
Write-Output ("    total png = " + $allPng.Count)

foreach ($f in $allPng) {
    try {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $w = $img.Width; $h = $img.Height
        $img.Dispose()
    } catch { continue }

    $name = $f.Name.ToLower()
    $rel  = $f.FullName.Substring($dest.Length).Replace('\','/').ToLower()

    if (($name -match 'run|walk|movement|move') -and (Is-Allowed $rel $w $h 64 128)) {
        $playerRun += $f.FullName
    } elseif (($name -match 'jump|hop|fall') -and (Is-Allowed $rel $w $h 64 128)) {
        $playerJump += $f.FullName
    } elseif (($name -match 'dead|death|hurt|hit') -and (Is-Allowed $rel $w $h 64 128)) {
        $playerDead += $f.FullName
    }
    elseif (($name -match 'spike|thorn|saw|rock|stone|block|crate|barrel|enemy|mushroom|slime') -and (Is-Allowed $rel $w $h 80 128)) {
        $obstacles += $f.FullName
    }
    elseif (($name -match 'coin|gold|gem|diamond|star|jewel') -and (Is-Allowed $rel $w $h 64 64)) {
        $coins += $f.FullName
    }
}

Write-Output ("    playerRun=" + $playerRun.Count + " playerJump=" + $playerJump.Count + " playerDead=" + $playerDead.Count)
Write-Output ("    obstacles=" + $obstacles.Count + " coins=" + $coins.Count)

# ---------- 4. copy to category dirs (renamed, fixed order) ----------
function Copy-ToDir($src, [string]$dir, [string]$prefix) {
    $n = 0
    foreach ($p in $src) {
        if ($n -ge 8) { break }
        $n++
        Copy-Item $p (Join-Path $dir ($prefix + $n + '.png')) -Force
    }
    return $n
}

$copiedRun  = Copy-ToDir ($playerRun  | Sort-Object)  (Join-Path $root 'player')   'run'
$copiedJump = Copy-ToDir ($playerJump | Sort-Object)  (Join-Path $root 'player')   'jump'
$copiedDead = Copy-ToDir ($playerDead | Sort-Object)  (Join-Path $root 'player')   'dead'
$copiedObs  = Copy-ToDir ($obstacles  | Sort-Object)  (Join-Path $root 'obstacle') 'obs'
$copiedCoin = Copy-ToDir ($coins      | Sort-Object)  (Join-Path $root 'collect')  'coin'

Write-Output ("    copied: run=" + $copiedRun + " jump=" + $copiedJump + " dead=" + $copiedDead + " obs=" + $copiedObs + " coin=" + $copiedCoin)

# ---------- 5. generate 262x928 vertical parallax backgrounds ----------
function New-PixelSky() {
    $bmp = New-Object System.Drawing.Bitmap 262,928
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    for ($y=0; $y -lt 928; $y++) {
        $t = $y / 928.0
        $r = [int](120 + (94-120)*$t)
        $gr = [int](190 + (160-190)*$t)
        $b = [int](240 + (224-240)*$t)
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($r,$gr,$b))
        $g.DrawLine($pen, 0, $y, 262, $y)
        $pen.Dispose()
    }
    $clouds = @(@(30,120,3),@(90,150,2),@(170,110,3),@(30,300,2),@(120,340,3),@(200,280,2),@(60,520,3),@(150,560,2),@(40,740,3),@(140,760,2),@(210,720,3))
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200,255,255,255))
    foreach ($c in $clouds) {
        $bx=$c[0]; $by=$c[1]; $bs=$c[2]*10
        for($i=0;$i -lt $bs;$i++){
            $px=$bx+($i%5)*8; $py=$by+[int]($i/5)*6
            $g.FillRectangle($brush,$px,$py,8,6)
        }
    }
    $brush.Dispose()
    $g.Dispose()
    return $bmp
}

function New-PixelHills() {
    $bmp = New-Object System.Drawing.Bitmap 262,928
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(20,30,50))
    $layers = @(
        @(@(30,60,90), 300, 130),
        @(@(22,45,72), 520, 110),
        @(@(16,34,58), 740, 90)
    )
    foreach ($L in $layers) {
        $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($L[0][0],$L[0][1],$L[0][2]))
        $baseY=$L[1]; $peak=$L[2]
        for ($x=0; $x -lt 262; $x+=8) {
            $hh = $peak + ([int]([Math]::Sin($x*0.07)*($peak*0.4)))
            for ($y=$baseY; $y -lt 928; $y+=6) {
                $g.FillRectangle($brush,$x,$y,8,6)
            }
            for($i=0;$i -lt $hh;$i+=6){
                $g.FillRectangle($brush,$x+3, $baseY-$i-6, 2,6)
            }
        }
        $brush.Dispose()
    }
    $g.Dispose()
    return $bmp
}

function New-PixelRoad() {
    $bmp = New-Object System.Drawing.Bitmap 262,928
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(40,44,56))
    $bar = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(90,100,120))
    for ($y=0; $y -lt 928; $y+=10) {
        $g.FillRectangle($bar,0,$y,6,8)
        $g.FillRectangle($bar,256,$y,6,8)
    }
    $bar.Dispose()
    $line = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(180,180,190))
    for ($y=0; $y -lt 928; $y+=24) {
        $g.FillRectangle($line,83,$y,3,14)
        $g.FillRectangle($line,170,$y,3,14)
    }
    $line.Dispose()
    $g.Dispose()
    return $bmp
}

$bgFar  = New-PixelSky
$bgMid  = New-PixelHills
$bgNear = New-PixelRoad
$bgFar.Save((Join-Path $root 'bg\bg_far.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$bgMid.Save((Join-Path $root 'bg\bg_mid.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$bgNear.Save((Join-Path $root 'bg\bg_near.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$bgFar.Dispose(); $bgMid.Dispose(); $bgNear.Dispose()
Write-Output "    backgrounds generated: bg_far/bg_mid/bg_near.png (262x928)"

# ---------- 6. final listing ----------
Write-Output "=== final asset list ==="
Get-ChildItem $root -Recurse -Filter *.png | ForEach-Object {
    $img=[System.Drawing.Image]::FromFile($_.FullName)
    Write-Output ($_.FullName.Substring($root.Length).Replace('\','/') + "  " + $img.Width + "x" + $img.Height)
    $img.Dispose()
}
Write-Output "=== DONE ==="
