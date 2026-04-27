param(
    [Parameter(Mandatory = $true)]
    [string]$InputPng,
    [Parameter(Mandatory = $true)]
    [string]$OutputIco
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class NativeIconMethods {
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DestroyIcon(IntPtr handle);
}
"@

function Test-IsNearlyWhite {
    param([System.Drawing.Color]$Color)

    return $Color.A -gt 0 -and $Color.R -ge 245 -and $Color.G -ge 245 -and $Color.B -ge 245
}

function Get-ContentBounds {
    param([System.Drawing.Bitmap]$Bitmap)

    $minX = $Bitmap.Width
    $minY = $Bitmap.Height
    $maxX = -1
    $maxY = -1

    for ($y = 0; $y -lt $Bitmap.Height; $y++) {
        for ($x = 0; $x -lt $Bitmap.Width; $x++) {
            $pixel = $Bitmap.GetPixel($x, $y)
            if (Test-IsNearlyWhite $pixel) {
                continue
            }

            if ($x -lt $minX) { $minX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }

    if ($maxX -lt $minX -or $maxY -lt $minY) {
        return [System.Drawing.Rectangle]::new(0, 0, $Bitmap.Width, $Bitmap.Height)
    }

    return [System.Drawing.Rectangle]::new($minX, $minY, $maxX - $minX + 1, $maxY - $minY + 1)
}

$inputPath = [System.IO.Path]::GetFullPath($InputPng)
$outputPath = [System.IO.Path]::GetFullPath($OutputIco)
$outputDirectory = [System.IO.Path]::GetDirectoryName($outputPath)
if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

$iconSize = 64
$padding = 4

$sourceBitmap = [System.Drawing.Bitmap]::FromFile($inputPath)
try {
    $contentBounds = Get-ContentBounds -Bitmap $sourceBitmap
    $iconBitmap = [System.Drawing.Bitmap]::new(
        $iconSize,
        $iconSize,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    )

    try {
        $graphics = [System.Drawing.Graphics]::FromImage($iconBitmap)
        $imageAttributes = [System.Drawing.Imaging.ImageAttributes]::new()

        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

            $imageAttributes.SetColorKey(
                [System.Drawing.Color]::FromArgb(255, 245, 245, 245),
                [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
            )

            $availableWidth = $iconSize - ($padding * 2)
            $availableHeight = $iconSize - ($padding * 2)
            $scale = [Math]::Min(
                $availableWidth / [double][Math]::Max($contentBounds.Width, 1),
                $availableHeight / [double][Math]::Max($contentBounds.Height, 1)
            )

            $drawWidth = [Math]::Max(1, [int][Math]::Round($contentBounds.Width * $scale))
            $drawHeight = [Math]::Max(1, [int][Math]::Round($contentBounds.Height * $scale))
            $offsetX = [int](($iconSize - $drawWidth) / 2)
            $offsetY = [int](($iconSize - $drawHeight) / 2)
            $destinationRect = [System.Drawing.Rectangle]::new($offsetX, $offsetY, $drawWidth, $drawHeight)

            $graphics.DrawImage(
                $sourceBitmap,
                $destinationRect,
                $contentBounds.X,
                $contentBounds.Y,
                $contentBounds.Width,
                $contentBounds.Height,
                [System.Drawing.GraphicsUnit]::Pixel,
                $imageAttributes
            )
        } finally {
            $imageAttributes.Dispose()
            $graphics.Dispose()
        }

        $iconHandle = $iconBitmap.GetHicon()
        try {
            $icon = [System.Drawing.Icon]::FromHandle($iconHandle)
            try {
                $stream = [System.IO.File]::Create($outputPath)
                try {
                    $icon.Save($stream)
                } finally {
                    $stream.Dispose()
                }
            } finally {
                $icon.Dispose()
            }
        } finally {
            [void][NativeIconMethods]::DestroyIcon($iconHandle)
        }
    } finally {
        $iconBitmap.Dispose()
    }
} finally {
    $sourceBitmap.Dispose()
}
