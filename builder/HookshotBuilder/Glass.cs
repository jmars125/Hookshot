using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;

namespace HookshotBuilder;

/// Windows 11's acrylic behind the window, dark title colours and round corners (DWM). Windows 10 has none of it, and
/// neither does a light Windows: both get the plain Slate window, which the light palette is made for.
static class Glass
{
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("dwmapi.dll")] static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);
    [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtrW(IntPtr hwnd, int index);
    [DllImport("user32.dll")] static extern IntPtr SetWindowLongPtrW(IntPtr hwnd, int index, IntPtr value);

    [StructLayout(LayoutKind.Sequential)]
    struct Margins { public int Left, Right, Top, Bottom; }

    const int DarkMode = 20, CornerPreference = 33, SystemBackdrop = 38;
    const int Round = 2, Acrylic = 3;

    /// Windows draws the acrylic in the system's Windows mode. Its light variant is brighter than anything the installer's
    /// tint can darken enough: the fixed Slate palette has light text and InkFaint (the inactive steps, the "Supported
    /// version" badge, the details) needs the dark page behind it. A light system gets that page, as Windows 10 does.
    static bool LightBackdrop()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            var value = key?.GetValue("SystemUsesLightTheme") ?? key?.GetValue("AppsUseLightTheme");
            return value is int on && on != 0;
        }
        catch
        {
            return false;
        }
    }

    /// true: acrylic is on (the window's background must stay see-through)
    public static bool Apply(Window w)
    {
        var hwnd = new WindowInteropHelper(w).Handle;
        if (hwnd == IntPtr.Zero) return false;
        // (a framed window: Windows draws the backdrop only behind one. Without the system menu it draws no caption buttons of its
        // own behind the window's; WindowStyle None lost the acrylic, and the window was flat grey)
        const int GwlStyle = -16;
        const long SysMenu = 0x00080000;
        SetWindowLongPtrW(hwnd, GwlStyle, (IntPtr)((long)GetWindowLongPtrW(hwnd, GwlStyle) & ~SysMenu));
        var on = 1;
        DwmSetWindowAttribute(hwnd, DarkMode, ref on, sizeof(int));
        var corner = Round;
        DwmSetWindowAttribute(hwnd, CornerPreference, ref corner, sizeof(int));
        if (LightBackdrop()) return false;      // (the plain Slate page: light acrylic cannot carry the palette)
        if (Environment.OSVersion.Version.Build < 22621 || SystemParameters.HighContrast) return false;   // (22H2: the backdrop attribute)
        if (HwndSource.FromHwnd(hwnd) is { CompositionTarget: { } target }) target.BackgroundColor = Colors.Transparent;
        var all = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref all);
        var backdrop = Acrylic;
        return DwmSetWindowAttribute(hwnd, SystemBackdrop, ref backdrop, sizeof(int)) == 0;
    }
}
