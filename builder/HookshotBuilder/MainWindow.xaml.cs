using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;

namespace HookshotBuilder;

public partial class MainWindow : Window
{
    const long NeededBytes = 60L << 30;
    static readonly (string Id, S Text)[] Stages = { ("read", S.Read), ("lift", S.Lift), ("compile", S.Compile), ("finish", S.Finish) };
    Language lang = Strings.Default();
    string T(S s) => (int)s < lang.Text.Length ? lang.Text[(int)s] : Strings.All[0].Text[(int)s];
    string FindText => T(S.DropHint);

    readonly Dictionary<string, StageRow> rows = new();
    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly DispatcherTimer checkDelay = new() { Interval = TimeSpan.FromMilliseconds(500) };
    readonly Stopwatch elapsed = new();
    Runner? runner;
    string? current;
    string finished = "";
    int checkSerial;
    bool sourceOk;

    public MainWindow()
    {
        InitializeComponent();
        OutPath.Text = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "Hookshot");
        foreach (var (id, text) in Stages)
        {
            rows[id] = new StageRow(T(text));
            StageList.Children.Add(rows[id].Root);
        }
        clock.Tick += (_, _) => ShowClock();
        checkDelay.Tick += (_, _) => { checkDelay.Stop(); CheckSource(); };
        Drop += Dropped;
        LanguageBox.ItemsSource = Strings.All;
        LanguageBox.SelectedItem = lang;
        ApplyLanguage();
        Refresh();
        Loaded += (_, _) => Updater.FromCommandLine(this);
    }

    void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LanguageBox.SelectedItem is Language l) { lang = l; ApplyLanguage(); }
    }

    /// Every word of the window in the chosen language; the game starts in it too (make_game.py --language writes language.txt).
    void ApplyLanguage()
    {
        if (BuildButton == null) return;
        Subtitle.Text = T(S.Subtitle);
        GameFilesLabel.Text = T(S.GameFiles);
        UseNup.Content = T(S.Nup);
        UseFolders.Content = T(S.Folders);
        BrowseA.Content = BrowseB.Content = BrowseC.Content = BrowseD.Content = T(S.Browse);
        SaveToLabel.Text = T(S.SaveTo);
        BuildButton.Content = T(S.Build);
        ShowLog.Content = T(ShowLog.IsChecked == true ? S.HideDetails : S.ShowDetails);
        CancelButton.Content = T(S.Cancel);
        BackButton.Content = T(S.Back);
        OpenButton.Content = T(S.OpenFolder);
        PlayButton.Content = T(S.Play);
        foreach (var (id, text) in Stages) rows[id].SetText(T(text));
        if (!sourceOk && !SourceGiven) Say(T(S.DropHint), "InkSoft");
        else CheckSource();
        Refresh();
    }

    // ---- the first page ----

    void SourceKindChanged(object sender, RoutedEventArgs e)
    {
        if (NupRow == null) return;
        NupRow.Visibility = UseNup.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        FolderRows.Visibility = UseNup.IsChecked == true ? Visibility.Collapsed : Visibility.Visible;
        PathsChanged(sender, e);
    }

    void PathsChanged(object sender, RoutedEventArgs e)
    {
        if (BuildButton == null) return;
        sourceOk = false;
        checkDelay.Stop();
        checkDelay.Start();
        Refresh();
    }

    bool SourceGiven => UseNup.IsChecked == true ? File.Exists(NupPath.Text)
        : Directory.Exists(RomfsPath.Text) && File.Exists(System.IO.Path.Combine(ExefsPath.Text, "main"));

    void Refresh()
    {
        BuildButton.IsEnabled = sourceOk && OutPath.Text.Length > 2;
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(OutPath.Text));
            var free = new DriveInfo(root!).AvailableFreeSpace;
            var enough = free >= NeededBytes;
            SpaceText.Text = T(S.Needs) + " " + string.Format(T(S.Free), root, free >> 30);
            SpaceText.Foreground = (Brush)FindResource(enough ? "InkSoft" : "Bad");
        }
        catch (Exception)
        {
            SpaceText.Text = T(S.Needs);
        }
    }

    async void CheckSource()
    {
        if (!SourceGiven)
        {
            Say(T(S.DropHint), "InkSoft");
            return;
        }
        var serial = ++checkSerial;
        Say(T(S.Checking), "InkSoft");
        var tools = Tools.Find();
        if (tools == null)
        {
            Say(T(S.ToolsMissing), "Bad");
            return;
        }
        var args = new List<string> { "--check" };
        args.AddRange(UseNup.IsChecked == true ? new[] { "--nup", NupPath.Text } : new[] { "--exefs", ExefsPath.Text });
        var (ok, text) = await Runner.Check(tools, args);
        if (serial != checkSerial) return;
        sourceOk = ok;
        Say(ok ? T(S.VersionOk) : text.StartsWith("This is build") ? T(S.VersionBad) : text, ok ? "Accent" : "Bad");
        Refresh();
    }

    void Say(string text, string brush)
    {
        CheckText.Text = text;
        CheckText.Foreground = (Brush)FindResource(brush);
    }

    void Dropped(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        foreach (var p in paths)
        {
            var name = System.IO.Path.GetFileName(p).ToLowerInvariant();
            if (name.EndsWith(".nup")) { UseNup.IsChecked = true; NupPath.Text = p; }
            else if (name == "romfs") { UseFolders.IsChecked = true; RomfsPath.Text = p; }
            else if (name == "exefs") { UseFolders.IsChecked = true; ExefsPath.Text = p; }
            else if (Directory.Exists(System.IO.Path.Combine(p, "romfs")) && Directory.Exists(System.IO.Path.Combine(p, "exefs")))
            {
                UseFolders.IsChecked = true;
                RomfsPath.Text = System.IO.Path.Combine(p, "romfs");
                ExefsPath.Text = System.IO.Path.Combine(p, "exefs");
            }
        }
    }

    void BrowseNup(object sender, RoutedEventArgs e)
    {
        var d = new OpenFileDialog { Filter = "Game file (*.nup)|*.nup", Title = "Choose your game file" };
        if (d.ShowDialog(this) == true) NupPath.Text = d.FileName;
    }

    static string? PickFolder(string title)
    {
        var d = new OpenFolderDialog { Title = title };
        return d.ShowDialog() == true ? d.FolderName : null;
    }

    void BrowseRomfs(object sender, RoutedEventArgs e) => RomfsPath.Text = PickFolder("Choose the romfs folder") ?? RomfsPath.Text;
    void BrowseExefs(object sender, RoutedEventArgs e) => ExefsPath.Text = PickFolder("Choose the exefs folder") ?? ExefsPath.Text;
    void BrowseOut(object sender, RoutedEventArgs e) => OutPath.Text = PickFolder("Choose where to save the game") ?? OutPath.Text;

    // ---- the build ----

    void StartBuild(object sender, RoutedEventArgs e)
    {
        var tools = Tools.Find();
        if (tools == null) return;
        var args = new List<string>();
        if (UseNup.IsChecked == true) args.AddRange(new[] { "--nup", NupPath.Text });
        else args.AddRange(new[] { "--romfs", RomfsPath.Text, "--exefs", ExefsPath.Text });
        args.AddRange(new[] { "--out", OutPath.Text, "--language", lang.Code });
        Run(tools, args);
    }

    // ---- the update (Updater.cs): the game started this app with --update <folder> ----

    StageRow? downloadRow;
    CancellationTokenSource? download;

    /// The download's line at the top of the list, and the page shown
    public void BeginUpdate(string folder)
    {
        OutPath.Text = folder;
        downloadRow ??= new StageRow(T(S.Download));
        if (!StageList.Children.Contains(downloadRow.Root)) StageList.Children.Insert(0, downloadRow.Root);
        foreach (var r in rows.Values) r.Reset();
        downloadRow.Reset();
        downloadRow.Begin();
        Log.Clear();
        BuildTitle.Text = T(S.Updating);
        BuildNote.Text = "";
        BuildNote.Foreground = (Brush)FindResource("InkSoft");
        ShowPage(building: true);
        SetButtons(cancel: true);
    }

    public CancellationToken DownloadToken => (download = new CancellationTokenSource()).Token;
    public void DownloadProgress(double f) => downloadRow?.Progress(f);
    public void Note(string line) => Line(line);

    public void DownloadFailed(string why)
    {
        download = null;
        downloadRow?.Stopped();
        BuildTitle.Text = T(S.UpdateFailed);
        BuildNote.Text = why;
        BuildNote.Foreground = (Brush)FindResource("Bad");
        SetButtons(back: true);
    }

    /// The new tools are here: the folder built again with them
    internal void UpdateBuild(Tools tools, string folder)
    {
        download = null;
        downloadRow?.Done();
        BuildTitle.Text = T(S.Updating);
        Run(tools, new List<string> { "--update", folder }, keepRows: true);
    }

    void Run(Tools tools, List<string> args, bool keepRows = false)
    {
        if (!keepRows)
        {
            if (downloadRow != null) StageList.Children.Remove(downloadRow.Root);
            BuildTitle.Text = T(S.Building);
        }
        foreach (var r in rows.Values) r.Reset();
        if (!keepRows) Log.Clear();
        current = null;
        finished = "";
        BuildNote.Foreground = (Brush)FindResource("InkSoft");
        ShowPage(building: true);
        SetButtons(cancel: true);
        elapsed.Restart();
        clock.Start();
        ShowClock();

        runner = new Runner(tools, args);
        runner.Line += l => Dispatcher.BeginInvoke(() => Line(l));
        runner.Finished += failure => Dispatcher.BeginInvoke(() => Ended(failure));
        runner.Start();
    }

    void Line(string line)
    {
        if (line.StartsWith("@@stage "))
        {
            var id = line.Split(' ')[1];
            if (current != null && rows.TryGetValue(current, out var was)) was.Done();
            current = id;
            if (rows.TryGetValue(id, out var row)) row.Begin();
        }
        else if (line.StartsWith("@@progress "))
        {
            if (current != null && rows.TryGetValue(current, out var row) &&
                double.TryParse(line[11..], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f))
                row.Progress(f);
        }
        else if (line.StartsWith("@@done "))
        {
            finished = line[7..];
        }
        if (!line.StartsWith("@@progress"))
        {
            Log.AppendText(line + "\n");
            if (Log.LineCount > 4000) Log.Text = Log.Text[(Log.Text.Length / 2)..];
            if (ShowLog.IsChecked == true) Log.ScrollToEnd();
        }
    }

    void Ended(string? failure)
    {
        clock.Stop();
        elapsed.Stop();
        runner = null;
        if (finished.Length > 0)
        {
            foreach (var r in rows.Values) r.Done();
            BuildTitle.Text = T(S.Ready);
            BuildNote.Text = $"{finished}  ·  {T(S.BuiltIn)} {Span(elapsed.Elapsed)}";
            SetButtons(done: true);
            return;
        }
        BuildTitle.Text = T(failure == "cancelled" ? S.Cancelled : S.Stopped);
        BuildNote.Text = failure == "cancelled" ? T(S.CancelledNote) : failure ?? "";
        BuildNote.Foreground = (Brush)FindResource(failure == "cancelled" ? "InkSoft" : "Bad");
        if (current != null && rows.TryGetValue(current, out var row)) row.Stopped();
        SetButtons(back: true);
    }

    void ShowClock()
    {
        if (runner != null) BuildNote.Text = $"{T(S.Patience)}  {T(S.Elapsed)}: {Span(elapsed.Elapsed)}";
    }

    static string Span(TimeSpan t) => t.TotalHours >= 1 ? $"{(int)t.TotalHours} h {t.Minutes} min" : t.TotalMinutes >= 1 ? $"{t.Minutes} min" : $"{t.Seconds} s";

    void ShowPage(bool building)
    {
        SetupPage.Visibility = building ? Visibility.Collapsed : Visibility.Visible;
        BuildPage.Visibility = building ? Visibility.Visible : Visibility.Collapsed;
    }

    void SetButtons(bool cancel = false, bool done = false, bool back = false)
    {
        CancelButton.Visibility = cancel ? Visibility.Visible : Visibility.Collapsed;
        PlayButton.Visibility = OpenButton.Visibility = done ? Visibility.Visible : Visibility.Collapsed;
        BackButton.Visibility = back ? Visibility.Visible : Visibility.Collapsed;
    }

    void CancelBuild(object sender, RoutedEventArgs e)
    {
        download?.Cancel();
        runner?.Cancel();
    }
    void Back(object sender, RoutedEventArgs e) => ShowPage(building: false);

    void LogToggled(object sender, RoutedEventArgs e)
    {
        var show = ShowLog.IsChecked == true;
        Log.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        ShowLog.Content = T(show ? S.HideDetails : S.ShowDetails);
        if (show) Log.ScrollToEnd();
    }

    void OpenFolder(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{finished}\"") { UseShellExecute = true });

    void Play(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(System.IO.Path.Combine(finished, "hookshot.exe")) { UseShellExecute = true, WorkingDirectory = finished });

    void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    void CloseWindow(object sender, RoutedEventArgs e)
    {
        if (runner != null && MessageBox.Show(this, T(S.CloseQuestion), "Hookshot Builder", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        runner?.Cancel();
        Close();
    }
}

/// One line of the build's list: a marker, the step's name, and a thin bar while it has progress to show.
sealed class StageRow
{
    public readonly Grid Root = new() { Margin = new Thickness(0, 0, 0, 14) };
    readonly Ellipse dot = new() { Width = 8, Height = 8, VerticalAlignment = VerticalAlignment.Center };
    readonly TextBlock name = new();
    readonly TextBlock percent = new() { HorizontalAlignment = HorizontalAlignment.Right };
    readonly ProgressBar bar = new() { Margin = new Thickness(20, 6, 0, 0), Visibility = Visibility.Collapsed };

    public StageRow(string text)
    {
        Root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(20) });
        Root.ColumnDefinitions.Add(new ColumnDefinition());
        Root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        name.Text = text;
        bar.Style = (Style)Application.Current.FindResource("Thin");
        percent.Foreground = Brush("InkSoft");
        Grid.SetColumn(name, 1);
        Grid.SetColumn(percent, 1);
        Grid.SetRow(bar, 1);
        Grid.SetColumnSpan(bar, 2);
        Root.Children.Add(dot);
        Root.Children.Add(name);
        Root.Children.Add(percent);
        Root.Children.Add(bar);
        Reset();
    }

    static Brush Brush(string key) => (Brush)Application.Current.FindResource(key);

    public void SetText(string text) => name.Text = text;

    public void Reset()
    {
        dot.Fill = Brush("Line");
        name.Foreground = Brush("InkFaint");
        percent.Text = "";
        bar.Visibility = Visibility.Collapsed;
        bar.Value = 0;
    }

    public void Begin()
    {
        dot.Fill = Brush("Ink");
        name.Foreground = Brush("Ink");
    }

    public void Progress(double f)
    {
        bar.Visibility = Visibility.Visible;
        bar.Value = f;
        percent.Text = $"{(int)(f * 100)}%";
    }

    public void Done()
    {
        dot.Fill = Brush("Accent");
        name.Foreground = Brush("InkSoft");
        percent.Text = "";
        bar.Visibility = Visibility.Collapsed;
    }

    public void Stopped()
    {
        dot.Fill = Brush("Bad");
        name.Foreground = Brush("Ink");
    }
}
