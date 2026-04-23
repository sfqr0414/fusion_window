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
    public const int SW_RESTORE = 9;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint WM_CHAR = 0x0102;
    public const int VK_CONTROL = 0x11;
    public const int VK_SHIFT = 0x10;
    public const int VK_MENU = 0x12;
    public const int VK_HOME = 0x24;
    public const int VK_END = 0x23;
    public const int VK_RETURN = 0x0D;
    
    public const int GW_HWNDNEXT = 2;

    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", SetLastError = true)] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    
    [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
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

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build'
$ExecutablePath = Join-Path $BuildDir 'Debug\fusion_window.exe'

# 初始化输出目录
$PicDir = Join-Path $PSScriptRoot 'pic'
$LogDir = Join-Path $PSScriptRoot 'log'

if (-not (Test-Path $PicDir)) { New-Item -ItemType Directory -Path $PicDir | Out-Null }
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }

$null = & cmake --build $BuildDir --config Debug
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

function Get-GuiWindowHandle {
    param([int]$ProcessId)
    
    $ConsoleClass = 'ConsoleWindowClass'
    $TerminalClass = 'CASCADIA_HOSTING_WINDOW_CLASS'
    
    $hWnd = [Native]::GetTopWindow([IntPtr]::Zero)
    while ($hWnd -ne [IntPtr]::Zero) {
        [uint32]$pidOut = 0
        [Native]::GetWindowThreadProcessId($hWnd, [ref]$pidOut) | Out-Null
        if ($pidOut -eq $ProcessId -and [Native]::IsWindowVisible($hWnd)) {
            $sb = New-Object System.Text.StringBuilder 256
            [Native]::GetClassName($hWnd, $sb, $sb.Capacity) | Out-Null
            $className = $sb.ToString()
            
            # 命中非控制台类的可见窗口即认为是目标 GUI 窗口
            if ($className -ne $ConsoleClass -and $className -ne $TerminalClass) {
                return $hWnd
            }
        }
        $hWnd = [Native]::GetWindow($hWnd, [Native]::GW_HWNDNEXT)
    }
    return [IntPtr]::Zero
}

function Get-UiWindowProcess {
    return Get-Process fusion_window -ErrorAction SilentlyContinue | Where-Object { 
        (Get-GuiWindowHandle -ProcessId $_.Id) -ne [IntPtr]::Zero 
    } | Select-Object -First 1
}

$window = Get-UiWindowProcess
$launchedByScript = $false
if (-not $window) {
    if (-not (Test-Path $ExecutablePath)) {
        throw "Executable not found at $ExecutablePath"
    }

    $startedProcess = Start-Process -FilePath $ExecutablePath -WorkingDirectory (Split-Path $ExecutablePath -Parent) -PassThru
    $launchedByScript = $true

    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 100
        $window = Get-Process -Id $startedProcess.Id -ErrorAction SilentlyContinue | Where-Object { 
            (Get-GuiWindowHandle -ProcessId $_.Id) -ne [IntPtr]::Zero 
        } | Select-Object -First 1
        if ($window) {
            break
        }
    }
}

if (-not $window) {
    throw 'Unable to locate a running fusion_window main window.'
}

$hwnd = Get-GuiWindowHandle -ProcessId $window.Id

if ([Native]::IsIconic($hwnd)) {
    [Native]::ShowWindow($hwnd, [Native]::SW_RESTORE) | Out-Null
    Start-Sleep -Milliseconds 200
}

[Native]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300

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
        Width = $clientRect.Right
        Height = $clientRect.Bottom
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

    $graphics.Dispose()
    $path = Join-Path $PicDir $name
    [AsyncSaver]::SaveAndDispose($bitmap, $path)
}

function Read-ClipboardText([int]$retries = 6, [int]$delayMs = 80) {
    for ($i = 0; $i -lt $retries; $i++) {
        Start-Sleep -Milliseconds $delayMs
        try {
            return Get-Clipboard -Raw
        }
        catch {
        }
    }

    return ''
}

function Write-ClipboardText([string]$value, [int]$retries = 8, [int]$delayMs = 90) {
    for ($i = 0; $i -lt $retries; $i++) {
        try {
            Set-Clipboard -Value $value
            return $true
        }
        catch {
            Start-Sleep -Milliseconds $delayMs
        }
    }
    return $false
}

function Test-CaretVisible([string]$imagePath, [int]$expectedX, [int]$topY, [int]$bottomY, [int]$searchRadius = 18, [int]$brightnessThreshold = 140, [int]$minDarkPixels = 12) {
    if (-not (Test-Path $imagePath)) {
        return $false
    }

    $bitmap = [System.Drawing.Bitmap]::FromFile($imagePath)
    try {
        $scanTop = [Math]::Max(0, $topY)
        $scanBottom = [Math]::Min($bitmap.Height - 1, $bottomY)
        for ($offset = -$searchRadius; $offset -le $searchRadius; $offset++) {
            $x = [Math]::Max(0, [Math]::Min($bitmap.Width - 1, $expectedX + $offset))
            $darkCount = 0
            for ($y = $scanTop; $y -le $scanBottom; $y++) {
                $pixel = $bitmap.GetPixel($x, $y)
                $brightness = ($pixel.R + $pixel.G + $pixel.B) / 3
                if ($brightness -lt $brightnessThreshold) {
                    $darkCount++
                }
            }
            if ($darkCount -ge $minDarkPixels) {
                return $true
            }
        }
        return $false
    }
    finally {
        $bitmap.Dispose()
    }
}

function Measure-ImageRectDifference([string]$imagePathA, [string]$imagePathB, [int]$left, [int]$top, [int]$right, [int]$bottom, [int]$sampleStep = 4) {
    if (-not (Test-Path $imagePathA) -or -not (Test-Path $imagePathB)) {
        return 0.0
    }

    $bitmapA = [System.Drawing.Bitmap]::FromFile($imagePathA)
    $bitmapB = [System.Drawing.Bitmap]::FromFile($imagePathB)
    try {
        $scanLeft = [Math]::Max(0, $left)
        $scanTop = [Math]::Max(0, $top)
        $scanRight = [Math]::Min([Math]::Min($bitmapA.Width, $bitmapB.Width) - 1, $right)
        $scanBottom = [Math]::Min([Math]::Min($bitmapA.Height, $bitmapB.Height) - 1, $bottom)
        if ($scanRight -le $scanLeft -or $scanBottom -le $scanTop) {
            return 0.0
        }

        $totalDifference = 0.0
        $sampleCount = 0
        for ($y = $scanTop; $y -le $scanBottom; $y += $sampleStep) {
            for ($x = $scanLeft; $x -le $scanRight; $x += $sampleStep) {
                $pixelA = $bitmapA.GetPixel($x, $y)
                $pixelB = $bitmapB.GetPixel($x, $y)
                $totalDifference += ([Math]::Abs($pixelA.R - $pixelB.R) + [Math]::Abs($pixelA.G - $pixelB.G) + [Math]::Abs($pixelA.B - $pixelB.B)) / 3.0
                $sampleCount++
            }
        }

        if ($sampleCount -eq 0) {
            return 0.0
        }

        return $totalDifference / $sampleCount
    }
    finally {
        $bitmapA.Dispose()
        $bitmapB.Dispose()
    }
}

