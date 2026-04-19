Add-Type -AssemblyName System.Drawing

function New-IconBitmap {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $bg = [System.Drawing.Color]::FromArgb(255, 88, 101, 242)
    $radius = [Math]::Max(2, [int]($Size * 0.18))
    $rect = New-Object System.Drawing.Rectangle(0, 0, $Size, $Size)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $bgBrush = New-Object System.Drawing.SolidBrush($bg)
    $g.FillPath($bgBrush, $path)

    $white = [System.Drawing.Color]::White
    $whiteBrush = New-Object System.Drawing.SolidBrush($white)
    $s = [double]$Size
    $body = New-Object System.Drawing.RectangleF([single]($s*0.22), [single]($s*0.40), [single]($s*0.18), [single]($s*0.20))
    $g.FillRectangle($whiteBrush, $body)

    $cone = @(
        (New-Object System.Drawing.PointF([single]($s*0.40), [single]($s*0.40))),
        (New-Object System.Drawing.PointF([single]($s*0.60), [single]($s*0.20))),
        (New-Object System.Drawing.PointF([single]($s*0.60), [single]($s*0.80))),
        (New-Object System.Drawing.PointF([single]($s*0.40), [single]($s*0.60)))
    )
    $g.FillPolygon($whiteBrush, $cone)

    $red = [System.Drawing.Color]::FromArgb(255, 237, 66, 69)
    $penWidth = [Math]::Max(1.5, $s * 0.10)
    $pen = New-Object System.Drawing.Pen($red, [single]$penWidth)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $p1 = New-Object System.Drawing.PointF([single]($s*0.18), [single]($s*0.82))
    $p2 = New-Object System.Drawing.PointF([single]($s*0.82), [single]($s*0.18))
    $g.DrawLine($pen, $p1, $p2)

    $pen.Dispose()
    $bgBrush.Dispose()
    $whiteBrush.Dispose()
    $path.Dispose()
    $g.Dispose()
    return $bmp
}

# Build a single-size ICO using GDI's CreateIconIndirect + Icon.Save, which
# produces a BMP-format ICO that rc.exe accepts. We then merge multiple sizes
# into one multi-resolution ICO by splicing directory entries together.

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class IconNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct ICONINFO {
        public bool fIcon;
        public int xHotspot;
        public int yHotspot;
        public IntPtr hbmMask;
        public IntPtr hbmColor;
    }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr CreateIconIndirect(ref ICONINFO piconinfo);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool DestroyIcon(IntPtr hIcon);
    [DllImport("gdi32.dll", SetLastError=true)]
    public static extern bool DeleteObject(IntPtr hObject);
    [DllImport("gdi32.dll", SetLastError=true)]
    public static extern IntPtr CreateBitmap(int w, int h, uint planes, uint bpp, IntPtr bits);
}
'@

function New-SingleSizeIcoBytes {
    param([System.Drawing.Bitmap]$Bmp)

    # Build an HICON that preserves alpha via CreateIconIndirect (color + mask).
    # We create a 1bpp AND mask of all zeros so alpha lives in the color bitmap.
    $w = $Bmp.Width; $h = $Bmp.Height
    $hbmColor = $Bmp.GetHbitmap([System.Drawing.Color]::FromArgb(0,0,0,0))
    $hbmMask  = [IconNative]::CreateBitmap($w, $h, 1, 1, [IntPtr]::Zero)

    $info = New-Object IconNative+ICONINFO
    $info.fIcon    = $true
    $info.hbmColor = $hbmColor
    $info.hbmMask  = $hbmMask
    $hIcon = [IconNative]::CreateIconIndirect([ref]$info)

    [IconNative]::DeleteObject($hbmColor) | Out-Null
    [IconNative]::DeleteObject($hbmMask)  | Out-Null

    if ($hIcon -eq [IntPtr]::Zero) { throw "CreateIconIndirect failed" }

    $icon = [System.Drawing.Icon]::FromHandle($hIcon)
    $ms = New-Object System.IO.MemoryStream
    $icon.Save($ms)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    $icon.Dispose()
    [IconNative]::DestroyIcon($hIcon) | Out-Null
    return $bytes
}

# Given several single-entry ICO files (as byte arrays), merge them into one
# multi-resolution ICO by concatenating their image data and rewriting the
# directory.
function Merge-Icos {
    param([byte[][]]$IcoList, [string]$OutPath)

    $count = $IcoList.Count
    $imageBlobs = @()
    $entries = @()

    foreach ($ico in $IcoList) {
        # ICONDIR: 6 bytes. ICONDIRENTRY: 16 bytes. Image data follows.
        $entrySize = [BitConverter]::ToUInt32($ico, 6 + 8)   # dwBytesInRes
        $entryOff  = [BitConverter]::ToUInt32($ico, 6 + 12)  # dwImageOffset
        $w  = $ico[6 + 0]
        $h  = $ico[6 + 1]
        $cp = $ico[6 + 2]
        $rs = $ico[6 + 3]
        $pl = [BitConverter]::ToUInt16($ico, 6 + 4)
        $bp = [BitConverter]::ToUInt16($ico, 6 + 6)

        $blob = New-Object byte[] $entrySize
        [Array]::Copy($ico, [int]$entryOff, $blob, 0, [int]$entrySize)

        $entries += ,@{ W=$w; H=$h; CP=$cp; RS=$rs; PL=$pl; BP=$bp; Size=$entrySize }
        $imageBlobs += ,$blob
    }

    $fs = [System.IO.File]::Open($OutPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    $bw = New-Object System.IO.BinaryWriter($fs)
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$count)

    $offset = 6 + 16 * $count
    for ($i = 0; $i -lt $count; $i++) {
        $e = $entries[$i]
        $bw.Write([byte]$e.W); $bw.Write([byte]$e.H); $bw.Write([byte]$e.CP); $bw.Write([byte]$e.RS)
        $bw.Write([uint16]$e.PL); $bw.Write([uint16]$e.BP)
        $bw.Write([uint32]$e.Size); $bw.Write([uint32]$offset)
        $offset += $e.Size
    }
    for ($i = 0; $i -lt $count; $i++) { $bw.Write($imageBlobs[$i]) }
    $bw.Flush(); $bw.Dispose(); $fs.Dispose()
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128)
$icos = @()
foreach ($sz in $sizes) {
    $bmp = New-IconBitmap -Size $sz
    $icos += ,(New-SingleSizeIcoBytes -Bmp $bmp)
    $bmp.Dispose()
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$outPath   = Join-Path $repoRoot 'app.ico'
Merge-Icos -IcoList $icos -OutPath $outPath
Write-Host "Wrote $outPath ($($sizes -join ','))"
