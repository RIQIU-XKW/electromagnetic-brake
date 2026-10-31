# 截取 Electromagnetic_braking 主窗口, 存成 PNG(验证 UI 布局用)。
# 用 PrintWindow(PW_RENDERFULLCONTENT) 抓窗口自身内容, 不怕被别的窗口挡住。
# ASCII only.

param(
    # repo root = <this script dir>\.. ; Qt build drops the exe in <repo>\build\qt
    [string] $Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\qt\Electromagnetic_braking.exe"),
    [string] $Out = (Join-Path $PSScriptRoot "outputs\ui_shot.png"),
    [int]    $WaitMs = 3200
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinShot {    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int X, int Y, int W, int H, bool repaint);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    /* 本机是 150% DPI: 不声明 DPI 感知的话 GetWindowRect 给的是"逻辑"尺寸(1440x900),
       而窗口实际占 2160x1350 物理像素 —— 截出来就是左上角一块, 右边的控件全丢。
       所以进程必须先在**取窗口矩形之前**声明 DPI 感知。 */
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();

    public static int W, H;
    public static bool Grab(IntPtr h, string path) {
        RECT r;
        if (!GetWindowRect(h, out r)) return false;
        W = r.Right - r.Left;
        H = r.Bottom - r.Top;
        if (W <= 0 || H <= 0) return false;
        using (System.Drawing.Bitmap bmp = new System.Drawing.Bitmap(W, H)) {
            using (System.Drawing.Graphics g = System.Drawing.Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                bool ok = PrintWindow(h, hdc, 2u);   // 2 = PW_RENDERFULLCONTENT
                g.ReleaseHdc(hdc);
                if (!ok) return false;
            }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);

            /* 再存两张裁图: 读图工具给的预览只有 ~931 px, 975 宽的整图会被裁掉右边。
               左边那张看第 1 列, 右边那张看最后一列(矩阵/按钮的最右侧)。 */
            int cw = W < 900 ? W : 900;
            using (System.Drawing.Bitmap crop = bmp.Clone(
                       new System.Drawing.Rectangle(0, 0, cw, H), bmp.PixelFormat)) {
                crop.Save(path.Replace(".png", "_left.png"), System.Drawing.Imaging.ImageFormat.Png);
            }

            int rw = W < 520 ? W : 520;
            using (System.Drawing.Bitmap crop = bmp.Clone(
                       new System.Drawing.Rectangle(W - rw, 0, rw, H), bmp.PixelFormat)) {
                crop.Save(path.Replace(".png", "_right.png"), System.Drawing.Imaging.ImageFormat.Png);
            }

            /* 第 4 列(最右边那一列)的窄条: 看 LED3 和"读板子状态"按钮在不在。 */
            int nw = W < 340 ? W : 340;
            using (System.Drawing.Bitmap crop = bmp.Clone(
                       new System.Drawing.Rectangle(W - nw, 0, nw, H), bmp.PixelFormat)) {
                crop.Save(path.Replace(".png", "_col4.png"), System.Drawing.Imaging.ImageFormat.Png);
            }
        }
        return true;
    }
}
"@ -ReferencedAssemblies "System.Drawing"

[void][WinShot]::SetProcessDPIAware()

$proc = Start-Process -FilePath $Exe -PassThru

# 等主窗口出来**并且真的有了大小**再截 —— 刚启动时 MainWindowHandle 可能先指到一个
# 还没布局完的 158x26 的小窗口, 截出来就是一张没用的图。
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 200
    $proc.Refresh()
    if ($proc.HasExited) { Write-Host "process exited early"; exit 3 }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { continue }

    $r = New-Object WinShot+RECT
    if ([WinShot]::GetWindowRect($proc.MainWindowHandle, [ref]$r)) {
        $w = $r.Right - $r.Left
        $hh = $r.Bottom - $r.Top
        if ($w -ge 400 -and $hh -ge 300) { $h = $proc.MainWindowHandle; break }
    }
}

if ($h -eq [IntPtr]::Zero) {
    Write-Host "main window never appeared"
    $proc | Stop-Process -Force
    exit 2
}

# 铺满工作区: 预设页是个长滚动区, 宽一点才能看清布局有没有横向溢出。
# ShowWindow(SW_MAXIMIZE) 在这个程序上不生效(窗口大小被 loadSettings 里的 resize 定过),
# 所以直接 MoveWindow 到工作区大小。尺寸是**物理像素**(已经声明 DPI 感知):
# 本机 150% -> 2100x1300 物理 ≈ 1400x867 逻辑。
[void][WinShot]::MoveWindow($h, 0, 0, 2100, 1300, $true)
[void][WinShot]::SetForegroundWindow($h)
Start-Sleep -Milliseconds $WaitMs

$ok = [WinShot]::Grab($h, $Out)
Write-Host ("capture ok={0} size={1}x{2} -> {3}" -f $ok, [WinShot]::W, [WinShot]::H, $Out)

Start-Sleep -Milliseconds 300
$proc | Stop-Process -Force

if (-not $ok) { exit 1 }
exit 0
