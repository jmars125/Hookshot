using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Windows;

namespace HookshotBuilder;

/// The game's update offer (host/hos_update.c) starts this app with --update <game folder> --tag <release>. When these tools are
/// not that release, the release's HookshotBuilder.zip is downloaded and unpacked beside this app, and the new app is started with
/// --fetched to build the folder; when they are, the folder is built again here (nx2/make_game.py --update: from the game files the
/// folder keeps).
static class Updater
{
    const string Latest = "https://api.github.com/repos/NotRexed/Hookshot/releases/latest";
    const string Asset = "HookshotBuilder.zip";

    public static async void FromCommandLine(MainWindow w)
    {
        var a = Environment.GetCommandLineArgs();
        string? folder = null, tag = null;
        var fetched = false;
        for (var i = 1; i < a.Length; i++)
        {
            if (a[i] == "--update" && i + 1 < a.Length) folder = a[++i];
            else if (a[i] == "--tag" && i + 1 < a.Length) tag = a[++i];
            else if (a[i] == "--fetched") fetched = true;
        }
        if (folder == null) return;
        w.BeginUpdate(folder);
        var tools = Tools.Find();
        if (tools == null)
        {
            w.DownloadFailed("The Hookshot tools were not found next to this app.");
            return;
        }
        // (already the new release, or just downloaded: no second download, whatever its version file says)
        if (fetched || (tag != null && tools.Version == tag))
        {
            w.UpdateBuild(tools, folder);
            return;
        }
        try
        {
            var token = w.DownloadToken;
            using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
            w.Note("Asking GitHub for the latest release");
            string? newTag = null, url = null;
            long size = 0;
            using (var ask = new CancellationTokenSource(TimeSpan.FromSeconds(30)))
            using (var both = CancellationTokenSource.CreateLinkedTokenSource(token, ask.Token))
            using (var doc = JsonDocument.Parse(await http.GetStringAsync(Latest, both.Token)))
            {
                newTag = doc.RootElement.GetProperty("tag_name").GetString();
                foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
                    if (asset.GetProperty("name").GetString() == Asset)
                    {
                        url = asset.GetProperty("browser_download_url").GetString();
                        size = asset.GetProperty("size").GetInt64();
                    }
            }
            if (newTag == null || url == null) throw new InvalidOperationException("The latest release has no " + Asset + ".");
            if (newTag == tools.Version)
            {
                w.UpdateBuild(tools, folder);
                return;
            }
            var name = string.Concat(newTag.Where(c => char.IsLetterOrDigit(c) || c == '-' || c == '_' || c == '.'));
            var cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HookshotBuilder", "update");
            Directory.CreateDirectory(cache);
            var zip = Path.Combine(cache, "HookshotBuilder-" + name + ".zip");
            w.Note($"Downloading {Asset} of {newTag} ({size >> 20} MB)");
            using (var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token))
            {
                response.EnsureSuccessStatusCode();
                var total = response.Content.Headers.ContentLength ?? size;
                await using var from = await response.Content.ReadAsStreamAsync(token);
                await using var to = File.Create(zip);
                var buffer = new byte[1 << 20];
                long have = 0;
                var shown = DateTime.MinValue;
                for (int n; (n = await from.ReadAsync(buffer, token)) > 0;)
                {
                    await to.WriteAsync(buffer.AsMemory(0, n), token);
                    have += n;
                    if (total > 0 && DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(200))
                    {
                        shown = DateTime.UtcNow;
                        w.DownloadProgress(Math.Min(1.0, (double)have / total));
                    }
                }
                if (total > 0 && have != total) throw new IOException($"The download ended early ({have >> 20} of {total >> 20} MB).");
            }
            w.Note("Unpacking the new version");
            // (beside this app when that folder takes writing, else in the app data folder)
            var parent = Directory.GetParent(AppContext.BaseDirectory.TrimEnd('\\', '/'))!.FullName;
            string target;
            try
            {
                target = await Unpack(zip, Path.Combine(parent, "HookshotBuilder " + name));
            }
            catch (Exception e) when (e is UnauthorizedAccessException or IOException)
            {
                target = await Unpack(zip, Path.Combine(Path.GetDirectoryName(cache)!, "builders", name));
            }
            try { File.Delete(zip); } catch (Exception) { }
            var exe = Path.Combine(target, "HookshotBuilder.exe");
            if (!File.Exists(exe)) throw new FileNotFoundException("The download has no HookshotBuilder.exe (your antivirus may have removed it).");
            var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = target };
            foreach (var arg in new[] { "--update", folder, "--tag", newTag, "--fetched" }) si.ArgumentList.Add(arg);
            Process.Start(si);
            Application.Current.Shutdown();
        }
        catch (OperationCanceledException)
        {
            w.DownloadFailed("The download was cancelled. Your game folder is unchanged.");
        }
        catch (Exception e)
        {
            w.DownloadFailed(e.Message + " Your game folder is unchanged; you can also download the new version from GitHub yourself.");
        }
    }

    /// The zip's HookshotBuilder folder, as target (an older one there replaced)
    static async Task<string> Unpack(string zip, string target)
    {
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        await Task.Run(() => ZipFile.ExtractToDirectory(zip, part));
        var inner = Path.Combine(part, "HookshotBuilder");
        if (Directory.Exists(target)) Directory.Delete(target, true);
        Directory.Move(Directory.Exists(inner) ? inner : part, target);
        if (Directory.Exists(part)) Directory.Delete(part, true);
        return target;
    }
}
