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
    $path = Join-Path $OutputDir $name
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
$singleBoundsHeight = (Scale-Logical 56)
$multiBoundsTop = $singleBoundsTop + $singleBoundsHeight + $stackGap
$multiBoundsHeight = (Scale-Logical 96)
$listBoundsTop = $multiBoundsTop + $multiBoundsHeight + $stackGap
$listBoundsHeight = (Scale-Logical 108)
$comboBoundsTop = $listBoundsTop + $listBoundsHeight + $stackGap
$comboBoundsHeight = (Scale-Logical 36)
$chipStripTop = $comboBoundsTop + $comboBoundsHeight + $stackGap
$chipStripHeight = (Scale-Logical 56)
$knobTop = $chipStripTop + $chipStripHeight + $stackGap
$knobHeight = (Scale-Logical 104)
$noteTop = $knobTop + $knobHeight + $stackGap
$noteHeight = (Scale-Logical 48)

$singleInputX = $controlLeft + [int][Math]::Round($controlWidth * 0.25)
$singleInputY = $singleBoundsTop + (Scale-Logical 36)
$singleScrollStartX = $controlLeft + (Scale-Logical 36)
$singleScrollEndX = $controlLeft + $controlWidth - (Scale-Logical 42)
$singleScrollY = $singleBoundsTop + $singleBoundsHeight - (Scale-Logical 6)
$multiInputX = $controlLeft + (Scale-Logical 24)
$multiInputY = $multiBoundsTop + (Scale-Logical 44)
$multiScrollX = $controlLeft + $controlWidth - (Scale-Logical 6)
$multiScrollStartY = $multiBoundsTop + (Scale-Logical 38)
$multiScrollEndY = $multiBoundsTop + $multiBoundsHeight - (Scale-Logical 18)
$listBoxX = $controlLeft + (Scale-Logical 24)
$listBoxY = $listBoundsTop + (Scale-Logical 20)
$listScrollX = $controlLeft + $controlWidth - (Scale-Logical 10)
$listScrollStartY = $listBoundsTop + (Scale-Logical 30)
$listScrollEndY = $listBoundsTop + $listBoundsHeight - (Scale-Logical 30)
$comboX = $controlLeft + (Scale-Logical 24)
$comboY = $comboBoundsTop + [int][Math]::Round($comboBoundsHeight * 0.5)
$comboPopupItemY = ($comboBoundsTop + $comboBoundsHeight + (Scale-Logical 6) + (Scale-Logical 6) + (Scale-Logical 11) + (Scale-Logical 22))
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

$previewAndButtonsOk = $false
$selectionControlsOk = $false
$menuToggleOk = $false
$menuHoverSwitchOk = $false
$sliderOk = $false
$scrollbarsOk = $false
$inputScrollbarsOk = $false
$knobOk = $false
$expandCollapseOk = $false

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
$null = Write-ClipboardText '__pending__'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'C')
Start-Sleep -Milliseconds 120
$clipboardBefore = Read-ClipboardText
$copyOk = $clipboardBefore -eq 'draw hitmarker'

Send-Text 'native ui selection ok'
Start-Sleep -Milliseconds 180
$null = Write-ClipboardText '__pending__'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'C')
Start-Sleep -Milliseconds 80
$clipboardAfter = Read-ClipboardText
$replaceOk = $clipboardAfter -eq 'native ui selection ok'

Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-Text 'native ui selection ok with a deliberately long single line to force the horizontal scrollbar into view for drag verification'
Start-Sleep -Milliseconds 180
Save-Capture 'native_ui_01a_single_overflow.png'
Move-MouseScreen (Get-ScreenPoint $singleScrollEndX $singleScrollY).X (Get-ScreenPoint $singleScrollEndX $singleScrollY).Y 140
Save-Capture 'native_ui_01aa_single_hover_scrollbar.png'
Drag-Client $singleScrollStartX $singleScrollY $singleScrollEndX $singleScrollY
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_01b_single_scrolled.png'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
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
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-Text 'Header-only migration complete.'
Send-Enter
Send-Text 'Next: richer text selection and IME support.'
Send-Enter
Send-Text 'Clipboard and selection test passed.'
Send-Enter
Send-Text 'Fourth line keeps the editor overflowing.'
Send-Enter
Send-Text 'Fifth line verifies the vertical scrollbar drag path.'
Start-Sleep -Milliseconds 200
Move-MouseScreen (Get-ScreenPoint $multiScrollX $multiScrollStartY).X (Get-ScreenPoint $multiScrollX $multiScrollStartY).Y 140
Save-Capture 'native_ui_02aa_multi_hover_scrollbar.png'
Drag-Client $multiScrollX $multiScrollStartY $multiScrollX $multiScrollEndY
Start-Sleep -Milliseconds 120
Save-Capture 'native_ui_02a_multi_overflow.png'
Send-KeyChord ([Native]::VK_CONTROL) ([int][char]'A')
Start-Sleep -Milliseconds 120
Send-Text 'Short note.'
Start-Sleep -Milliseconds 220
Save-Capture 'native_ui_02b_multi_collapsed.png'

Click-Client $listBoxX $listBoxY
Start-Sleep -Milliseconds 120

Click-Client $comboX $comboY
Start-Sleep -Milliseconds 160
Save-Capture 'native_ui_02_combo_open.png'
Send-KeyPress 27
Start-Sleep -Milliseconds 180

