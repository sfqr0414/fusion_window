Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type -ReferencedAssemblies "System.Drawing.dll","System.Windows.Forms.dll" -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

public struct POINT { public int X; public int Y; }
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

public static class Native {
    public const uint DWMWA_CAPTION_BUTTON_BOUNDS = 5;
    public const int SW_RESTORE = 9;

    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", SetLastError = true)] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool IsZoomed(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, uint dwAttribute, out RECT pvAttribute, int cbAttribute);
}

public static class AsyncSaver {
    private static List<Task> _tasks = new List<Task>();

    public static void SaveAndDispose(Bitmap bmp, string path) {
        Task t = Task.Run(() => {
            try {
                bmp.Save(path, ImageFormat.Png);
            } 
            catch { }
            finally {
                bmp.Dispose();
            }
        });
        lock(_tasks) {
            _tasks.Add(t);
        }
    }

    public static void WaitAll() {
        Task[] tasksToWait;
        lock(_tasks) {
            tasksToWait = _tasks.ToArray();
        }
        Task.WaitAll(tasksToWait);
    }
}
"@

[Native]::SetProcessDPIAware() | Out-Null

$MouseEventLeftDown = 0x0002
$MouseEventLeftUp = 0x0004
$OutputDir = 'D:\Repo\fusion_window\artifacts'

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

$window = Get-Process fusion_window -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$hwnd = [IntPtr]$window.MainWindowHandle
[Native]::SetForegroundWindow($hwnd) | Out-Null

function Get-Rect {
    $rect = New-Object RECT
    [Native]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    return $rect
}

function Get-ClientBoundsInWindow {
    $windowRect = Get-Rect
    $clientRect = New-Object RECT
    [Native]::GetClientRect($hwnd, [ref]$clientRect) | Out-Null

    $clientOrigin = New-Object POINT
    $clientOrigin.X = 0
    $clientOrigin.Y = 0
    [Native]::ClientToScreen($hwnd, [ref]$clientOrigin) | Out-Null

    $left = $clientOrigin.X - $windowRect.Left
    $top = $clientOrigin.Y - $windowRect.Top

    return [pscustomobject]@{
        Left = $left
        Top = $top
        Right = $left + $clientRect.Right
        Bottom = $top + $clientRect.Bottom
    }
}

function Get-CaptionButtonInfo {
    $windowRect = Get-Rect
    $buttonRect = New-Object RECT
    $hr = [Native]::DwmGetWindowAttribute($hwnd, [Native]::DWMWA_CAPTION_BUTTON_BOUNDS, [ref]$buttonRect, 16)

    if ($hr -ne 0 -or ($buttonRect.Right - $buttonRect.Left) -le 0) {
        return [pscustomobject]@{
            Bounds = 'fallback'
            Minimize = @{ X = $windowRect.Right - 140; Y = $windowRect.Top + 20 }
            Maximize = @{ X = $windowRect.Right - 95; Y = $windowRect.Top + 20 }
            Close = @{ X = $windowRect.Right - 45; Y = $windowRect.Top + 20 }
        }
    }

    $groupWidth = $buttonRect.Right - $buttonRect.Left
    $segmentWidth = [math]::Max(1, [math]::Round($groupWidth / 3.0))
    $centerY = $windowRect.Top + $buttonRect.Top + [math]::Round(($buttonRect.Bottom - $buttonRect.Top) / 2.0)

    return [pscustomobject]@{
        Bounds = "$($buttonRect.Left),$($buttonRect.Top),$($buttonRect.Right),$($buttonRect.Bottom)"
        Minimize = @{ X = $windowRect.Left + $buttonRect.Left + [math]::Round($segmentWidth * 0.5); Y = $centerY }
        Maximize = @{ X = $windowRect.Left + $buttonRect.Left + [math]::Round($segmentWidth * 1.5); Y = $centerY }
        Close = @{ X = $windowRect.Left + $buttonRect.Left + [math]::Round($segmentWidth * 2.5); Y = $centerY }
    }
}

function Parse-Bounds([string]$boundsText) {
    if ([string]::IsNullOrWhiteSpace($boundsText) -or $boundsText -eq 'fallback') {
        return $null
    }

    $parts = $boundsText.Split(',')
    if ($parts.Length -ne 4) {
        return $null
    }

    return [pscustomobject]@{
        Left = [int]$parts[0]
        Top = [int]$parts[1]
        Right = [int]$parts[2]
        Bottom = [int]$parts[3]
    }
}