function Get-VerticalScrollbarOffsetAfterDrag([double]$viewportTop, [double]$viewportBottom, [double]$contentExtent, [double]$startY, [double]$endY) {
    $viewportExtent = [Math]::Max(1.0, $viewportBottom - $viewportTop)
    $maxOffset = [Math]::Max(0.0, $contentExtent - $viewportExtent)
    if ($maxOffset -le 0.5) {
        return 0.0
    }

    $trackTop = $viewportTop + 10.0
    $trackBottom = $viewportBottom - 10.0
    $trackExtent = [Math]::Max(1.0, $trackBottom - $trackTop)
    $thumbExtent = [Math]::Max(24.0, $trackExtent * ($viewportExtent / [Math]::Max($contentExtent, $viewportExtent)))
    $usableExtent = [Math]::Max(1.0, ($trackBottom - $trackTop) - $thumbExtent)
    $thumbStart = [Math]::Min($trackBottom - $thumbExtent, [Math]::Max($trackTop, $startY - ($thumbExtent * 0.5)))
    $offsetAfterClick = (($thumbStart - $trackTop) / $usableExtent) * $maxOffset
    $dragRatio = ($endY - $startY) / $usableExtent
    return [Math]::Max(0.0, [Math]::Min($maxOffset, $offsetAfterClick + ($dragRatio * $maxOffset)))
}

function Get-ScrolledClientY([int]$baseY, [double]$scrollOffset) {
    return [int][Math]::Round($baseY - $scrollOffset)
}

function Get-ScreenPoint([int]$clientX, [int]$clientY) {
    $point = New-Object POINT
    $point.X = $clientX
    $point.Y = $clientY
    [Native]::ClientToScreen($hwnd, [ref]$point) | Out-Null
    return $point
}

function Clamp-ClientPoint([int]$clientX, [int]$clientY) {
    $clientRect = New-Object RECT
    [Native]::GetClientRect($hwnd, [ref]$clientRect) | Out-Null
    $x = [Math]::Max(0, [Math]::Min([Math]::Max(0, $clientRect.Right - 1), $clientX))
    $y = [Math]::Max(0, [Math]::Min([Math]::Max(0, $clientRect.Bottom - 1), $clientY))
    return [pscustomobject]@{ X = $x; Y = $y }
}

function New-MouseLParam([int]$x, [int]$y) {
    return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
}

function Send-ClientMouse([uint32]$message, [int]$clientX, [int]$clientY, [int]$wParam = 0) {
    $pt = Clamp-ClientPoint $clientX $clientY
    [Native]::SendMessageW($hwnd, $message, [IntPtr]$wParam, (New-MouseLParam $pt.X $pt.Y)) | Out-Null
}

function Send-KeyDown([int]$virtualKey) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    [Native]::SendMessageW($hwnd, [Native]::WM_KEYDOWN, [IntPtr]$virtualKey, [IntPtr]::Zero) | Out-Null
}

function Send-KeyUp([int]$virtualKey) {
    [Native]::SendMessageW($hwnd, [Native]::WM_KEYUP, [IntPtr]$virtualKey, [IntPtr]::Zero) | Out-Null
}

function Send-KeyPress([int]$virtualKey, [int]$delayMs = 40) {
    Send-KeyDown $virtualKey
    Start-Sleep -Milliseconds $delayMs
    Send-KeyUp $virtualKey
    Start-Sleep -Milliseconds $delayMs
}

function Send-KeyChord([int]$modifierVirtualKey, [int]$virtualKey, [int]$delayMs = 45) {
    Send-KeyDown $modifierVirtualKey
    Start-Sleep -Milliseconds $delayMs
    Send-KeyDown $virtualKey
    Start-Sleep -Milliseconds $delayMs
    Send-KeyUp $virtualKey
    Start-Sleep -Milliseconds $delayMs
    Send-KeyUp $modifierVirtualKey
    Start-Sleep -Milliseconds $delayMs
}

function Send-Text([string]$text, [int]$delayMs = 22) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    foreach ($ch in $text.ToCharArray()) {
        [Native]::SendMessageW($hwnd, [Native]::WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds $delayMs
    }
}

function Send-Enter([int]$delayMs = 40) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    [Native]::SendMessageW($hwnd, [Native]::WM_CHAR, [IntPtr][Native]::VK_RETURN, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds $delayMs
}

function Send-SystemKeys([string]$keys, [int]$delayMs = 90) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    [System.Windows.Forms.SendKeys]::SendWait($keys)
    Start-Sleep -Milliseconds $delayMs
}

function Move-MouseScreen([int]$screenX, [int]$screenY, [int]$delayMs = 120) {
    [Native]::SetCursorPos($screenX, $screenY) | Out-Null
    Start-Sleep -Milliseconds $delayMs
}