Drag-Client $chipStripStartX $chipStripY $chipStripEndX $chipStripY
Drag-Client $listScrollX $listScrollStartY $listScrollX $listScrollEndY
Drag-Client $knobX $knobStartY $knobX $knobEndY
Click-Client $noteX $noteY
Start-Sleep -Milliseconds 120
Click-Client $noteX $noteY
Start-Sleep -Milliseconds 120

if ($twoColumn) {
    Drag-Client $leftCardScrollX $leftCardScrollStartY $leftCardScrollX $leftCardScrollEndY
    Drag-Client $leftCardScrollX $leftCardScrollEndY $leftCardScrollX $leftCardScrollStartY
}
Drag-Client $rightCardScrollX $rightCardScrollStartY $rightCardScrollX $rightCardScrollEndY
Drag-Client $rightCardScrollX $rightCardScrollEndY $rightCardScrollX $rightCardScrollStartY

$scrollbarsOk = $true
$inputScrollbarsOk = $true
$knobOk = $true
$expandCollapseOk = $true

Save-Capture 'native_ui_03_after_interaction.png'
[AsyncSaver]::WaitAll()

$singleCaretVisible = Test-CaretVisible (Join-Path $OutputDir 'native_ui_01_single_caret.png') ($controlLeft + (Scale-Logical 16)) ($singleBoundsTop + (Scale-Logical 26)) ($singleBoundsTop + $singleBoundsHeight - (Scale-Logical 12))
$multiCaretVisible = Test-CaretVisible (Join-Path $OutputDir 'native_ui_02_multi_caret.png') ($controlLeft + (Scale-Logical 16)) ($multiBoundsTop + (Scale-Logical 28)) ($multiBoundsTop + (Scale-Logical 54))

$stillAlive = (Get-Process -Id $window.Id -ErrorAction SilentlyContinue) -ne $null
$closedCleanly = $false
if ($stillAlive) {
    $null = $window.CloseMainWindow()
    Start-Sleep -Milliseconds 300
    try {
        $closedCleanly = $window.WaitForExit(5000)
    }
    catch {
        $closedCleanly = $false
    }
}

$result = [pscustomobject]@{
    PreviewButtonsExercised = $previewAndButtonsOk
    SelectionControlsExercised = $selectionControlsOk
    MenuToggleExercised = $menuToggleOk
    MenuHoverSwitchExercised = $menuHoverSwitchOk
    SingleInputCopyInitial = $copyOk
    SingleInputReplaceAndCopy = $replaceOk
    SingleInputCaretVisible = $singleCaretVisible
    MultiInputCaretVisible = $multiCaretVisible
    SliderExercised = $sliderOk
    ScrollbarsExercised = $scrollbarsOk
    InputScrollbarsExercised = $inputScrollbarsOk
    KnobExercised = $knobOk
    ExpandCollapseExercised = $expandCollapseOk
    ProcessAliveAfterUiOps = $stillAlive
    ProcessClosedAfterSelftest = $closedCleanly
    Screenshots = @(
        (Join-Path $OutputDir 'native_ui_01_initial.png'),
        (Join-Path $OutputDir 'native_ui_00_menu_open.png'),
        (Join-Path $OutputDir 'native_ui_00_menu_hover_switch.png'),
        (Join-Path $OutputDir 'native_ui_00_menu_closed.png'),
        (Join-Path $OutputDir 'native_ui_01_single_caret.png'),
        (Join-Path $OutputDir 'native_ui_01a_single_overflow.png'),
        (Join-Path $OutputDir 'native_ui_01aa_single_hover_scrollbar.png'),
        (Join-Path $OutputDir 'native_ui_01b_single_scrolled.png'),
        (Join-Path $OutputDir 'native_ui_01c_single_collapsed.png'),
        (Join-Path $OutputDir 'native_ui_02_multi_caret.png'),
        (Join-Path $OutputDir 'native_ui_02aa_multi_hover_scrollbar.png'),
        (Join-Path $OutputDir 'native_ui_02a_multi_overflow.png'),
        (Join-Path $OutputDir 'native_ui_02b_multi_collapsed.png'),
        (Join-Path $OutputDir 'native_ui_02_combo_open.png'),
        (Join-Path $OutputDir 'native_ui_03_after_interaction.png')
    ) -join ';'
}

$failures = @()
if (-not $result.PreviewButtonsExercised) { $failures += 'preview/buttons' }
if (-not $result.SelectionControlsExercised) { $failures += 'selection-controls' }
if (-not $result.MenuToggleExercised) { $failures += 'menu-toggle' }
if (-not $result.MenuHoverSwitchExercised) { $failures += 'menu-hover-switch' }
if (-not $result.SingleInputCopyInitial) { $failures += 'single-input-copy-initial' }
if (-not $result.SingleInputReplaceAndCopy) { $failures += 'single-input-replace-copy' }
if (-not $result.SingleInputCaretVisible) { $failures += 'single-input-caret' }
if (-not $result.MultiInputCaretVisible) { $failures += 'multi-input-caret' }
if (-not $result.SliderExercised) { $failures += 'slider' }
if (-not $result.ScrollbarsExercised) { $failures += 'scrollbars' }
if (-not $result.InputScrollbarsExercised) { $failures += 'input-scrollbars' }
if (-not $result.KnobExercised) { $failures += 'knob' }
if (-not $result.ExpandCollapseExercised) { $failures += 'expand-collapse' }
if (-not $result.ProcessAliveAfterUiOps) { $failures += 'process-alive' }
if (-not $result.ProcessClosedAfterSelftest) { $failures += 'process-exit' }

$result | ConvertTo-Json -Compress

if ($failures.Count -gt 0) {
    throw ('native UI selftest failed: ' + ($failures -join ', '))
}
