#requires -Version 7.2
# Generates the UWP package logos from the app artwork in xbox/assets/AppIcon.png.
# Square logos use a centred square crop of the artwork; the splash screen centres the
# artwork at full height on the package background colour.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$root=Split-Path $PSScriptRoot -Parent
$assets=Join-Path $root '.local/build/layout/Assets'
New-Item -ItemType Directory -Force $assets | Out-Null
$source=Join-Path $root 'assets/AppIcon.png'
$background=[Drawing.Color]::FromArgb(16,24,32)

function New-Logo([string]$name,[int]$width,[int]$height,[bool]$square) {
    $bitmap=[Drawing.Bitmap]::new($width,$height)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear($background)
        $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::HighQuality
        if($script:art) {
            $art=$script:art
            if($square) {
                $side=[Math]::Min($art.Width,$art.Height)
                $crop=[Drawing.Rectangle]::new([int](($art.Width-$side)/2),[int](($art.Height-$side)/2),$side,$side)
                $scale=[Math]::Min($width,$height)
                $offsetX=[int](($width-$scale)/2); $offsetY=[int](($height-$scale)/2)
                $graphics.DrawImage($art,[Drawing.Rectangle]::new($offsetX,$offsetY,$scale,$scale),$crop,[Drawing.GraphicsUnit]::Pixel)
            } else {
                $scale=[Math]::Min($height/$art.Height,$width/$art.Width)
                $drawWidth=[int]($art.Width*$scale); $drawHeight=[int]($art.Height*$scale)
                $graphics.DrawImage($art,[Drawing.Rectangle]::new([int](($width-$drawWidth)/2),[int](($height-$drawHeight)/2),$drawWidth,$drawHeight))
            }
        } else {
            $font=[Drawing.Font]::new('Segoe UI',[float]([Math]::Min($width,$height)*0.45),[Drawing.FontStyle]::Bold)
            try {
                $format=[Drawing.StringFormat]::new(); $format.Alignment='Center'; $format.LineAlignment='Center'
                $graphics.DrawString('U',$font,[Drawing.Brushes]::Goldenrod,[Drawing.RectangleF]::new(0,0,$width,$height),$format)
                $format.Dispose()
            } finally { $font.Dispose() }
        }
        $bitmap.Save((Join-Path $assets $name),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}

$script:art=$null
if(Test-Path -LiteralPath $source) { $script:art=[Drawing.Image]::FromFile($source) }
try {
    New-Logo 'StoreLogo.png' 50 50 $true
    New-Logo 'Square150x150Logo.png' 150 150 $true
    New-Logo 'Square44x44Logo.png' 44 44 $true
    New-Logo 'SplashScreen.png' 620 300 $false
} finally { if($script:art) { $script:art.Dispose() } }