function Save-Capture([string]$name, [switch]$useCopyFromScreen) {
    $rect = Get-Rect
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top

    if ($width -le 0 -or $height -le 0) { return }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    if ($useCopyFromScreen) {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size, [System.Drawing.CopyPixelOperation]::SourceCopy)
    } else {
        $hdc = $graphics.GetHdc()
        [Native]::PrintWindow($hwnd, $hdc, 2) | Out-Null  
        $graphics.ReleaseHdc($hdc)
    }
    
    # 手术刀修改 2：主线程拍完照立刻释放 graphics，然后把 Bitmap 丢给后台线程去保存
    $graphics.Dispose()
    $path = Join-Path $OutputDir $name
    [AsyncSaver]::SaveAndDispose($bitmap, $path) 
}

function Get-ColorDistance($a, $b) {
    return [math]::Abs([int]$a.R - [int]$b.R) + [math]::Abs([int]$a.G - [int]$b.G) + [math]::Abs([int]$a.B - [int]$b.B)
}

function Test-CloseEdgeRedFill([string]$imagePath, [string]$boundsText) {
    $bounds = Parse-Bounds $boundsText
    if ($null -eq $bounds) {
        return $false
    }

    $bitmap = [System.Drawing.Bitmap]::FromFile($imagePath)
    try {
        $closeWidth = [math]::Max(1, [math]::Round(($bounds.Right - $bounds.Left) / 3.0))
        $closeLeft = $bounds.Left + ($closeWidth * 2)
        $sampleX = [math]::Min($bitmap.Width - 1, [math]::Max($closeLeft, $bounds.Right - 2))
        $redHits = 0
        $samples = 0

        for ($sampleY = $bounds.Top + 6; $sampleY -le $bounds.Bottom - 6; $sampleY += 6) {
            $pixel = $bitmap.GetPixel($sampleX, [math]::Min($bitmap.Height - 1, $sampleY))
            if ($pixel.R -ge 150 -and ($pixel.R - [math]::Max($pixel.G, $pixel.B)) -ge 60) {
                $redHits++
            }
            $samples++
        }

        return $samples -gt 0 -and $redHits -ge [math]::Ceiling($samples * 0.6)
    }
    finally {
        $bitmap.Dispose()
    }
}

function Test-VerticalEdgeSeamless([string]$imagePath, [int]$edgeX, [int]$referenceX, [int]$startY, [int]$endY, [int]$threshold = 45) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($imagePath)
    try {
        if ($edgeX -lt 0) { $edgeX = $bitmap.Width + $edgeX }
        if ($referenceX -lt 0) { $referenceX = $bitmap.Width + $referenceX }
        $edgeX = [math]::Max(0, [math]::Min($bitmap.Width - 1, $edgeX))
        $referenceX = [math]::Max(0, [math]::Min($bitmap.Width - 1, $referenceX))
        $passCount = 0
        $samples = 0

        for ($y = $startY; $y -le $endY; $y += 14) {
            $sampleY = [math]::Max(0, [math]::Min($bitmap.Height - 1, $y))
            $edgeColor = $bitmap.GetPixel($edgeX, $sampleY)
            $referenceColor = $bitmap.GetPixel($referenceX, $sampleY)
            if ((Get-ColorDistance $edgeColor $referenceColor) -le $threshold) {
                $passCount++
            }
            $samples++
        }

        return $samples -gt 0 -and $passCount -ge [math]::Ceiling($samples * 0.7)
    }
    finally {
        $bitmap.Dispose()
    }
}

function Test-HorizontalEdgeSeamless([string]$imagePath, [int]$edgeY, [int]$referenceY, [int]$startX, [int]$endX, [int]$threshold = 45) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($imagePath)
    try {
        if ($edgeY -lt 0) { $edgeY = $bitmap.Height + $edgeY }
        if ($referenceY -lt 0) { $referenceY = $bitmap.Height + $referenceY }
        $edgeY = [math]::Max(0, [math]::Min($bitmap.Height - 1, $edgeY))
        $referenceY = [math]::Max(0, [math]::Min($bitmap.Height - 1, $referenceY))
        $passCount = 0
        $samples = 0

        for ($x = $startX; $x -le $endX; $x += 18) {
            $sampleX = [math]::Max(0, [math]::Min($bitmap.Width - 1, $x))
            $edgeColor = $bitmap.GetPixel($sampleX, $edgeY)
            $referenceColor = $bitmap.GetPixel($sampleX, $referenceY)
            if ((Get-ColorDistance $edgeColor $referenceColor) -le $threshold) {
                $passCount++
            }
            $samples++
        }

        return $samples -gt 0 -and $passCount -ge [math]::Ceiling($samples * 0.7)
    }
    finally {
        $bitmap.Dispose()
    }
}

