param()
$ErrorActionPreference = 'Stop'
$deskAssetDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'
Add-Type -AssemblyName System.Drawing
$deskLogo = [Drawing.Image]::FromFile((Join-Path $deskAssetDirectory 'deskflow-logo.png'))
$deskEntries = [Collections.Generic.List[byte[]]]::new()
$deskSizes = @(16,20,24,32,40,48,64,128,256)
try {
    foreach ($deskSize in ($deskSizes + @(44,50,150,310) | Sort-Object -Unique)) {
        $deskBitmap = [Drawing.Bitmap]::new($deskSize,$deskSize,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $deskGraphics = [Drawing.Graphics]::FromImage($deskBitmap)
        $deskStream = [IO.MemoryStream]::new()
        try {
            $deskGraphics.Clear([Drawing.Color]::Transparent)
            $deskGraphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $deskGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $deskGraphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $deskGraphics.DrawImage($deskLogo,[Drawing.Rectangle]::new(0,0,$deskSize,$deskSize))
            $deskBitmap.Save((Join-Path $deskAssetDirectory "DeskFlow-$deskSize.png"),[Drawing.Imaging.ImageFormat]::Png)
            if ($deskSize -in $deskSizes) {
                $deskBitmap.Save($deskStream,[Drawing.Imaging.ImageFormat]::Png)
                $deskEntries.Add($deskStream.ToArray())
            }
        } finally { $deskStream.Dispose(); $deskGraphics.Dispose(); $deskBitmap.Dispose() }
    }
    $deskIconStream = [IO.File]::Create((Join-Path $deskAssetDirectory 'DeskFlow.ico'))
    $deskWriter = [IO.BinaryWriter]::new($deskIconStream)
    try {
        $deskWriter.Write([uint16]0); $deskWriter.Write([uint16]1); $deskWriter.Write([uint16]$deskSizes.Count)
        $deskOffset = [uint32](6 + 16 * $deskSizes.Count)
        for ($deskIndex=0; $deskIndex -lt $deskSizes.Count; $deskIndex++) {
            $deskDimension = [byte]$(if ($deskSizes[$deskIndex] -eq 256) {0} else {$deskSizes[$deskIndex]})
            $deskWriter.Write($deskDimension); $deskWriter.Write($deskDimension)
            $deskWriter.Write([byte]0); $deskWriter.Write([byte]0)
            $deskWriter.Write([uint16]1); $deskWriter.Write([uint16]32)
            $deskWriter.Write([uint32]$deskEntries[$deskIndex].Length); $deskWriter.Write($deskOffset)
            $deskOffset += $deskEntries[$deskIndex].Length
        }
        foreach ($deskBytes in $deskEntries) { $deskWriter.Write([byte[]]$deskBytes) }
    } finally { $deskWriter.Dispose() }
} finally { $deskLogo.Dispose() }
Write-Host 'DeskFlow PNG and ICO assets generated from the original transparent logo.'