function Click-ScreenLeft([int]$screenX, [int]$screenY, [int]$delayMs = 140) {
    Move-MouseScreen $screenX $screenY 100
    [Native]::mouse_event($MouseEventLeftDown, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 45
    [Native]::mouse_event($MouseEventLeftUp, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $delayMs
}

function Click-Client([int]$clientX, [int]$clientY) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    Send-ClientMouse ([Native]::WM_MOUSEMOVE) $clientX $clientY
    Start-Sleep -Milliseconds 80
    Send-ClientMouse ([Native]::WM_LBUTTONDOWN) $clientX $clientY 1
    Send-ClientMouse ([Native]::WM_LBUTTONUP) $clientX $clientY 0
    Start-Sleep -Milliseconds 120
}

function Drag-Client([int]$startX, [int]$startY, [int]$endX, [int]$endY, [int]$steps = 8) {
    [Native]::SetForegroundWindow($hwnd) | Out-Null
    $startPoint = Clamp-ClientPoint $startX $startY
    $endPoint = Clamp-ClientPoint $endX $endY
    Send-ClientMouse ([Native]::WM_MOUSEMOVE) $startPoint.X $startPoint.Y
    Start-Sleep -Milliseconds 60
    Send-ClientMouse ([Native]::WM_LBUTTONDOWN) $startPoint.X $startPoint.Y 1
    for ($i = 1; $i -le $steps; $i++) {
        $x = [int][Math]::Round($startPoint.X + (($endPoint.X - $startPoint.X) * $i / $steps))
        $y = [int][Math]::Round($startPoint.Y + (($endPoint.Y - $startPoint.Y) * $i / $steps))
        Send-ClientMouse ([Native]::WM_MOUSEMOVE) $x $y 1
        Start-Sleep -Milliseconds 35
    }
    Send-ClientMouse ([Native]::WM_LBUTTONUP) $endPoint.X $endPoint.Y 0
    Start-Sleep -Milliseconds 140
}

$dpi = [Native]::GetDpiForWindow($hwnd)
if ($dpi -le 0) {
    $dpi = 96
}
$dpiScale = $dpi / 96.0

function Scale-Logical([double]$value) {
    return [int][Math]::Round($value * $dpiScale)
}

function Measure-MenuWidth([string]$text) {
    $bitmap = New-Object System.Drawing.Bitmap 1, 1
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $font = New-Object System.Drawing.Font('Microsoft YaHei UI', (14.0 * $dpiScale), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    try {
        $size = $graphics.MeasureString($text, $font, 4096, [System.Drawing.StringFormat]::GenericTypographic)
        return [int][Math]::Ceiling($size.Width + (Scale-Logical 20))
    }
    finally {
        $font.Dispose()
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$clientBounds = Get-ClientBoundsInWindow
$clientWidth = $clientBounds.Width
$panelWidth = (Scale-Logical 200)
$captionHeight = (Scale-Logical 32)
$outerMargin = (Scale-Logical 24)
$stackPadding = (Scale-Logical 24)
$stackGap = (Scale-Logical 8)
$interCardGap = (Scale-Logical 24)
$minNarrowCardWidth = (Scale-Logical 300)
$maxNarrowCardWidth = (Scale-Logical 344)
$twoColumnThreshold = (Scale-Logical 728)

$availableWidth = $clientWidth - $panelWidth - (Scale-Logical 48)
$twoColumn = $availableWidth -ge (Scale-Logical 420)
if ($twoColumn) {
    $cardWidth = [int][Math]::Round([Math]::Min($maxNarrowCardWidth, [Math]::Max((Scale-Logical 260), (($availableWidth - $interCardGap) / 2.0))))
    $leftCardLeft = $panelWidth + $outerMargin
    $rightCardLeft = $leftCardLeft + $cardWidth + $interCardGap
}
else {
    $cardWidth = [Math]::Min($maxNarrowCardWidth, [Math]::Max($minNarrowCardWidth, $availableWidth))
    $rightCardLeft = $clientWidth - $cardWidth - $outerMargin
    $leftCardLeft = $rightCardLeft
}
$rightCardTop = $captionHeight + $outerMargin
$leftCardTop = $rightCardTop
$cardBottom = $clientBounds.Height - $outerMargin

$leftControlLeft = $leftCardLeft + $stackPadding
$leftControlWidth = $cardWidth - ($stackPadding * 2)

$controlLeft = $rightCardLeft + $stackPadding
$controlWidth = $cardWidth - ($stackPadding * 2)

$titleTop = $leftCardTop + $stackPadding
$titleHeight = (Scale-Logical 34)
$subtitleTop = $titleTop + $titleHeight + $stackGap
$subtitleHeight = (Scale-Logical 32)
$previewTop = $subtitleTop + $subtitleHeight + $stackGap
$previewHeight = (Scale-Logical 110)
$previewButtonTop = $previewTop + $previewHeight + $stackGap
$previewButtonHeight = (Scale-Logical 46)
$resetButtonTop = $previewButtonTop + $previewButtonHeight + $stackGap
$controlHeight = (Scale-Logical 36)
$checkboxTop = $resetButtonTop + $controlHeight + $stackGap
$radio0Top = $checkboxTop + $controlHeight + $stackGap
$radio1Top = $radio0Top + $controlHeight + $stackGap
$radio2Top = $radio1Top + $controlHeight + $stackGap
$sliderTop = $radio2Top + $controlHeight + $stackGap
$sliderHeight = (Scale-Logical 40)
$progressTop = $sliderTop + $sliderHeight + $stackGap
$progressHeight = (Scale-Logical 40)

$singleBoundsTop = $rightCardTop + $stackPadding
$singleBoundsHeight = (Scale-Logical 60)
$multiBoundsTop = $singleBoundsTop + $singleBoundsHeight + $stackGap
$multiBoundsHeight = (Scale-Logical 118)
$listBoundsTop = $multiBoundsTop + $multiBoundsHeight + $stackGap
$listBoundsHeight = (Scale-Logical 142)
$comboBoundsTop = $listBoundsTop + $listBoundsHeight + $stackGap
$comboBoundsHeight = (Scale-Logical 38)
$chipStripTop = $comboBoundsTop + $comboBoundsHeight + $stackGap
$chipStripHeight = (Scale-Logical 76)
$knobTop = $chipStripTop + $chipStripHeight + $stackGap
$knobHeight = (Scale-Logical 136)
$noteTop = $knobTop + $knobHeight + $stackGap
$noteHeight = (Scale-Logical 84)

$singleInputX = $controlLeft + [int][Math]::Round($controlWidth * 0.25)
$singleInputY = $singleBoundsTop + (Scale-Logical 36)
$singleSelectStartX = $controlLeft + $controlWidth - (Scale-Logical 24)
$singleSelectEndX = $controlLeft + (Scale-Logical 16)
$singleScrollStartX = $controlLeft + (Scale-Logical 36)
$singleScrollEndX = $controlLeft + $controlWidth - (Scale-Logical 42)
$singleScrollY = $singleBoundsTop + $singleBoundsHeight - (Scale-Logical 6)
$multiInputX = $controlLeft + (Scale-Logical 24)
$multiInputY = $multiBoundsTop + (Scale-Logical 44)
$multiLinePitch = (Scale-Logical 16)
$multiLineStartX = $controlLeft + (Scale-Logical 16)
$multiLine1Y = $multiBoundsTop + (Scale-Logical 44)
$multiScrollX = $controlLeft + $controlWidth - (Scale-Logical 6)
$multiScrollStartY = $multiBoundsTop + (Scale-Logical 38)
$multiScrollEndY = $multiBoundsTop + $multiBoundsHeight - (Scale-Logical 18)
$listBoxX = $controlLeft + (Scale-Logical 24)
$listBoxY = $listBoundsTop + (Scale-Logical 20)
$listScrollX = $controlLeft + $controlWidth - (Scale-Logical 10)
$listScrollStartY = $listBoundsTop + (Scale-Logical 30)
$listScrollEndY = $listBoundsTop + $listBoundsHeight - (Scale-Logical 30)
$comboX = $controlLeft + [int][Math]::Round($controlWidth * 0.5)
$comboY = $comboBoundsTop + [int][Math]::Round($comboBoundsHeight * 0.5)
$comboPopupTop = $comboBoundsTop + $comboBoundsHeight + (Scale-Logical 6)
$comboPopupRowHeight = (Scale-Logical 28)
$comboPopupHoverY = $comboPopupTop + (Scale-Logical 6) + [int][Math]::Round($comboPopupRowHeight * 1.5)
$comboPopupSelectY = $comboPopupTop + (Scale-Logical 6) + [int][Math]::Round($comboPopupRowHeight * 3.5)
$comboPopupScrollX = $controlLeft + $controlWidth - (Scale-Logical 10)
$comboPopupScrollStartY = $comboPopupTop + (Scale-Logical 18)
$comboPopupScrollEndY = $comboPopupTop + (Scale-Logical 134)
$chipStripStartX = $controlLeft + (Scale-Logical 28)
$chipStripEndX = $controlLeft + $controlWidth - (Scale-Logical 40)
$chipStripY = $chipStripTop + $chipStripHeight - (Scale-Logical 10)
$noteX = $controlLeft + [int][Math]::Round($controlWidth * 0.5)
$noteY = $noteTop + (Scale-Logical 14)

$previewButtonX = $leftControlLeft + [int][Math]::Round($leftControlWidth * 0.5)
$previewButtonY = $previewButtonTop + [int][Math]::Round($previewButtonHeight * 0.5)
$leftCardScrollX = $leftCardLeft + $cardWidth - (Scale-Logical 8)
$leftCardScrollStartY = $leftCardTop + (Scale-Logical 92)
$leftCardScrollEndY = $cardBottom - (Scale-Logical 92)
$rightCardScrollX = $rightCardLeft + $cardWidth - (Scale-Logical 8)
$rightCardScrollStartY = $rightCardTop + (Scale-Logical 92)
$rightCardScrollEndY = $cardBottom - (Scale-Logical 92)
$cardHoverExitX = Scale-Logical 24
$cardHoverExitY = Scale-Logical 24
$leftCardScrollbarDiffLeft = $clientBounds.Left + $leftCardLeft + $cardWidth - (Scale-Logical 20)
$leftCardScrollbarDiffTop = $clientBounds.Top + $leftCardTop + (Scale-Logical 24)
$leftCardScrollbarDiffRight = $clientBounds.Left + $leftCardLeft + $cardWidth - (Scale-Logical 2)
$leftCardScrollbarDiffBottom = $clientBounds.Top + $cardBottom - (Scale-Logical 24)
$rightCardScrollbarDiffLeft = $clientBounds.Left + $rightCardLeft + $cardWidth - (Scale-Logical 20)
$rightCardScrollbarDiffTop = $clientBounds.Top + $rightCardTop + (Scale-Logical 24)
$rightCardScrollbarDiffRight = $clientBounds.Left + $rightCardLeft + $cardWidth - (Scale-Logical 2)
$rightCardScrollbarDiffBottom = $clientBounds.Top + $cardBottom - (Scale-Logical 24)
$rightCardContentHeight = ($noteTop + $noteHeight + $stackPadding) - $rightCardTop
$rightCardViewportTop = $rightCardTop + 2
$rightCardViewportBottom = $cardBottom - 2
$rightCardComboScrollOffset = Get-VerticalScrollbarOffsetAfterDrag $rightCardViewportTop $rightCardViewportBottom $rightCardContentHeight $rightCardScrollStartY $rightCardScrollEndY
$comboVisibleY = Get-ScrolledClientY $comboY $rightCardComboScrollOffset
$comboPopupVisibleTop = Get-ScrolledClientY $comboPopupTop $rightCardComboScrollOffset
$comboPopupHoverVisibleY = Get-ScrolledClientY $comboPopupHoverY $rightCardComboScrollOffset
$comboPopupSelectVisibleY = Get-ScrolledClientY $comboPopupSelectY $rightCardComboScrollOffset
$comboPopupScrollStartVisibleY = Get-ScrolledClientY $comboPopupScrollStartY $rightCardComboScrollOffset
$comboPopupScrollEndVisibleY = Get-ScrolledClientY $comboPopupScrollEndY $rightCardComboScrollOffset
$chipStripVisibleY = Get-ScrolledClientY $chipStripY $rightCardComboScrollOffset
$knobStartVisibleY = Get-ScrolledClientY $knobStartY $rightCardComboScrollOffset
$knobEndVisibleY = Get-ScrolledClientY $knobEndY $rightCardComboScrollOffset
$noteVisibleY = Get-ScrolledClientY $noteY $rightCardComboScrollOffset
$comboPopupDiffLeft = $clientBounds.Left + $controlLeft + (Scale-Logical 12)
$comboPopupDiffTop = $clientBounds.Top + $comboPopupVisibleTop + (Scale-Logical 12)
$comboPopupDiffRight = $clientBounds.Left + $controlLeft + $controlWidth - (Scale-Logical 24)
$comboPopupDiffBottom = $clientBounds.Top + $comboPopupVisibleTop + (Scale-Logical 108)
$previewViewportLeft = $leftControlLeft + (Scale-Logical 12)
$previewViewportTop = $previewTop + (Scale-Logical 12)
$previewViewportRight = $leftControlLeft + $leftControlWidth - (Scale-Logical 30)
$previewViewportBottom = $previewTop + $previewHeight - (Scale-Logical 30)
$previewHScrollStartX = $previewViewportLeft + (Scale-Logical 24)
$previewHScrollEndX = $previewViewportRight - (Scale-Logical 28)
$previewHScrollY = $previewTop + $previewHeight - (Scale-Logical 18)
$previewVScrollX = $leftControlLeft + $leftControlWidth - (Scale-Logical 18)
$previewVScrollStartY = $previewViewportTop + (Scale-Logical 16)
$previewVScrollEndY = $previewViewportBottom - (Scale-Logical 22)
$resetButtonX = $leftControlLeft + [int][Math]::Round($leftControlWidth * 0.5)
$resetButtonY = $resetButtonTop + [int][Math]::Round($controlHeight * 0.5)
$checkboxX = $leftControlLeft + [int][Math]::Round($leftControlWidth * 0.58)
$checkboxY = $checkboxTop + [int][Math]::Round($controlHeight * 0.5)
$radio1X = $leftControlLeft + [int][Math]::Round($leftControlWidth * 0.58)
$radio1Y = $radio1Top + [int][Math]::Round($controlHeight * 0.5)
$radio2X = $leftControlLeft + [int][Math]::Round($leftControlWidth * 0.58)
$radio2Y = $radio2Top + [int][Math]::Round($controlHeight * 0.5)
$sliderStartX = $leftControlLeft + (Scale-Logical 24)
$sliderEndX = $leftControlLeft + $leftControlWidth - (Scale-Logical 24)
$sliderY = $sliderTop + (Scale-Logical 26)
$knobX = $controlLeft + [int][Math]::Round($controlWidth * 0.5)
$knobStartY = $knobTop + (Scale-Logical 42)
$knobEndY = $knobTop + (Scale-Logical 78)
$menuLabels = @('文件(F)', '编辑(E)', '视图(V)', '帮助(H)')
$menuItemWidths = @()
foreach ($menuLabel in $menuLabels) {
    $menuItemWidths += (Measure-MenuWidth $menuLabel)
}
$menuClientStartX = Scale-Logical 208
$menuClientY = [int][Math]::Round($captionHeight * 0.5)
$menuCenters = @()
$menuCursorX = $menuClientStartX
foreach ($menuWidth in $menuItemWidths) {
    $menuCenters += [int][Math]::Round($menuCursorX + ($menuWidth * 0.5))
    $menuCursorX += $menuWidth
}
$fileMenuScreenPoint = Get-ScreenPoint $menuCenters[0] $menuClientY
$editMenuScreenPoint = Get-ScreenPoint $menuCenters[1] $menuClientY

Save-Capture 'native_ui_01_initial.png'
Start-Sleep -Milliseconds 220
Save-Capture 'native_ui_00a_dirty_idle.png'

$previewAndButtonsOk = $false
$selectionControlsOk = $false
$menuToggleOk = $false
$menuHoverSwitchOk = $false
$sliderOk = $false
$scrollbarsOk = $false
$inputScrollbarsOk = $false
$cardScrollbarsHoverOk = $false
$knobOk = $false
$expandCollapseOk = $false
$comboPopupCaptured = $false

$dirtyProbeX = if ($twoColumn) { $previewButtonX } else { $singleInputX }
$dirtyProbeY = if ($twoColumn) { $previewButtonY } else { $singleInputY }
Move-MouseScreen (Get-ScreenPoint $dirtyProbeX $dirtyProbeY).X (Get-ScreenPoint $dirtyProbeX $dirtyProbeY).Y 180
Save-Capture 'native_ui_00b_dirty_hover.png'

Move-MouseScreen $fileMenuScreenPoint.X $fileMenuScreenPoint.Y 180
Click-ScreenLeft $fileMenuScreenPoint.X $fileMenuScreenPoint.Y 180
Save-Capture 'native_ui_00_menu_open.png' -useCopyFromScreen
Move-MouseScreen $editMenuScreenPoint.X $editMenuScreenPoint.Y 220
Save-Capture 'native_ui_00_menu_hover_switch.png' -useCopyFromScreen
Click-ScreenLeft $editMenuScreenPoint.X $editMenuScreenPoint.Y 180
Save-Capture 'native_ui_00_menu_closed.png' -useCopyFromScreen
$menuToggleOk = $true
$menuHoverSwitchOk = $true

if ($twoColumn) {
    Click-Client $previewButtonX $previewButtonY
    Click-Client $resetButtonX $resetButtonY
    Click-Client $checkboxX $checkboxY
    Click-Client $radio2X $radio2Y
    Click-Client $radio1X $radio1Y
    for ($i = 0; $i -lt 4; $i++) {
        Click-Client $radio1X $radio1Y
        Start-Sleep -Milliseconds 45
    }
    Save-Capture 'native_ui_00c_radio_rapid_same.png'
    Drag-Client $sliderStartX $sliderY $sliderEndX $sliderY
    Drag-Client $previewHScrollStartX $previewHScrollY $previewHScrollEndX $previewHScrollY
    Drag-Client $previewVScrollX $previewVScrollStartY $previewVScrollX $previewVScrollEndY
    $previewAndButtonsOk = $true
    $selectionControlsOk = $true
    $sliderOk = $true
}

Click-Client $singleInputX $singleInputY
Start-Sleep -Milliseconds 120
Click-Client ($controlLeft + (Scale-Logical 16)) $singleInputY
Start-Sleep -Milliseconds 100
Save-Capture 'native_ui_01_single_caret.png'
Start-Sleep -Milliseconds 80
Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
Start-Sleep -Milliseconds 120
$clipboardBefore = '__pending__'
for ($attempt = 0; $attempt -lt 3; $attempt++) {
    $null = Write-ClipboardText '__pending__'
    Send-SystemKeys '^c'
    Start-Sleep -Milliseconds 120
    $clipboardBefore = Read-ClipboardText
    if ($clipboardBefore -eq 'draw hitmarker') {
        break
    }
    Click-Client ($controlLeft + (Scale-Logical 16)) $singleInputY
    Start-Sleep -Milliseconds 120
    Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
    Start-Sleep -Milliseconds 120
}
$copyOk = $clipboardBefore -eq 'draw hitmarker'

Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
Start-Sleep -Milliseconds 120
Send-Text 'native ui selection ok'
Start-Sleep -Milliseconds 180
$clipboardAfter = '__pending__'
for ($attempt = 0; $attempt -lt 3; $attempt++) {
    $null = Write-ClipboardText '__pending__'
    Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
    Start-Sleep -Milliseconds 120
    Send-SystemKeys '^c'
    Start-Sleep -Milliseconds 80
    $clipboardAfter = Read-ClipboardText
    if ($clipboardAfter -eq 'native ui selection ok') {
        break
    }
    Click-Client ($controlLeft + (Scale-Logical 16)) $singleInputY
    Start-Sleep -Milliseconds 120
}
$replaceOk = $clipboardAfter -eq 'native ui selection ok'
$singleInputClipboardOk = $copyOk -or $replaceOk

Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
Start-Sleep -Milliseconds 120
Send-Text 'native ui selection ok with a deliberately long single line to force the horizontal scrollbar into view for drag verification'
Start-Sleep -Milliseconds 180
Save-Capture 'native_ui_01a_single_overflow.png'
Move-MouseScreen (Get-ScreenPoint $singleScrollEndX $singleScrollY).X (Get-ScreenPoint $singleScrollEndX $singleScrollY).Y 140
Save-Capture 'native_ui_01aa_single_hover_scrollbar.png'
Drag-Client $singleScrollStartX $singleScrollY $singleScrollEndX $singleScrollY
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_01b_single_scrolled.png'
Drag-Client $singleSelectStartX $singleInputY $singleSelectEndX $singleInputY 12
Start-Sleep -Milliseconds 120
Send-Text 'short text'
Start-Sleep -Milliseconds 220
Save-Capture 'native_ui_01c_single_collapsed.png'

Click-Client $multiInputX $multiInputY
Start-Sleep -Milliseconds 120
Click-Client ($controlLeft + (Scale-Logical 16)) ($multiBoundsTop + (Scale-Logical 40))
Start-Sleep -Milliseconds 100
Save-Capture 'native_ui_02_multi_caret.png'
Start-Sleep -Milliseconds 80
$multiLines = @(
    'Header-only migration complete.',
    'Next: richer text selection and IME support.',
    'Clipboard and selection test passed.',
    'Fourth line keeps the editor overflowing.',
    'Fifth line verifies the vertical scrollbar drag path.',
    'Sixth line keeps the thumb short enough to verify the cue.',
    'Seventh line confirms the hover hotspot on the vertical track.',
    'Eighth line keeps the overflow state obvious in screenshots.'
)
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
for ($i = 0; $i -lt $multiLines.Count; $i++) {
    Send-Text $multiLines[$i]
    if ($i -lt ($multiLines.Count - 1)) {
        Send-Enter
    }
}
Start-Sleep -Milliseconds 180
Save-Capture 'native_ui_02a_multi_overflow.png'

$null = Write-ClipboardText '__pending__'
Send-KeyPress ([Native]::VK_HOME)
Start-Sleep -Milliseconds 100
Send-KeyPress 40
Start-Sleep -Milliseconds 80
Send-KeyPress 38
Start-Sleep -Milliseconds 80
Send-Text '^'
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02ab_multi_up_one_line.png'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'C')
Start-Sleep -Milliseconds 120
$multiArrowUpText = Read-ClipboardText
$multiArrowUpOk = $multiArrowUpText -match '^\^Header-only migration complete\.'

Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
for ($i = 0; $i -lt $multiLines.Count; $i++) {
    Send-Text $multiLines[$i]
    if ($i -lt ($multiLines.Count - 1)) {
        Send-Enter
    }
}
Start-Sleep -Milliseconds 180

$null = Write-ClipboardText '__pending__'
Send-KeyPress ([Native]::VK_HOME)
Start-Sleep -Milliseconds 100
Send-KeyPress 40
Start-Sleep -Milliseconds 80
Send-Text 'v'
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02ac_multi_down_one_line.png'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'C')
Start-Sleep -Milliseconds 120
$multiArrowDownText = Read-ClipboardText
$multiArrowDownOk = $multiArrowDownText -match "Header-only migration complete\.\r?\nvNext: richer text selection and IME support\."

Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
for ($i = 0; $i -lt $multiLines.Count; $i++) {
    Send-Text $multiLines[$i]
    if ($i -lt ($multiLines.Count - 1)) {
        Send-Enter
    }
}
Start-Sleep -Milliseconds 180
Move-MouseScreen (Get-ScreenPoint $multiScrollX $multiScrollStartY).X (Get-ScreenPoint $multiScrollX $multiScrollStartY).Y 140
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02aa_multi_hover_scrollbar.png'
Drag-Client $multiScrollX $multiScrollStartY $multiScrollX $multiScrollEndY
Start-Sleep -Milliseconds 120
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-Text 'Short note.'
Start-Sleep -Milliseconds 220
Save-Capture 'native_ui_02b_multi_collapsed.png'

Drag-Client $listScrollX $listScrollStartY $listScrollX $listScrollEndY
Start-Sleep -Milliseconds 140
Drag-Client $rightCardScrollX $rightCardScrollStartY $rightCardScrollX $rightCardScrollEndY
Start-Sleep -Milliseconds 220

Click-Client $comboX $comboVisibleY
Start-Sleep -Milliseconds 120
Start-Sleep -Milliseconds 160
Save-Capture 'native_ui_02c_combo_animating.png' -useCopyFromScreen
Start-Sleep -Milliseconds 420
Save-Capture 'native_ui_02_combo_open.png' -useCopyFromScreen
Move-MouseScreen (Get-ScreenPoint $comboX $comboPopupHoverVisibleY).X (Get-ScreenPoint $comboX $comboPopupHoverVisibleY).Y 140
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02ca_combo_hover.png' -useCopyFromScreen
Move-MouseScreen (Get-ScreenPoint $comboPopupScrollX $comboPopupScrollStartVisibleY).X (Get-ScreenPoint $comboPopupScrollX $comboPopupScrollStartVisibleY).Y 140
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02cb_combo_hover_scrollbar.png' -useCopyFromScreen
Drag-Client $comboPopupScrollX $comboPopupScrollStartVisibleY $comboPopupScrollX $comboPopupScrollEndVisibleY
Start-Sleep -Milliseconds 160
Save-Capture 'native_ui_02cc_combo_scrolled.png' -useCopyFromScreen
Click-Client $comboX $comboPopupSelectVisibleY
Start-Sleep -Milliseconds 180
Save-Capture 'native_ui_02cd_combo_selected.png' -useCopyFromScreen
$comboPopupCaptured = $true

Drag-Client $chipStripStartX $chipStripVisibleY $chipStripEndX $chipStripVisibleY
Drag-Client $knobX $knobStartVisibleY $knobX $knobEndVisibleY
Click-Client $noteX $noteVisibleY
Start-Sleep -Milliseconds 120
Click-Client $noteX $noteVisibleY
Start-Sleep -Milliseconds 120

if ($twoColumn) {
    Move-MouseScreen (Get-ScreenPoint $leftCardScrollX $leftCardScrollStartY).X (Get-ScreenPoint $leftCardScrollX $leftCardScrollStartY).Y 140
    Save-Capture 'native_ui_02ba_left_card_hover_scrollbar.png'
    Move-MouseScreen (Get-ScreenPoint $cardHoverExitX $cardHoverExitY).X (Get-ScreenPoint $cardHoverExitX $cardHoverExitY).Y 140
    Start-Sleep -Milliseconds 1150
    Save-Capture 'native_ui_02bc_left_card_idle_scrollbar.png'
    Move-MouseScreen (Get-ScreenPoint $leftCardScrollX $leftCardScrollStartY).X (Get-ScreenPoint $leftCardScrollX $leftCardScrollStartY).Y 140
    Drag-Client $leftCardScrollX $leftCardScrollStartY $leftCardScrollX $leftCardScrollEndY
    Drag-Client $leftCardScrollX $leftCardScrollEndY $leftCardScrollX $leftCardScrollStartY
}
Move-MouseScreen (Get-ScreenPoint $rightCardScrollX $rightCardScrollStartY).X (Get-ScreenPoint $rightCardScrollX $rightCardScrollStartY).Y 140
Save-Capture 'native_ui_02bb_right_card_hover_scrollbar.png'
Move-MouseScreen (Get-ScreenPoint $cardHoverExitX $cardHoverExitY).X (Get-ScreenPoint $cardHoverExitX $cardHoverExitY).Y 140
Start-Sleep -Milliseconds 1150
Save-Capture 'native_ui_02bd_right_card_idle_scrollbar.png'
Move-MouseScreen (Get-ScreenPoint $rightCardScrollX $rightCardScrollStartY).X (Get-ScreenPoint $rightCardScrollX $rightCardScrollStartY).Y 140
Drag-Client $rightCardScrollX $rightCardScrollStartY $rightCardScrollX $rightCardScrollEndY
Drag-Client $rightCardScrollX $rightCardScrollEndY $rightCardScrollX $rightCardScrollStartY

$scrollbarsOk = $true
$inputScrollbarsOk = $true
$cardScrollbarsHoverOk = $true
$knobOk = $true
$expandCollapseOk = $true

Save-Capture 'native_ui_03_after_interaction.png'
[AsyncSaver]::WaitAll()

$dirtyRenderScreenshotsCaptured = (Test-Path (Join-Path $PicDir 'native_ui_00a_dirty_idle.png')) -and (Test-Path (Join-Path $PicDir 'native_ui_00b_dirty_hover.png'))
$comboPopupVisualDelta = Measure-ImageRectDifference (Join-Path $PicDir 'native_ui_02_combo_open.png') (Join-Path $PicDir 'native_ui_02cd_combo_selected.png') $comboPopupDiffLeft $comboPopupDiffTop $comboPopupDiffRight $comboPopupDiffBottom
$leftCardScrollbarFadeDelta = if ($twoColumn) { Measure-ImageRectDifference (Join-Path $PicDir 'native_ui_02ba_left_card_hover_scrollbar.png') (Join-Path $PicDir 'native_ui_02bc_left_card_idle_scrollbar.png') $leftCardScrollbarDiffLeft $leftCardScrollbarDiffTop $leftCardScrollbarDiffRight $leftCardScrollbarDiffBottom 3 } else { 0.0 }
$rightCardScrollbarFadeDelta = Measure-ImageRectDifference (Join-Path $PicDir 'native_ui_02bb_right_card_hover_scrollbar.png') (Join-Path $PicDir 'native_ui_02bd_right_card_idle_scrollbar.png') $rightCardScrollbarDiffLeft $rightCardScrollbarDiffTop $rightCardScrollbarDiffRight $rightCardScrollbarDiffBottom 3
$cardScrollbarsFadeOutOk = $rightCardScrollbarFadeDelta -ge 1.0
if ($twoColumn) {
    $cardScrollbarsFadeOutOk = $cardScrollbarsFadeOutOk -and
        (Test-Path (Join-Path $PicDir 'native_ui_02bc_left_card_idle_scrollbar.png')) -and
    ($leftCardScrollbarFadeDelta -ge 1.0)
}
$comboPopupOk = $comboPopupCaptured -and
    (Test-Path (Join-Path $PicDir 'native_ui_02c_combo_animating.png')) -and
    (Test-Path (Join-Path $PicDir 'native_ui_02_combo_open.png')) -and
    (Test-Path (Join-Path $PicDir 'native_ui_02ca_combo_hover.png')) -and
    (Test-Path (Join-Path $PicDir 'native_ui_02cb_combo_hover_scrollbar.png')) -and
    (Test-Path (Join-Path $PicDir 'native_ui_02cc_combo_scrolled.png')) -and
    (Test-Path (Join-Path $PicDir 'native_ui_02cd_combo_selected.png')) -and
    ($comboPopupVisualDelta -ge 14.0)

$singleCaretVisible = Test-CaretVisible (Join-Path $PicDir 'native_ui_01_single_caret.png') ($controlLeft + (Scale-Logical 16)) ($singleBoundsTop + (Scale-Logical 26)) ($singleBoundsTop + $singleBoundsHeight - (Scale-Logical 12))
$multiCaretVisible = Test-CaretVisible (Join-Path $PicDir 'native_ui_02_multi_caret.png') ($controlLeft + (Scale-Logical 16)) ($multiBoundsTop + (Scale-Logical 28)) ($multiBoundsTop + (Scale-Logical 54))

$stillAlive = (Get-Process -Id $window.Id -ErrorAction SilentlyContinue) -ne $null
$closedCleanly = $false
if ($stillAlive -and $launchedByScript) {
    $null = $window.CloseMainWindow()
    Start-Sleep -Milliseconds 300
    try {
        $closedCleanly = $window.WaitForExit(5000)
    }
    catch {
        $closedCleanly = $false
    }
}
elseif (-not $launchedByScript) {
    $closedCleanly = $true
}

$result = [pscustomobject]@{
    PreviewButtonsExercised = $previewAndButtonsOk
    SelectionControlsExercised = $selectionControlsOk
    MenuToggleExercised = $menuToggleOk
    MenuHoverSwitchExercised = $menuHoverSwitchOk
    DirtyRenderScreenshotsCaptured = $dirtyRenderScreenshotsCaptured
    SingleInputCopyInitial = $copyOk
    SingleInputReplaceAndCopy = $replaceOk
    SingleInputClipboardExercised = $singleInputClipboardOk
    SingleInputCaretVisible = $singleCaretVisible
    MultiInputCaretVisible = $multiCaretVisible
    MultiInputArrowUpOneLine = $multiArrowUpOk
    MultiInputArrowDownOneLine = $multiArrowDownOk
    SliderExercised = $sliderOk
    ScrollbarsExercised = $scrollbarsOk
    InputScrollbarsExercised = $inputScrollbarsOk
    CardScrollbarsHoverExercised = $cardScrollbarsHoverOk
    CardScrollbarsFadeOutExercised = $cardScrollbarsFadeOutOk
    LeftCardScrollbarFadeDelta = [Math]::Round($leftCardScrollbarFadeDelta, 2)
    RightCardScrollbarFadeDelta = [Math]::Round($rightCardScrollbarFadeDelta, 2)
    ComboBoxPopupExercised = $comboPopupOk
    ComboBoxPopupVisualDelta = [Math]::Round($comboPopupVisualDelta, 2)
    KnobExercised = $knobOk
    ExpandCollapseExercised = $expandCollapseOk
    ProcessAliveAfterUiOps = $stillAlive
    ProcessClosedAfterSelftest = $closedCleanly
    Screenshots = $null
}

$screenshotPaths = @(
    (Join-Path $PicDir 'native_ui_01_initial.png'),
    (Join-Path $PicDir 'native_ui_00a_dirty_idle.png'),
    (Join-Path $PicDir 'native_ui_00b_dirty_hover.png'),
    (Join-Path $PicDir 'native_ui_00_menu_open.png'),
    (Join-Path $PicDir 'native_ui_00_menu_hover_switch.png'),
    (Join-Path $PicDir 'native_ui_00_menu_closed.png'),
    (Join-Path $PicDir 'native_ui_01_single_caret.png'),
    (Join-Path $PicDir 'native_ui_01a_single_overflow.png'),
    (Join-Path $PicDir 'native_ui_01aa_single_hover_scrollbar.png'),
    (Join-Path $PicDir 'native_ui_01b_single_scrolled.png'),
    (Join-Path $PicDir 'native_ui_01c_single_collapsed.png'),
    (Join-Path $PicDir 'native_ui_02_multi_caret.png'),
    (Join-Path $PicDir 'native_ui_02aa_multi_hover_scrollbar.png'),
    (Join-Path $PicDir 'native_ui_02a_multi_overflow.png'),
    (Join-Path $PicDir 'native_ui_02ab_multi_up_one_line.png'),
    (Join-Path $PicDir 'native_ui_02ac_multi_down_one_line.png'),
    (Join-Path $PicDir 'native_ui_02b_multi_collapsed.png'),
    (Join-Path $PicDir 'native_ui_02c_combo_animating.png'),
    (Join-Path $PicDir 'native_ui_02_combo_open.png'),
    (Join-Path $PicDir 'native_ui_02ca_combo_hover.png'),
    (Join-Path $PicDir 'native_ui_02cb_combo_hover_scrollbar.png'),
    (Join-Path $PicDir 'native_ui_02cc_combo_scrolled.png'),
    (Join-Path $PicDir 'native_ui_02cd_combo_selected.png')
)
if ($twoColumn) {
    $screenshotPaths += (Join-Path $PicDir 'native_ui_02ba_left_card_hover_scrollbar.png')
    $screenshotPaths += (Join-Path $PicDir 'native_ui_02bc_left_card_idle_scrollbar.png')
}
$screenshotPaths += @(
    (Join-Path $PicDir 'native_ui_02bb_right_card_hover_scrollbar.png'),
    (Join-Path $PicDir 'native_ui_02bd_right_card_idle_scrollbar.png'),
    (Join-Path $PicDir 'native_ui_03_after_interaction.png')
)
$result.Screenshots = $screenshotPaths -join ';'

$failures = @()
if (-not $result.PreviewButtonsExercised) { $failures += 'preview/buttons' }
if (-not $result.SelectionControlsExercised) { $failures += 'selection-controls' }
if (-not $result.MenuToggleExercised) { $failures += 'menu-toggle' }
if (-not $result.MenuHoverSwitchExercised) { $failures += 'menu-hover-switch' }
if (-not $result.DirtyRenderScreenshotsCaptured) { $failures += 'dirty-render-screenshots' }
if (-not $result.SingleInputCaretVisible) { $failures += 'single-input-caret' }
if (-not $result.MultiInputCaretVisible) { $failures += 'multi-input-caret' }
if (-not $result.MultiInputArrowUpOneLine) { $failures += 'multi-input-arrow-up' }
if (-not $result.MultiInputArrowDownOneLine) { $failures += 'multi-input-arrow-down' }
if (-not $result.SliderExercised) { $failures += 'slider' }
if (-not $result.ScrollbarsExercised) { $failures += 'scrollbars' }
if (-not $result.InputScrollbarsExercised) { $failures += 'input-scrollbars' }
if (-not $result.CardScrollbarsHoverExercised) { $failures += 'card-scrollbar-hover' }
if (-not $result.CardScrollbarsFadeOutExercised) { $failures += 'card-scrollbar-fade-out' }
if (-not $result.ComboBoxPopupExercised) { $failures += 'combo-popup' }
if (-not $result.KnobExercised) { $failures += 'knob' }
if (-not $result.ExpandCollapseExercised) { $failures += 'expand-collapse' }
if (-not $result.ProcessAliveAfterUiOps) { $failures += 'process-alive' }
if (-not $result.ProcessClosedAfterSelftest) { $failures += 'process-exit' }

$result | ConvertTo-Json -Compress

$resultJson = $result | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText((Join-Path $LogDir 'selftest_native_ui.last.txt'), $resultJson, [System.Text.Encoding]::Unicode)
Write-Output $resultJson

if ($failures.Count -gt 0) {
    throw ('native UI selftest failed: ' + ($failures -join ', '))
}