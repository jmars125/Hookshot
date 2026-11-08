using System.Diagnostics;
using System.Globalization;

namespace HookshotBuilder;

/// A choice for this window only. Automatic leaves the build tools' inherited override and memory policy alone.
static class BuildWorkers
{
    public const string Automatic = "Automatic";

    public static IEnumerable<object> Choices(int processors)
    {
        yield return Automatic;
        for (var count = 1; count <= Math.Max(1, processors); count++) yield return count;
    }

    public static int? Selected(object? value) => value switch
    {
        string text when text == Automatic => null,
        int count when count > 0 => count,
        _ => throw new ArgumentException("Choose Automatic or a positive number of build workers."),
    };

    public static void Apply(ProcessStartInfo info, int? count)
    {
        if (count is <= 0) throw new ArgumentOutOfRangeException(nameof(count), "Build workers must be positive.");
        if (count is int selected) info.Environment["HOOKSHOT_JOBS"] = selected.ToString(CultureInfo.InvariantCulture);
    }
}
