using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace HookshotBuilder;

/// Windows 11's acrylic behind the window, dark title colours and round corners (DWM). Windows 10 has none of it: the
/// window is plain Slate there.
static class Glass
{
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("dwmapi.dll")] static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    [StructLayout(LayoutKind.Sequential)]
    struct Margins { public int Left, Right, Top, Bottom; }

    const int DarkMode = 20, CornerPreference = 33, SystemBackdrop = 38;
    const int Round = 2, Acrylic = 3;

    /// true: acrylic is on (the window's background must stay see-through)
    public static bool Apply(Window w)
    {
        var hwnd = new WindowInteropHelper(w).Handle;
        if (hwnd == IntPtr.Zero) return false;
        var on = 1;
        DwmSetWindowAttribute(hwnd, DarkMode, ref on, sizeof(int));
        var corner = Round;
        DwmSetWindowAttribute(hwnd, CornerPreference, ref corner, sizeof(int));
        if (Environment.OSVersion.Version.Build < 22621 || SystemParameters.HighContrast) return false;   // (22H2: the backdrop attribute)
        if (HwndSource.FromHwnd(hwnd) is { CompositionTarget: { } target }) target.BackgroundColor = Colors.Transparent;
        var all = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref all);
        var backdrop = Acrylic;
        return DwmSetWindowAttribute(hwnd, SystemBackdrop, ref backdrop, sizeof(int)) == 0;
    }
}
