param([string]$OutputPath = (Join-Path $PSScriptRoot '..\assets\qgsbackup.ico'))

Add-Type -AssemblyName System.Drawing
$bitmap = [Drawing.Bitmap]::new(256, 256, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([Drawing.Color]::Transparent)

$background = [Drawing.Drawing2D.GraphicsPath]::new()
$radius = 42
$background.AddArc(8, 8, $radius, $radius, 180, 90)
$background.AddArc(206, 8, $radius, $radius, 270, 90)
$background.AddArc(206, 206, $radius, $radius, 0, 90)
$background.AddArc(8, 206, $radius, $radius, 90, 90)
$background.CloseFigure()
$brush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 59, 145, 93))
$graphics.FillPath($brush, $background)

$font = [Drawing.Font]::new('Segoe UI', 125, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
$textBrush = [Drawing.SolidBrush]::new([Drawing.Color]::White)
$graphics.DrawString('Q', $font, $textBrush, 43, 25)

$pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 147, 231, 255), 18)
$pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
$graphics.DrawLine($pen, 126, 178, 182, 178)
$graphics.DrawLine($pen, 182, 178, 182, 136)
$arrow = [Drawing.Point[]]@([Drawing.Point]::new(150, 151), [Drawing.Point]::new(182, 181), [Drawing.Point]::new(214, 151))
$graphics.DrawLines($pen, $arrow)

$memory = [IO.MemoryStream]::new()
$bitmap.Save($memory, [Drawing.Imaging.ImageFormat]::Png)
$png = $memory.ToArray()
$stream = [IO.File]::Open($OutputPath, [IO.FileMode]::Create)
$writer = [IO.BinaryWriter]::new($stream)
$writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]1)
$writer.Write([Byte]0); $writer.Write([Byte]0); $writer.Write([Byte]0); $writer.Write([Byte]0)
$writer.Write([UInt16]1); $writer.Write([UInt16]32); $writer.Write([UInt32]$png.Length); $writer.Write([UInt32]22)
$writer.Write($png)
$writer.Dispose(); $stream.Dispose(); $memory.Dispose(); $pen.Dispose(); $font.Dispose(); $textBrush.Dispose(); $brush.Dispose(); $background.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
