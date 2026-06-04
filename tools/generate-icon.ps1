param(
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Output) {
    $Output = Join-Path $root "gfx\icon.png"
}

Add-Type -AssemblyName System.Drawing

$size = 48
$scale = 8
$canvas = $size * $scale

function ColorFromHex([string]$hex, [int]$alpha = 255) {
    $h = $hex.TrimStart("#")
    return [System.Drawing.Color]::FromArgb(
        $alpha,
        [Convert]::ToInt32($h.Substring(0, 2), 16),
        [Convert]::ToInt32($h.Substring(2, 2), 16),
        [Convert]::ToInt32($h.Substring(4, 2), 16)
    )
}

function S([float]$value) {
    return $value * $script:scale
}

function New-RoundRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = S($r * 2)
    $xx = S($x)
    $yy = S($y)
    $ww = S($w)
    $hh = S($h)

    $path.AddArc($xx, $yy, $d, $d, 180, 90)
    $path.AddArc($xx + $ww - $d, $yy, $d, $d, 270, 90)
    $path.AddArc($xx + $ww - $d, $yy + $hh - $d, $d, $d, 0, 90)
    $path.AddArc($xx, $yy + $hh - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Fill-RoundRect($g, [float]$x, [float]$y, [float]$w, [float]$h, [float]$r, $brush) {
    $path = New-RoundRectPath $x $y $w $h $r
    $g.FillPath($brush, $path)
    $path.Dispose()
}

function Stroke-RoundRect($g, [float]$x, [float]$y, [float]$w, [float]$h, [float]$r, $pen) {
    $path = New-RoundRectPath $x $y $w $h $r
    $g.DrawPath($pen, $path)
    $path.Dispose()
}

function Points([float[]]$coords) {
    $pts = New-Object "System.Drawing.PointF[]" ($coords.Length / 2)
    for ($i = 0; $i -lt $coords.Length; $i += 2) {
        $x = [float](S ([float]$coords[$i]))
        $y = [float](S ([float]$coords[$i + 1]))
        $pts[$i / 2] = [System.Drawing.PointF]::new($x, $y)
    }
    return $pts
}

$bmp = New-Object System.Drawing.Bitmap($canvas, $canvas, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

$bgRect = New-Object System.Drawing.RectangleF 0, 0, $canvas, $canvas
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $bgRect,
    (ColorFromHex "#101014"),
    (ColorFromHex "#17131d"),
    [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal
)
$g.FillRectangle($bg, $bgRect)

$cyan = ColorFromHex "#00a4dc"
$purple = ColorFromHex "#aa5cc3"
$gold = ColorFromHex "#f4b400"
$panel = ColorFromHex "#202026"
$panelDark = ColorFromHex "#15151b"
$ink = ColorFromHex "#050507"
$white = ColorFromHex "#f5f7fb"

$gridPen = New-Object System.Drawing.Pen (ColorFromHex "#ffffff" 14), (S 0.35)
for ($i = -48; $i -lt 96; $i += 8) {
    $g.DrawLine($gridPen, (S $i), 0, (S ($i + 48)), (S 48))
}

$cyanGlow = New-Object System.Drawing.Pen (ColorFromHex "#00a4dc" 95), (S 2.2)
$purpleGlow = New-Object System.Drawing.Pen (ColorFromHex "#aa5cc3" 95), (S 2.2)
$cyanPen = New-Object System.Drawing.Pen $cyan, (S 1.25)
$purplePen = New-Object System.Drawing.Pen $purple, (S 1.25)
$darkPen = New-Object System.Drawing.Pen (ColorFromHex "#000000" 115), (S 1.1)

$panelBrush = New-Object System.Drawing.SolidBrush $panel
$panelDarkBrush = New-Object System.Drawing.SolidBrush $panelDark

Fill-RoundRect $g 6 5 36 16 3 $panelBrush
Stroke-RoundRect $g 6 5 36 16 3 $cyanGlow
Stroke-RoundRect $g 6 5 36 16 3 $cyanPen

Fill-RoundRect $g 8 27 32 14 2.6 $panelDarkBrush
Stroke-RoundRect $g 8 27 32 14 2.6 $purpleGlow
Stroke-RoundRect $g 8 27 32 14 2.6 $purplePen

$hingePen = New-Object System.Drawing.Pen (ColorFromHex "#f5f7fb" 45), (S 1)
$g.DrawLine($hingePen, (S 15), (S 24), (S 33), (S 24))

$shadowBrush = New-Object System.Drawing.SolidBrush (ColorFromHex "#050507" 150)
$purpleBrush = New-Object System.Drawing.SolidBrush (ColorFromHex "#aa5cc3" 230)
$cyanBrush = New-Object System.Drawing.SolidBrush (ColorFromHex "#00a4dc" 235)
$goldBrush = New-Object System.Drawing.SolidBrush $gold
$shineBrush = New-Object System.Drawing.SolidBrush (ColorFromHex "#ffffff" 90)

$g.FillPolygon($shadowBrush, (Points @(18.8, 13.7, 18.8, 35.9, 36.2, 24.8)))
$g.FillPolygon($purpleBrush, (Points @(18.0, 13.0, 18.0, 35.0, 35.2, 24.1)))
$g.FillPolygon($cyanBrush, (Points @(15.8, 12.0, 15.8, 34.0, 33.0, 23.0)))
$g.DrawPolygon($darkPen, (Points @(15.8, 12.0, 15.8, 34.0, 33.0, 23.0)))
$g.FillPolygon($goldBrush, (Points @(20.4, 17.5, 20.4, 30.0, 30.0, 23.8)))
$g.FillPolygon($shineBrush, (Points @(20.4, 17.5, 20.4, 21.0, 26.3, 21.5)))

$rimPen = New-Object System.Drawing.Pen (ColorFromHex "#f5f7fb" 65), (S 0.85)
$g.DrawLine($rimPen, (S 17.4), (S 13.5), (S 17.4), (S 32.4))

$cornerCyan = New-Object System.Drawing.SolidBrush (ColorFromHex "#00a4dc" 210)
$cornerPurple = New-Object System.Drawing.SolidBrush (ColorFromHex "#aa5cc3" 210)
$cornerGold = New-Object System.Drawing.SolidBrush (ColorFromHex "#f4b400" 235)
Fill-RoundRect $g 4.1 4.1 5.5 1.4 0.7 $cornerGold
Fill-RoundRect $g 4.1 4.1 1.4 5.5 0.7 $cornerGold
Fill-RoundRect $g 38.4 42.5 5.5 1.4 0.7 $cornerPurple
Fill-RoundRect $g 42.5 38.4 1.4 5.5 0.7 $cornerCyan

$g.Dispose()

$outBmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$outG = [System.Drawing.Graphics]::FromImage($outBmp)
$outG.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$outG.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$outG.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$outG.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$outG.DrawImage($bmp, 0, 0, $size, $size)

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
$outBmp.Save($Output, [System.Drawing.Imaging.ImageFormat]::Png)

$outG.Dispose()
$outBmp.Dispose()
$bmp.Dispose()

Write-Host "Icon written to $Output"
