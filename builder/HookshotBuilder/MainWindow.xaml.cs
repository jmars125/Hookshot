using System.Diagnostics;
using System.IO;
using Path = System.Windows.Shapes.Path;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;

namespace HookshotBuilder;

/// The installer: one 960x600 window. It opens with the hookshot in the middle while it looks for the game files, then walks
/// through Game files > Build > Play. The game's update offer (Updater.cs) opens the same window on its update screen.
public partial class MainWindow : Window
{
    const long NeededBytes = 60L << 30;
    Language lang = Strings.Default();
    string T(S s) => (int)s < lang.Text.Length ? lang.Text[(int)s] : Strings.All[0].Text[(int)s];

    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch elapsed = new();
    readonly Stopwatch compileClock = new();
    readonly Dictionary<string, StepRow> rows = new();
    readonly Dictionary<string, (bool Ok, string Text)?> checks = new(StringComparer.OrdinalIgnoreCase);
    readonly List<Dump> dumps = new();
    readonly System.Text.StringBuilder log = new();
    Runner? runner;
    string? current;
    string finished = "";
    string? chosenNup, romfs, exefs;
    bool updating, checking;
    double compiled;
    int filesDone, filesOf;
    string lastFailure = "";

    public MainWindow()
    {
        InitializeComponent();
        OutPath.Text = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "Hookshot");
        LanguageBox.ItemsSource = Strings.All;
        LanguageBox.SelectedItem = lang;
        clock.Tick += (_, _) => Tick();
        Drop += Dropped;
        SourceInitialized += (_, _) =>
        {
            if (!Glass.Apply(this))   // (Windows 10, or no acrylic: plain Slate)
            {
                Background = (Brush)FindResource("Page");
                Root.Background = (Brush)FindResource("Page");
            }
        };
        Loaded += (_, _) => Start();
        var tools = Tools.Find();
        ToolsNote.Text = tools != null
            ? "Nothing else to install: Hookshot's own build tools are here, downloaded and checked by its setup."
            : "Hookshot's build tools are missing from its folder. Press Build and Hookshot offers to download them again.";
        SetStep(0);
        Refresh();
    }

    // ================= the start =================

    async void Start()
    {
        Animate();
        var started = Stopwatch.StartNew();
        if (Environment.GetCommandLineArgs().Contains("--update"))
        {
            SplashText.Text = "Getting the update…";
            await Task.Delay(900);
            ShowMain();
            Updater.FromCommandLine(this);
            return;
        }
        SplashText.Text = "Looking for your game files…";
        var found = await Task.Run(Finder.FindDumps);
        dumps.AddRange(found);
        SplashText.Text = found.Count switch { 0 => "No game files found yet", 1 => "Found 1 game file", _ => $"Found {found.Count} game files" };
        var wait = 1700 - (int)started.ElapsedMilliseconds;
        if (wait > 0) await Task.Delay(wait);
        FillDumps();
        ShowMain();
        if (Environment.GetEnvironmentVariable("HOOKSHOT_UI_PREVIEW") is { Length: > 0 } preview) { Preview(preview); return; }
        CheckAll();
    }

    /// (development) HOOKSHOT_UI_PREVIEW=build|ready|problem|update: one screen with made-up progress, nothing run
    void Preview(string what)
    {
        switch (what)
        {
            case "build":
                MakeRows(update: false);
                SetStep(1);
                ShowPage(BuildPage);
                rows["read"].Begin(); rows["read"].Done(); rows["read"].Small("Version checked");
                rows["lift"].Begin(); rows["lift"].Done();
                rows["compile"].Begin(); rows["compile"].Small("31 of 58 files");
                SetProgress(0.54);
                BuildNote.Text = "About 19 minutes left · your PC stays usable";
                break;
            case "ready":
                finished = OutPath.Text;
                ShowReady();
                break;
            case "problem":
                ShowProblem("Files of the builder's own compiler are missing (clang.exe in tools\\llvm-mingw\\bin).", tools: true);
                break;
            case "update":
                BeginUpdate(OutPath.Text);
                rows["download"].Done(); rows["download"].Small("3 MB · build tools unchanged");
                rows["read"].Begin(); rows["read"].Done();
                rows["lift"].Begin(); rows["lift"].Done();
                rows["compile"].Begin(); rows["compile"].Small("7 of 516 files");
                SetProgress(0.62);
                BuildNote.Text = "About 3 minutes left. Your current game stays as it is until the new one is complete.";
                break;
        }
    }

    void Animate()
    {
        var forever = RepeatBehavior.Forever;
        var ease = new SineEase { EasingMode = EasingMode.EaseInOut };
        HaloScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(0.94, 1.04, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        HaloScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(0.94, 1.04, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        Halo.BeginAnimation(OpacityProperty, new DoubleAnimation(0.6, 1, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        BobLift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, -6, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        BobTurn.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(-4, 2, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        Sweep.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(-88, 208, TimeSpan.FromSeconds(1.2)) { RepeatBehavior = forever, EasingFunction = ease });
    }

    /// The hookshot fades out and the first step fades in, in the same window
    void ShowMain()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(450));
        fade.Completed += (_, _) => { Splash.Visibility = Visibility.Collapsed; StopSplash(); };
        Splash.BeginAnimation(OpacityProperty, fade);
        SplashScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(1.04, TimeSpan.FromMilliseconds(500)));
        SplashScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(1.04, TimeSpan.FromMilliseconds(500)));
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Main.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(500)) { BeginTime = TimeSpan.FromMilliseconds(150) });
        MainLift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(600)) { BeginTime = TimeSpan.FromMilliseconds(150), EasingFunction = ease });
        Main.IsHitTestVisible = true;
    }

    void StopSplash()
    {
        foreach (var (o, p) in new (Animatable, DependencyProperty)[] { (HaloScale, ScaleTransform.ScaleXProperty), (HaloScale, ScaleTransform.ScaleYProperty),
                     (BobLift, TranslateTransform.YProperty), (BobTurn, RotateTransform.AngleProperty), (Sweep, TranslateTransform.XProperty) })
            o.BeginAnimation(p, null);
        Halo.BeginAnimation(OpacityProperty, null);
    }

    // ================= the step pill: Game files > Build > Play =================

    void SetStep(int on)
    {
        StepItems.Children.Clear();
        var names = new[] { "Game files", "Build", "Play" };
        for (var i = 0; i < names.Length; i++)
        {
            if (i > 0)
                StepItems.Children.Add(new Path { Data = Geometry.Parse("M0,0 L4,4 L0,8"), Stroke = (Brush)FindResource("InkSoft"), StrokeThickness = 1.6,
                                                  Opacity = 0.45, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(5, 0, 5, 0) });
            var done = i < on;
            var text = new TextBlock { Text = names[i], FontSize = 13, VerticalAlignment = VerticalAlignment.Center,
                                       FontWeight = i == on ? FontWeights.SemiBold : FontWeights.Medium,
                                       Foreground = (Brush)FindResource(i == on ? "Ink" : done ? "InkSoft" : "InkFaint") };
            var inner = new StackPanel { Orientation = Orientation.Horizontal };
            if (done)
            {
                var tick = new Grid { Width = 22, Height = 22, Margin = new Thickness(0, 0, 8, 0) };
                tick.Children.Add(new Ellipse { Fill = (Brush)FindResource("GoSoft") });
                tick.Children.Add(new Path { Data = Geometry.Parse("M0,4 L3.5,7.5 L10,0"), Stroke = (Brush)FindResource("Go"), StrokeThickness = 1.8,
                                             HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
                inner.Children.Add(tick);
            }
            inner.Children.Add(text);
            var pill = new Border { CornerRadius = new CornerRadius(99), Padding = done ? new Thickness(6, 5, 14, 5) : new Thickness(14, 5, 14, 5), Child = inner };
            if (i == on)
            {
                text.Foreground = Brushes.White;
                pill.Background = new LinearGradientBrush(Color.FromRgb(0x4B, 0x70, 0xF0), Color.FromRgb(0x35, 0x57, 0xD6), 90);
                pill.Effect = new DropShadowEffect { Color = Color.FromRgb(0x3D, 0x63, 0xE6), BlurRadius = 18, ShadowDepth = 6, Direction = 270, Opacity = 0.6 };
            }
            StepItems.Children.Add(pill);
        }
        StepPill.Visibility = Visibility.Visible;
        VersionChip.Visibility = Visibility.Collapsed;
    }

    void ShowPage(FrameworkElement page)
    {
        foreach (var p in new FrameworkElement[] { FilesPage, BuildPage, ReadyPage, ProblemPage })
            p.Visibility = p == page ? Visibility.Visible : Visibility.Collapsed;
        page.Opacity = 0;
        page.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(250)));
    }

    // ================= 1: game files =================

    void FillDumps()
    {
        DumpList.Children.Clear();
        if (dumps.Count == 0)
        {
            DumpList.Children.Add(new TextBlock
            {
                Text = "No .nup files in Downloads, Desktop, Documents or the top folders of your drives. Browse to yours, or drop it on this window.",
                Style = (Style)FindResource("Note"), Margin = new Thickness(16, 14, 16, 14),
            });
            return;
        }
        for (var i = 0; i < dumps.Count; i++)
        {
            var d = dumps[i];
            if (i > 0) DumpList.Children.Add(new Border { Height = 1, Background = (Brush)FindResource("GlassLine") });
            var name = new TextBlock { Text = System.IO.Path.GetFileName(d.Path), FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
            var meta = new TextBlock { Text = $"{d.Where} · {Finder.Size(d.Size)} · {Finder.Ago(d.Modified)}", Style = (Style)FindResource("Faint") };
            var texts = new StackPanel { Children = { name, meta }, Margin = new Thickness(0, 0, 12, 0) };
            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition(), new ColumnDefinition { Width = GridLength.Auto } } };
            var chip = Chip(d.Path);
            Grid.SetColumn(chip, 1);
            grid.Children.Add(texts);
            grid.Children.Add(chip);
            var row = new RadioButton { Style = (Style)FindResource("DumpRow"), GroupName = "Dumps", Content = grid, Tag = d.Path, ToolTip = d.Path,
                                        IsChecked = string.Equals(d.Path, chosenNup, StringComparison.OrdinalIgnoreCase) };
            row.Checked += (_, _) => Choose(d.Path);
            DumpList.Children.Add(row);
        }
    }

    Border Chip(string path)
    {
        checks.TryGetValue(path, out var c);
        var (text, brush) = c == null ? ("Checking…", "InkFaint")
            : c.Value.Ok ? ("✓ Supported version", "Go")
            : c.Value.Text.StartsWith("This is build") ? ("Not the supported version", "Warn")
            : ("Can't be read", "Bad");
        var chip = new Border { Style = (Style)FindResource("Chip"), Child = new TextBlock { Text = text, FontSize = 12, Foreground = (Brush)FindResource(brush) } };
        if (c is { Ok: false }) chip.ToolTip = c.Value.Text;
        if (c is { Ok: true }) chip.Background = (Brush)FindResource("GoSoft");
        return chip;
    }

    /// Each found file's version, one at a time (the check unpacks into one probe folder); the first good one is chosen
    async void CheckAll()
    {
        if (checking) return;
        checking = true;
        var tools = Tools.Find();
        while (tools != null && dumps.FirstOrDefault(d => !checks.ContainsKey(d.Path)) is { } d)
        {
            checks[d.Path] = null;
            FillDumps();
            var r = await Runner.Check(tools, new[] { "--check", "--nup", d.Path });
            checks[d.Path] = r;
            if (!r.Ok && r.Text.Contains("antivirus", StringComparison.OrdinalIgnoreCase))
            {
                checking = false;
                ShowProblem(r.Text, tools: true);
                return;
            }
            if (chosenNup == null && r.Ok) chosenNup = d.Path;
            FillDumps();
            Refresh();
        }
        if (chosenNup == null && dumps.Count > 0 && tools != null) { chosenNup = dumps[0].Path; FillDumps(); }
        checking = false;
        Refresh();
    }

    void Choose(string path)
    {
        chosenNup = path;
        romfs = exefs = null;
        Refresh();
    }

    void AddDump(string path, string where)
    {
        var info = new FileInfo(path);
        if (!dumps.Any(d => string.Equals(d.Path, info.FullName, StringComparison.OrdinalIgnoreCase)))
            dumps.Insert(0, new Dump(info.FullName, where, info.Length, info.LastWriteTime));
        chosenNup = info.FullName;
        romfs = exefs = null;
        FillDumps();
        CheckAll();
        Refresh();
    }

    bool SourceReady => romfs != null && exefs != null
        || chosenNup != null && checks.TryGetValue(chosenNup, out var c) && c is { Ok: true };

    void PathsChanged(object sender, RoutedEventArgs e) => Refresh();

    void Refresh()
    {
        if (BuildButton == null) return;
        BuildButton.IsEnabled = SourceReady && OutPath.Text.Length > 2;
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(OutPath.Text))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            SpaceBar.Value = Math.Min(1, NeededBytes / (double)Math.Max(1, free));
            SpaceBar.Foreground = (Brush)FindResource(free >= NeededBytes ? "Go" : "Bad");
            SpaceText.Text = $"Needs 60 GB · {root.TrimEnd('\\')} has {free >> 30} GB free";
            SpaceText.Foreground = (Brush)FindResource(free >= NeededBytes ? "InkSoft" : "Bad");
        }
        catch (Exception)
        {
            SpaceText.Text = "Needs 60 GB";
        }
    }

    void Dropped(object sender, DragEventArgs e)
    {
        if (runner != null || e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        foreach (var p in paths)
        {
            if (p.EndsWith(".nup", StringComparison.OrdinalIgnoreCase) && File.Exists(p)) AddDump(p, "Dropped");
            else if (Directory.Exists(System.IO.Path.Combine(p, "romfs")) && Directory.Exists(System.IO.Path.Combine(p, "exefs")))
                UseFolders(System.IO.Path.Combine(p, "romfs"), System.IO.Path.Combine(p, "exefs"));
        }
    }

    void BrowseNup(object sender, RoutedEventArgs e)
    {
        var d = new OpenFileDialog { Filter = "Game file (*.nup)|*.nup", Title = "Choose your game file" };
        if (d.ShowDialog(this) == true) AddDump(d.FileName, "Chosen");
    }

    /// The other way in: an unpacked game, its romfs and exefs folders (or one folder holding both)
    void BrowseFolders(object sender, RoutedEventArgs e)
    {
        var both = PickFolder("Choose the folder with romfs and exefs (or the romfs folder)");
        if (both == null) return;
        if (Directory.Exists(System.IO.Path.Combine(both, "romfs")) && Directory.Exists(System.IO.Path.Combine(both, "exefs")))
        {
            UseFolders(System.IO.Path.Combine(both, "romfs"), System.IO.Path.Combine(both, "exefs"));
            return;
        }
        var ex = PickFolder("Now choose the exefs folder");
        if (ex != null) UseFolders(both, ex);
    }

    void UseFolders(string r, string x)
    {
        if (!File.Exists(System.IO.Path.Combine(x, "main")))
        {
            MessageBox.Show(this, "That exefs folder has no file named main.", "Hookshot");
            return;
        }
        romfs = r;
        exefs = x;
        chosenNup = null;
        foreach (var rb in DumpList.Children.OfType<RadioButton>()) rb.IsChecked = false;
        DumpList.Children.Insert(0, new TextBlock { Text = $"Using the folders {r} and {x}", Style = (Style)FindResource("Note"), Margin = new Thickness(16, 12, 16, 12) });
        Refresh();
    }

    static string? PickFolder(string title)
    {
        var d = new OpenFolderDialog { Title = title };
        return d.ShowDialog() == true ? d.FolderName : null;
    }

    void BrowseOut(object sender, RoutedEventArgs e) => OutPath.Text = PickFolder("Choose where the game goes") ?? OutPath.Text;

    void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LanguageBox.SelectedItem is not Language l) return;
        lang = l;
        CancelButton.Content = T(S.Cancel);
        BackButton.Content = T(S.Back);
        OpenButton.Content = T(S.OpenFolder);
        PlayText.Text = T(S.Play);
        ShowLog.Content = T(ShowLog.IsChecked == true ? S.HideDetails : S.ShowDetails);
    }

    // ================= 2: the build =================

    void StartBuild(object sender, RoutedEventArgs e)
    {
        var tools = Tools.Find();
        if (tools == null)
        {
            ShowProblem("The build tools are not in Hookshot's folder.", tools: true);
            return;
        }
        var args = new List<string>();
        if (romfs != null && exefs != null) args.AddRange(new[] { "--romfs", romfs, "--exefs", exefs });
        else if (chosenNup != null) args.AddRange(new[] { "--nup", chosenNup });
        else return;
        args.AddRange(new[] { "--out", OutPath.Text, "--language", lang.Code });
        Run(tools, args);
    }

    static readonly (string Id, string Build, string Update)[] Stages =
    {
        ("download", "", "Download the update"),
        ("read", "Read your game files", "Read the game files kept in the folder"),
        ("lift", "Translate the game to PC code", "Translate the game to PC code"),
        ("compile", "Compile", "Compile changed files"),
        ("finish", "Put the game together", "Swap in the new game"),
    };
    static readonly Dictionary<string, (double From, double Size)> Weight = new()
    {
        ["download"] = (0, 0.04), ["read"] = (0.04, 0.04), ["lift"] = (0.08, 0.14), ["compile"] = (0.22, 0.74), ["finish"] = (0.96, 0.04),
    };

    void MakeRows(bool update)
    {
        StageList.Children.Clear();
        rows.Clear();
        var list = Stages.Where(s => update || s.Id != "download").ToList();
        for (var i = 0; i < list.Count; i++)
        {
            var row = new StepRow(i + 1, update ? list[i].Update : list[i].Build, last: i == list.Count - 1);
            rows[list[i].Id] = row;
            StageList.Children.Add(row.Root);
        }
        if (update) rows["read"].Small("Your game files are kept: the .nup isn't needed");
    }

    void Run(Tools tools, List<string> args, bool keepRows = false)
    {
        if (!keepRows)
        {
            updating = false;
            MakeRows(update: false);
            SetStep(1);
            BuildTitle.Text = "Building your game";
            UpdateBar.Visibility = Visibility.Collapsed;
            RingBox.Visibility = Visibility.Visible;
        }
        log.Clear();
        Log.Clear();
        current = null;
        finished = "";
        compiled = 0;
        filesDone = filesOf = 0;
        ShowPage(BuildPage);
        CancelButton.Visibility = Visibility.Visible;
        SetProgress(updating ? Weight["read"].From : 0);
        elapsed.Restart();
        compileClock.Reset();
        clock.Start();
        Tick();
        runner = new Runner(tools, args);
        runner.Line += l => Dispatcher.BeginInvoke(() => Line(l));
        runner.Finished += failure => Dispatcher.BeginInvoke(() => Ended(failure));
        runner.Start();
    }

    static readonly Regex FilesLine = new(@"^(\d+) files, "), FileLine = new(@"^\s+\S+\s+\d+ MB\s+\d+s");

    void Line(string line)
    {
        if (line.StartsWith("@@stage "))
        {
            var id = line.Split(' ')[1];
            if (current != null && rows.TryGetValue(current, out var was)) was.Done();
            current = id;
            if (rows.TryGetValue(id, out var row)) row.Begin();
            if (id == "compile") compileClock.Restart();
            if (Weight.TryGetValue(id, out var w)) SetProgress(w.From);
        }
        else if (line.StartsWith("@@progress "))
        {
            if (double.TryParse(line[11..], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f) && current != null
                && Weight.TryGetValue(current, out var w))
            {
                if (current == "compile") compiled = f;
                SetProgress(w.From + w.Size * Math.Clamp(f, 0, 1));
            }
            return;
        }
        else if (line.StartsWith("@@done ")) finished = line[7..];
        else if (FilesLine.Match(line) is { Success: true } m) filesOf = int.Parse(m.Groups[1].Value);
        else if (FileLine.IsMatch(line)) filesDone++;
        if (current == "compile" && filesOf > 0 && rows.TryGetValue("compile", out var c)) c.Small($"{filesDone} of {filesOf} files");
        AddLog(line);
    }

    void AddLog(string line)
    {
        log.AppendLine(line);
        if (log.Length > 600_000) log.Remove(0, log.Length - 400_000);
        if (Log.Visibility == Visibility.Visible)
        {
            Log.AppendText(line + "\n");
            Log.ScrollToEnd();
        }
    }

    void SetProgress(double f)
    {
        f = Math.Clamp(f, 0, 1);
        RingText.Text = $"{(int)(f * 100)}%";
        UpdateBar.Value = f;
        const double r = 55, c = 60;
        if (f >= 0.999) { RingArc.Data = new EllipseGeometry(new Point(c, c), r, r); return; }
        var a = f * 2 * Math.PI;
        var end = new Point(c + r * Math.Sin(a), c - r * Math.Cos(a));
        var fig = new PathFigure { StartPoint = new Point(c, c - r), IsClosed = false };
        fig.Segments.Add(new ArcSegment(end, new Size(r, r), 0, f > 0.5, SweepDirection.Clockwise, true));
        RingArc.Data = f < 0.002 ? null : new PathGeometry(new[] { fig });
    }

    void Tick()
    {
        foreach (var r in rows.Values) r.Tick();
        if (runner == null) return;
        string left;
        if (current == "compile" && compiled > 0.03)
        {
            var rest = compileClock.Elapsed.TotalSeconds / compiled * (1 - compiled) + 60;
            left = rest > 90 ? $"About {(int)Math.Round(rest / 60)} minutes left" : "About a minute left";
        }
        else left = updating ? "Working…" : "Usually about 45 minutes";
        BuildNote.Text = updating ? $"{left}. Your current game stays as it is until the new one is complete."
                                  : $"{left} · your PC stays usable";
    }

    void Ended(string? failure)
    {
        clock.Stop();
        elapsed.Stop();
        runner = null;
        if (finished.Length > 0)
        {
            foreach (var r in rows.Values) r.Done();
            SetProgress(1);
            ShowReady();
            return;
        }
        if (failure == "cancelled")
        {
            SetStep(0);
            ShowPage(FilesPage);
            return;
        }
        if (current != null && rows.TryGetValue(current, out var row)) row.Stopped();
        ShowProblem(failure ?? "The build tools stopped without saying why.", tools: failure?.Contains("antivirus", StringComparison.OrdinalIgnoreCase) == true);
    }

    void LogToggled(object sender, RoutedEventArgs e)
    {
        var show = ShowLog.IsChecked == true;
        Log.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        StepCard.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        ShowLog.Content = T(show ? S.HideDetails : S.ShowDetails);
        if (show)
        {
            Log.Text = log.ToString();
            Log.ScrollToEnd();
        }
    }

    void CancelBuild(object sender, RoutedEventArgs e)
    {
        download?.Cancel();
        runner?.Cancel();
    }

    // ================= 3: play =================

    void ShowReady()
    {
        SetStep(2);
        ReadyTitle.Text = updating ? "Your game is updated" : "Your game is ready";
        ReadyNote.Text = $"{(updating ? "Updated" : "Built")} in {Span(elapsed.Elapsed)} · {finished}";
        if (!updating && Environment.GetEnvironmentVariable("HOOKSHOT_UI_PREVIEW") == null)
            Finder.Shortcuts(finished, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
        ShowPage(ReadyPage);
    }

    void ShortcutsChanged(object sender, RoutedEventArgs e)
    {
        if (finished.Length > 0) Finder.Shortcuts(finished, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
    }

    static string Span(TimeSpan t) => t.TotalHours >= 1 ? $"{(int)t.TotalHours} h {t.Minutes} min" : t.TotalMinutes >= 1 ? $"{(int)t.TotalMinutes} minutes" : $"{t.Seconds} seconds";

    void OpenFolder(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{finished}\"") { UseShellExecute = true });

    void Play(object sender, RoutedEventArgs e)
    {
        Process.Start(new ProcessStartInfo(System.IO.Path.Combine(finished, "hookshot.exe")) { UseShellExecute = true, WorkingDirectory = finished });
        WindowState = WindowState.Minimized;
    }

    // ================= when something goes wrong =================

    bool problemIsTools;

    void ShowProblem(string why, bool tools)
    {
        lastFailure = why;
        problemIsTools = tools;
        ProblemSteps.Children.Clear();
        ProblemSub.Text = updating ? "Your current game is unchanged and still plays." : "Nothing was changed in your game folder.";
        if (tools)
        {
            ProblemTitle.Text = "The build tools need repairing";
            ProblemHead.Text = "Your antivirus removed part of the build tools";
            ProblemText.Text = why + "\n\nAntivirus programs sometimes flag unsigned developer tools by mistake.";
            var base_ = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HookshotBuilder");
            foreach (var (n, s) in new[]
                     {
                         (1, "Open Windows Security → Protection history, and restore the removed files."),
                         (2, $"Or add {base_} to its exclusions."),
                         (3, "Then press Repair: Hookshot downloads the tools again from their official releases and checks each one."),
                     })
                ProblemSteps.Children.Add(new TextBlock { Text = $"{n}.  {s}", Style = (Style)FindResource("Note"), FontSize = 14, Margin = new Thickness(0, 6, 0, 0) });
        }
        else
        {
            ProblemTitle.Text = updating ? "The update stopped" : "The build stopped";
            ProblemHead.Text = "What went wrong";
            ProblemText.Text = why;
        }
        RepairButton.Visibility = tools ? Visibility.Visible : Visibility.Collapsed;
        ProblemDetails.Content = "Show details";
        if (!updating) SetStep(1);   // (the update keeps its versions in the head)
        ShowPage(ProblemPage);
    }

    void ProblemShowDetails(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        ProblemSteps.Children.Clear();
        ProblemSteps.Children.Add(new TextBox
        {
            Text = string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 80))), IsReadOnly = true, FontFamily = (FontFamily)FindResource("Mono"), FontSize = 11,
            Foreground = (Brush)FindResource("InkSoft"), Background = new SolidColorBrush(Color.FromArgb(0x33, 0, 0, 0)), BorderThickness = new Thickness(0),
            Padding = new Thickness(8), TextWrapping = TextWrapping.NoWrap, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
        });
    }

    void CopyForReport(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        var text = $"Hookshot Builder {Tools.Find()?.Version ?? "(source)"} on Windows {Environment.OSVersion.Version}\n" +
                   $"{ProblemTitle.Text}: {lastFailure}\n\n" + string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 200)));
        try { Clipboard.SetText(text); CopyReport.Content = "Copied"; } catch (Exception) { }
    }

    void Back(object sender, RoutedEventArgs e)
    {
        SetStep(0);
        ShowPage(FilesPage);
    }

    /// The setup (HookshotBuilder-Setup.exe beside this app) checks every part against its checksum and downloads the missing ones
    void Repair(object sender, RoutedEventArgs e)
    {
        var setup = System.IO.Path.Combine(AppContext.BaseDirectory, "HookshotBuilder-Setup.exe");
        if (File.Exists(setup))
        {
            Process.Start(new ProcessStartInfo(setup, "--repair") { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory });
            Application.Current.Shutdown();
            return;
        }
        Process.Start(new ProcessStartInfo("https://github.com/NotRexed/Hookshot/releases/latest") { UseShellExecute = true });
    }

    // ================= the update (Updater.cs): the game started this app with --update <folder> =================

    CancellationTokenSource? download;

    public void BeginUpdate(string folder)
    {
        updating = true;
        OutPath.Text = folder;
        finished = "";
        var a = Environment.GetCommandLineArgs();
        var i = Array.IndexOf(a, "--tag");
        var to = i >= 0 && i + 1 < a.Length ? a[i + 1] : "the latest version";
        var from = Tools.Find()?.Version;
        StepPill.Visibility = Visibility.Collapsed;
        VersionChip.Visibility = Visibility.Visible;
        VersionChipText.Text = from != null && from != to ? $"{from} → {to}" : to;
        MakeRows(update: true);
        BuildTitle.Text = "Updating your game";
        BuildNote.Text = "Getting the update…";
        RingBox.Visibility = Visibility.Collapsed;
        UpdateBar.Visibility = Visibility.Visible;
        SetProgress(0);
        rows["download"].Begin();
        CancelButton.Visibility = Visibility.Visible;
        ShowPage(BuildPage);
    }

    public CancellationToken DownloadToken => (download = new CancellationTokenSource()).Token;

    public void DownloadProgress(double f)
    {
        rows.GetValueOrDefault("download")?.Small($"{(int)(f * 100)}%");
        SetProgress(Weight["download"].Size * f);
    }

    public void Note(string line)
    {
        AddLog(line);
        rows.GetValueOrDefault("download")?.Small(line);
    }

    public void DownloadFailed(string why)
    {
        download = null;
        rows.GetValueOrDefault("download")?.Stopped();
        ShowProblem(why, tools: false);
        ProblemTitle.Text = "The update could not be downloaded";
    }

    /// The new tools are here: the folder built again with them (only what changed is compiled)
    internal void UpdateBuild(Tools tools, string folder)
    {
        download = null;
        if (rows.TryGetValue("download", out var d)) { d.Done(); d.Small("Build tools checked"); }
        Run(tools, new List<string> { "--update", folder }, keepRows: true);
    }

    // ================= the window =================

    void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    void CloseWindow(object sender, RoutedEventArgs e)
    {
        if (runner != null && MessageBox.Show(this, T(S.CloseQuestion), "Hookshot", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        runner?.Cancel();
        Close();
    }
}

/// One step of the build: its mark (done, now, waiting), its name, a line under it and its time.
sealed class StepRow
{
    public readonly Grid Root = new() { Margin = new Thickness(0, 10, 0, 10) };
    readonly Grid mark = new() { Width = 22, Height = 22, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };
    readonly Ellipse disc = new(), halo = new() { Width = 32, Height = 32, Margin = new Thickness(-5) };
    readonly TextBlock number = new() { FontSize = 12, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    readonly Path tick = new() { Data = Geometry.Parse("M0,4 L3.5,7.5 L10,0"), StrokeThickness = 1.8, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    readonly TextBlock name = new() { FontWeight = FontWeights.SemiBold };
    readonly TextBlock small = new() { FontSize = 12, Visibility = Visibility.Collapsed, TextTrimming = TextTrimming.CharacterEllipsis };
    readonly TextBlock time = new() { FontSize = 12, Text = "—", VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 2, 0, 0) };
    readonly Stopwatch watch = new();
    readonly int n;
    bool running;

    static Brush B(string key) => (Brush)Application.Current.FindResource(key);

    public StepRow(int n, string text, bool last)
    {
        this.n = n;
        Root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(34) });
        Root.ColumnDefinitions.Add(new ColumnDefinition());
        Root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        mark.Children.Add(halo);
        mark.Children.Add(disc);
        mark.Children.Add(number);
        mark.Children.Add(tick);
        tick.Stroke = B("Go");
        time.FontFamily = (FontFamily)Application.Current.FindResource("Mono");
        var texts = new StackPanel { Children = { name, small } };
        Grid.SetColumn(texts, 1);
        Grid.SetColumn(time, 2);
        if (!last)
        {
            var link = new Rectangle { Width = 1, Fill = B("GlassLine"), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(10.5, 28, 0, -19) };
            Root.Children.Add(link);
        }
        Root.Children.Add(mark);
        Root.Children.Add(texts);
        Root.Children.Add(time);
        name.Text = text;
        Wait();
    }

    public void Small(string text)
    {
        small.Text = text;
        small.Visibility = Visibility.Visible;
    }

    void Wait()
    {
        disc.Fill = Brushes.Transparent;
        disc.Stroke = B("GlassLineStrong");
        disc.StrokeThickness = 1.5;
        halo.Fill = Brushes.Transparent;
        number.Text = "";
        tick.Visibility = Visibility.Collapsed;
        name.Foreground = B("InkFaint");
        name.FontWeight = FontWeights.Medium;
        small.Foreground = B("InkFaint");
        time.Foreground = B("InkFaint");
    }

    public void Begin()
    {
        running = true;
        watch.Restart();
        disc.Fill = B("Brand");
        disc.Stroke = null;
        halo.Fill = B("BrandSoft");
        number.Text = n.ToString();
        number.Foreground = Brushes.White;
        tick.Visibility = Visibility.Collapsed;
        name.Foreground = B("Ink");
        name.FontWeight = FontWeights.SemiBold;
        time.Foreground = B("Ink");
        Tick();
    }

    public void Tick()
    {
        if (running) time.Text = $"{(int)watch.Elapsed.TotalMinutes}:{watch.Elapsed.Seconds:00}";
    }

    public void Done()
    {
        if (running) Tick();
        running = false;
        watch.Stop();
        disc.Fill = B("GoSoft");
        disc.Stroke = null;
        halo.Fill = Brushes.Transparent;
        number.Text = "";
        tick.Visibility = Visibility.Visible;
        name.Foreground = B("Ink");
        name.FontWeight = FontWeights.SemiBold;
        time.Foreground = B("InkFaint");
    }

    public void Stopped()
    {
        running = false;
        watch.Stop();
        disc.Fill = B("BadSoft");
        halo.Fill = Brushes.Transparent;
        number.Text = "!";
        number.Foreground = B("Bad");
    }
}
