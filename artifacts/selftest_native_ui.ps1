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

function Get-ScreenPoint([int]$clientX, [int]$clientY) {
    $point = New-Object POINT
    $point.X = $clientX
    $point.Y = $clientY
    [Native]::ClientToScreen($hwnd, [ref]$point) | Out-Null
    return $point
}

function Click-Client([int]$clientX, [int]$clientY) {
    $point = Get-ScreenPoint $clientX $clientY
    [Native]::SetCursorPos($point.X, $point.Y) | Out-Null
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 80
    [Native]::mouse_event($MouseEventLeftDown, 0, 0, 0, [UIntPtr]::Zero)
    [Native]::mouse_event($MouseEventLeftUp, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
}

$dpi = [Native]::GetDpiForWindow($hwnd)
if ($dpi -le 0) {
    $dpi = 96
}
$dpiScale = $dpi / 96.0

function Scale-Logical([double]$value) {
    return [int][Math]::Round($value * $dpiScale)
}

$clientBounds = Get-ClientBoundsInWindow
$clientWidth = $clientBounds.Width
$panelWidth = (Scale-Logical 200)
$captionHeight = (Scale-Logical 32)
$outerMargin = (Scale-Logical 24)
$stackPadding = (Scale-Logical 24)
$stackGap = (Scale-Logical 12)
$interCardGap = (Scale-Logical 24)
$minNarrowCardWidth = (Scale-Logical 300)
$maxNarrowCardWidth = (Scale-Logical 344)
$twoColumnThreshold = (Scale-Logical 728)

$availableWidth = $clientWidth - $panelWidth - (Scale-Logical 48)
$twoColumn = $availableWidth -ge $twoColumnThreshold
if ($twoColumn) {
    $cardWidth = [int][Math]::Round(($availableWidth - $interCardGap) / 2.0)
    $rightCardLeft = $panelWidth + $outerMargin + $cardWidth + $interCardGap
}
else {
    $cardWidth = [Math]::Min($maxNarrowCardWidth, [Math]::Max($minNarrowCardWidth, $availableWidth))
    $rightCardLeft = $clientWidth - $cardWidth - $outerMargin
}
$rightCardTop = $captionHeight + $outerMargin

$controlLeft = $rightCardLeft + $stackPadding
$controlWidth = $cardWidth - ($stackPadding * 2)

$singleBoundsTop = $rightCardTop + $stackPadding
$singleBoundsHeight = (Scale-Logical 60)
$multiBoundsTop = $singleBoundsTop + $singleBoundsHeight + $stackGap
$multiBoundsHeight = (Scale-Logical 118)
$listBoundsTop = $multiBoundsTop + $multiBoundsHeight + $stackGap
$listBoundsHeight = (Scale-Logical 142)
$comboBoundsTop = $listBoundsTop + $listBoundsHeight + $stackGap
$comboBoundsHeight = (Scale-Logical 38)

$singleInputX = $controlLeft + [int][Math]::Round($controlWidth * 0.25)
$singleInputY = $singleBoundsTop + (Scale-Logical 40)
$multiInputX = $controlLeft + (Scale-Logical 24)
$multiInputY = $multiBoundsTop + (Scale-Logical 54)
$listBoxX = $controlLeft + (Scale-Logical 24)
$listBoxY = $listBoundsTop + (Scale-Logical 20)
$comboX = $controlLeft + (Scale-Logical 24)
$comboY = $comboBoundsTop + [int][Math]::Round($comboBoundsHeight * 0.5)
$comboPopupItemY = ($comboBoundsTop + $comboBoundsHeight + (Scale-Logical 6) + (Scale-Logical 6) + (Scale-Logical 11) + (Scale-Logical 22))

Save-Capture 'native_ui_01_initial.png'

Click-Client $singleInputX $singleInputY
[System.Windows.Forms.SendKeys]::SendWait('^a')
Start-Sleep -Milliseconds 120
[System.Windows.Forms.SendKeys]::SendWait('^c')
Start-Sleep -Milliseconds 120
$clipboardBefore = Get-Clipboard -Raw
$copyOk = $clipboardBefore -eq 'draw hitmarker'

[System.Windows.Forms.SendKeys]::SendWait('native ui selection ok')
Start-Sleep -Milliseconds 180
[System.Windows.Forms.SendKeys]::SendWait('^a')
Start-Sleep -Milliseconds 80
[System.Windows.Forms.SendKeys]::SendWait('^c')
Start-Sleep -Milliseconds 120
$clipboardAfter = Get-Clipboard -Raw
$replaceOk = $clipboardAfter -eq 'native ui selection ok'

Click-Client $multiInputX $multiInputY
[System.Windows.Forms.SendKeys]::SendWait('{END}{ENTER}Clipboard and selection test passed.')
Start-Sleep -Milliseconds 200

Click-Client $listBoxX $listBoxY
Start-Sleep -Milliseconds 120

Click-Client $comboX $comboY
Start-Sleep -Milliseconds 160
Save-Capture 'native_ui_02_combo_open.png'
Click-Client $comboX $comboPopupItemY
Start-Sleep -Milliseconds 180

Save-Capture 'native_ui_03_after_interaction.png'
[AsyncSaver]::WaitAll()

$stillAlive = (Get-Process -Id $window.Id -ErrorAction SilentlyContinue) -ne $null

[pscustomobject]@{
    SingleInputCopyInitial = $copyOk
    SingleInputReplaceAndCopy = $replaceOk
    ProcessAliveAfterUiOps = $stillAlive
    Screenshots = @(
        (Join-Path $OutputDir 'native_ui_01_initial.png'),
        (Join-Path $OutputDir 'native_ui_02_combo_open.png'),
        (Join-Path $OutputDir 'native_ui_03_after_interaction.png')
    ) -join ';'
}