function Move-Mouse([int]$x, [int]$y) {
    [Native]::SetCursorPos($x, $y) | Out-Null
    [System.Windows.Forms.Application]::DoEvents()
}

function Flush-Ui([int]$count = 120) {
    for ($i = 0; $i -lt $count; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
    }
}

function Click-Left([int]$x, [int]$y) {
    Move-Mouse $x $y
    [Native]::mouse_event($MouseEventLeftDown, 0, 0, 0, [UIntPtr]::Zero)
    Flush-Ui 30
    [Native]::mouse_event($MouseEventLeftUp, 0, 0, 0, [UIntPtr]::Zero)
    Flush-Ui 60
}

function Drag-Left([int]$fromX, [int]$fromY, [int]$toX, [int]$toY, [string]$captureName, [bool]$captureMidway = $false) {
    Move-Mouse $fromX $fromY
    Start-Sleep -Milliseconds 50

    [Native]::mouse_event($MouseEventLeftDown, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 50

    $steps = 20
    for ($i = 1; $i -le $steps; $i++) {
        $currentX = [math]::Round($fromX + (($toX - $fromX) * $i / $steps))
        $currentY = [math]::Round($fromY + (($toY - $fromY) * $i / $steps))
        [Native]::SetCursorPos($currentX, $currentY) | Out-Null

        if ($captureMidway -and $i -eq 10) {
            Save-Capture $captureName
        }

        Start-Sleep -Milliseconds 1
    }

    Start-Sleep -Milliseconds 200

    if (-not $captureMidway) {
        Save-Capture $captureName
    }

    [Native]::mouse_event($MouseEventLeftUp, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 100
}

$captionY = 18
$menuX = 420
$dragTitleX = 960

$buttonsBeforeMax = Get-CaptionButtonInfo
Move-Mouse $buttonsBeforeMax.Close.X $buttonsBeforeMax.Close.Y
Start-Sleep -Milliseconds 150
Save-Capture '01_startup.png'

$rect = Get-Rect
Move-Mouse ($rect.Left + $menuX) ($rect.Top + $captionY)
Start-Sleep -Milliseconds 200
Save-Capture '02_menu_hover.png'

Click-Left ($rect.Left + $menuX) ($rect.Top + $captionY)
Start-Sleep -Milliseconds 150
Save-Capture '03_menu_popup.png' -useCopyFromScreen

Click-Left $buttonsBeforeMax.Minimize.X $buttonsBeforeMax.Minimize.Y
Start-Sleep -Milliseconds 250
$minimized = [Native]::IsIconic($hwnd)
$restoredFromMinimize = $false
if ($minimized) {
    [Native]::ShowWindow($hwnd, [Native]::SW_RESTORE) | Out-Null
    Start-Sleep -Milliseconds 250
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    $restoredFromMinimize = -not [Native]::IsIconic($hwnd)
}

$buttonsBeforeMax = Get-CaptionButtonInfo
Click-Left $buttonsBeforeMax.Maximize.X $buttonsBeforeMax.Maximize.Y
Start-Sleep -Milliseconds 300
$maximized = [Native]::IsZoomed($hwnd)
$buttonsAfterMax = Get-CaptionButtonInfo
Save-Capture '04_maximized.png'

[Native]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 80
$hoverApproachX = [math]::Max($buttonsAfterMax.Close.X - 96, $buttonsAfterMax.Maximize.X + 12)
Move-Mouse $hoverApproachX $buttonsAfterMax.Close.Y
Start-Sleep -Milliseconds 80
Move-Mouse ($buttonsAfterMax.Close.X - 8) $buttonsAfterMax.Close.Y
Start-Sleep -Milliseconds 80
Move-Mouse $buttonsAfterMax.Close.X $buttonsAfterMax.Close.Y
Start-Sleep -Milliseconds 300
Save-Capture '07_maximized_close_hover.png'

Click-Left $buttonsAfterMax.Maximize.X $buttonsAfterMax.Maximize.Y
Start-Sleep -Milliseconds 150
$restored = -not [Native]::IsZoomed($hwnd)

$beforeDrag = Get-Rect
Drag-Left ($beforeDrag.Left + $dragTitleX) ($beforeDrag.Top + $captionY) ($beforeDrag.Left + $dragTitleX + 120) ($beforeDrag.Top + $captionY + 60) '05_title_drag.png'
$afterDrag = Get-Rect
$titleMoved = ($afterDrag.Left -ne $beforeDrag.Left) -or ($afterDrag.Top -ne $beforeDrag.Top)

$beforeResize = Get-Rect
$midY = [int](($beforeResize.Top + $beforeResize.Bottom) / 2)
Drag-Left ($beforeResize.Right - 2) $midY ($beforeResize.Right + 120) $midY '06_resize_drag.png' $true
$afterResize = Get-Rect
$resized = ($afterResize.Right - $afterResize.Left) -gt ($beforeResize.Right - $beforeResize.Left)

$restoredCloseRed = Test-CloseEdgeRedFill (Join-Path $OutputDir '01_startup.png') $buttonsBeforeMax.Bounds
$maximizedCloseRed = Test-CloseEdgeRedFill (Join-Path $OutputDir '07_maximized_close_hover.png') $buttonsAfterMax.Bounds
$clientBounds = Get-ClientBoundsInWindow
$contentStartY = [math]::Max($clientBounds.Top + 8, 70)
$contentEndY = [math]::Max($contentStartY, $clientBounds.Bottom - 16)
$contentStartX = [math]::Max($clientBounds.Left + 24, 24)
$contentEndX = [math]::Max($contentStartX, $clientBounds.Right - 24)
$maximizedLeftEdgeSeamless = Test-VerticalEdgeSeamless (Join-Path $OutputDir '04_maximized.png') $clientBounds.Left ($clientBounds.Left + 6) $contentStartY $contentEndY
$maximizedRightEdgeSeamless = Test-VerticalEdgeSeamless (Join-Path $OutputDir '04_maximized.png') ($clientBounds.Right - 1) ($clientBounds.Right - 7) $contentStartY $contentEndY
$maximizedBottomEdgeSeamless = Test-HorizontalEdgeSeamless (Join-Path $OutputDir '04_maximized.png') ($clientBounds.Bottom - 1) ($clientBounds.Bottom - 7) $contentStartX $contentEndX

[pscustomobject]@{
    ButtonBoundsBeforeMax = $buttonsBeforeMax.Bounds
    ButtonBoundsAfterMax = $buttonsAfterMax.Bounds
    CloseHoverRightEdgeRed = $restoredCloseRed
    MaximizedCloseHoverRightEdgeRed = $maximizedCloseRed
    MaximizedLeftEdgeSeamless = $maximizedLeftEdgeSeamless
    MaximizedRightEdgeSeamless = $maximizedRightEdgeSeamless
    MaximizedBottomEdgeSeamless = $maximizedBottomEdgeSeamless
    MinimizedByButton = $minimized
    RestoredFromMinimize = $restoredFromMinimize
    MaximizedByButton = $maximized
    RestoredByButton = $restored
    TitleDragMovedWindow = $titleMoved
    EdgeResizeChangedWidth = $resized
    Screenshots = @(
        (Join-Path $OutputDir '01_startup.png'),
        (Join-Path $OutputDir '02_menu_hover.png'),
        (Join-Path $OutputDir '03_menu_popup.png'),
        (Join-Path $OutputDir '04_maximized.png'),
        (Join-Path $OutputDir '05_title_drag.png'),
        (Join-Path $OutputDir '06_resize_drag.png'),
        (Join-Path $OutputDir '07_maximized_close_hover.png')
    ) -join ';'
} | Format-List | Out-String | Write-Output

Write-Host "Waiting for background capture tasks to finish..." -ForegroundColor Yellow
[AsyncSaver]::WaitAll()

Write-Host "Testing and screenshots completed, closing the target program..." -ForegroundColor Green
Stop-Process -Id $window.Id -Force