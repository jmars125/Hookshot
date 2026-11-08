using System.IO;

namespace HookshotBuilder;

/// A .nup found on this PC
sealed record Dump(string Path, string Where, long Size, DateTime Modified);

/// The player's game files where people keep them, and the game's shortcuts.
static class Finder
{
    /// .nup files in Downloads, Desktop, Documents, the home folder and the top of each drive, one folder level down too,
    /// newest first. (Only names and sizes are read here: the version is checked one file at a time afterwards.)
    public static List<Dump> FindDumps()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var places = new List<(string Dir, string Name)>
        {
            (Path.Combine(home, "Downloads"), "Downloads"),
            (Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Desktop"),
            (Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Documents"),
            (home, "Home"),
        };
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                if (d.IsReady && d.DriveType is DriveType.Fixed or DriveType.Removable) places.Add((d.RootDirectory.FullName, d.Name.TrimEnd('\\')));
            }
            catch (Exception) { }
        }
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var found = new List<Dump>();
        foreach (var (dir, name) in places)
        {
            if (!Directory.Exists(dir) || !seen.Add(Path.GetFullPath(dir))) continue;
            var dirs = new List<string> { dir };
            try
            {
                dirs.AddRange(Directory.EnumerateDirectories(dir)
                    .Where(s => !Path.GetFileName(s).StartsWith('.') && !Path.GetFileName(s).StartsWith('$')
                                && !string.Equals(Path.GetFileName(s), "Windows", StringComparison.OrdinalIgnoreCase)
                                && !Path.GetFileName(s).StartsWith("Program", StringComparison.OrdinalIgnoreCase))
                    .Take(200));
            }
            catch (Exception) { }
            foreach (var sub in dirs)
            {
                try
                {
                    foreach (var f in Directory.EnumerateFiles(sub, "*.nup"))
                    {
                        var info = new FileInfo(f);
                        if (seen.Add(info.FullName)) found.Add(new Dump(info.FullName, name, info.Length, info.LastWriteTime));
                    }
                }
                catch (Exception) { }
            }
        }
        return found.OrderByDescending(d => d.Modified).ToList();
    }

    public static string Ago(DateTime t)
    {
        var d = DateTime.Now - t;
        if (d.TotalDays < 1 && t.Date == DateTime.Today) return "modified today";
        if (d.TotalDays < 2) return "modified yesterday";
        if (d.TotalDays < 14) return $"{(int)d.TotalDays} days ago";
        if (d.TotalDays < 60) return $"{(int)(d.TotalDays / 7)} weeks ago";
        return t.ToString("d MMM yyyy");
    }

    public static string Size(long bytes) => bytes >= 1L << 30 ? $"{bytes / (double)(1L << 30):0.0} GB" : $"{bytes >> 20} MB";

    // ---- shortcuts to the built game ----

    static string DesktopLink => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Hookshot.lnk");
    static string StartLink => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Hookshot.lnk");

    /// Made or removed as the two ticks say (Windows' own shell object writes the .lnk)
    public static void Shortcuts(string gameFolder, bool desktop, bool start)
    {
        var exe = Path.Combine(gameFolder, "hookshot.exe");
        foreach (var (link, want) in new[] { (DesktopLink, desktop), (StartLink, start) })
        {
            try
            {
                if (!want)
                {
                    if (File.Exists(link)) File.Delete(link);
                    continue;
                }
                var type = Type.GetTypeFromProgID("WScript.Shell");
                if (type == null || !File.Exists(exe)) continue;
                dynamic shell = Activator.CreateInstance(type)!;
                dynamic s = shell.CreateShortcut(link);
                s.TargetPath = exe;
                s.WorkingDirectory = gameFolder;
                s.IconLocation = exe + ",0";
                s.Description = "The Legend of Zelda: Ocarina of Time, built by Hookshot";
                s.Save();
            }
            catch (Exception) { }
        }
    }
}
