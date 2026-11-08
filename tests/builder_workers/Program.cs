using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using HookshotBuilder;

static class Program
{
    const string Key = "HOOKSHOT_JOBS";
    static readonly MethodInfo InfoMethod = typeof(Runner).GetMethod("Info", BindingFlags.Static | BindingFlags.NonPublic)!;
    static int checks;

    static void Require(bool ok, string message)
    {
        if (!ok) throw new Exception(message);
        checks++;
    }

    static ProcessStartInfo Info(Tools tools, string[] args, int? workers = null) =>
        (ProcessStartInfo)InfoMethod.Invoke(null, new object?[] { tools, args, workers })!;

    static async Task<JsonElement> Delivery(ProcessStartInfo info)
    {
        using var child = Process.Start(info)!;
        var output = await child.StandardOutput.ReadToEndAsync();
        var error = await child.StandardError.ReadToEndAsync();
        await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
        Require(child.ExitCode == 0 && error.Length == 0, "The authored argument/environment child failed.");
        using var json = JsonDocument.Parse(output.Trim());
        return json.RootElement.Clone();
    }

    static void CheckInfo(ProcessStartInfo info, Tools tools, string[] args, string? expected)
    {
        Require(info.ArgumentList.SequenceEqual(new[] { "-I", Path.Combine(tools.Root, "nx2", "make_game.py") }.Concat(args)),
            "The worker choice changed the original source/update arguments.");
        Require(info.WorkingDirectory == tools.Root && !info.UseShellExecute && info.CreateNoWindow
            && info.RedirectStandardOutput && info.RedirectStandardError, "Runner process options changed.");
        info.Environment.TryGetValue(Key, out var actual);
        Require(actual == expected, "Unexpected child worker override.");
    }

    static async Task EmptyParent(string python, Tools tools)
    {
        // Environment.SetEnvironmentVariable(Key, "") may remove the variable on Windows instead of keeping it empty.
        // Start a real parent with an explicit empty environment entry, then exercise the actual Runner.Info inheritance.
        Require(Environment.GetEnvironmentVariable(Key) == "", "The empty-parent fixture did not inherit an empty entry.");
        var info = Info(tools, new[] { "--out", "authored output" });
        CheckInfo(info, tools, new[] { "--out", "authored output" }, "");
        var value = await Delivery(info);
        Require(value.GetProperty("jobs").GetString() == "", "Automatic changed an inherited empty entry.");
    }

    static async Task Main(string[] args)
    {
        if (args.Length is < 1 or > 2) throw new Exception("Pass the Python executable for the authored child.");
        var emptyParent = args.Length == 2 && args[0] == "--empty-parent";
        var python = Path.GetFullPath(args[^1]);
        var original = Environment.GetEnvironmentVariable(Key);
        var root = Path.Combine(Path.GetTempPath(), "hookshot-workers-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "nx2"));
        try
        {
            // This is authored test content, not the real pipeline, game files or a compiler invocation.
            File.WriteAllText(Path.Combine(root, "nx2", "make_game.py"), """
                import json, os, sys
                text = json.dumps({'argv': sys.argv[1:], 'jobs': os.environ.get('HOOKSHOT_JOBS')})
                print(('@@check ok ' if '--check' in sys.argv else '') + text, flush=True)
                """);
            var tools = new Tools(root, python);
            if (emptyParent)
            {
                await EmptyParent(python, tools);
                Console.WriteLine("Empty inherited worker entry passed.");
                return;
            }

            Require(BuildWorkers.Choices(4).SequenceEqual(new object[] { BuildWorkers.Automatic, 1, 2, 3, 4 }),
                "The menu must offer Automatic first and one count per logical processor.");
            Require(BuildWorkers.Choices(0).SequenceEqual(new object[] { BuildWorkers.Automatic, 1 }), "CPU fallback must still offer one worker.");
            Require(BuildWorkers.Selected(BuildWorkers.Automatic) is null && BuildWorkers.Selected(3) == 3, "Worker selection failed.");
            foreach (var invalid in new object?[] { null, "4", "automatic", 0, -1, 2L, new object() })
            {
                try { BuildWorkers.Selected(invalid); throw new Exception("Invalid selection accepted."); }
                catch (ArgumentException e) { Require(e.Message.Contains("Automatic"), "Invalid choice needs an actionable message."); }
            }
            foreach (var invalid in new[] { 0, -1 })
            {
                try { new Runner(tools, new List<string>(), invalid); throw new Exception("Invalid worker count accepted."); }
                catch (ArgumentOutOfRangeException) { checks++; }
            }

            var sourceArguments = new[]
            {
                new[] { "--nup", "authored file with spaces.nup", "--out", "authored output", "--language", "en-US" },
                new[] { "--romfs", "authored romfs", "--exefs", "authored exefs", "--out", "authored output", "--language", "fr" },
                new[] { "--update", "authored existing game" },
            };
            foreach (var inherited in new string?[] { null, "4", "0" })
            {
                Environment.SetEnvironmentVariable(Key, inherited);
                foreach (var command in sourceArguments)
                {
                    var info = Info(tools, command);
                    CheckInfo(info, tools, command, inherited);
                    Require(Environment.GetEnvironmentVariable(Key) == inherited, "Automatic changed its parent environment.");
                }
            }
            foreach (var inherited in new[] { "2", "not a number" })
            {
                Environment.SetEnvironmentVariable(Key, inherited);
                var command = sourceArguments[0];
                var info = Info(tools, command, 4);
                CheckInfo(info, tools, command, "4");
                var value = await Delivery(info);
                Require(value.GetProperty("jobs").GetString() == "4", "The selected worker count did not reach the child.");
                Require(value.GetProperty("argv").EnumerateArray().Select(e => e.GetString()).SequenceEqual(command), "Authored child arguments changed.");
                Require(Environment.GetEnvironmentVariable(Key) == inherited, "Manual selection changed the parent environment.");
            }

            Environment.SetEnvironmentVariable(Key, "6");
            var check = await Runner.Check(tools, new[] { "--check", "--nup", "authored check" });
            Require(check.Ok, "Runner.Check no longer recognizes its real protocol.");
            using (var value = JsonDocument.Parse(check.Text))
                Require(value.RootElement.GetProperty("jobs").GetString() == "6", "Check must retain the inherited override.");
            Require(Environment.GetEnvironmentVariable(Key) == "6", "Check changed its parent environment.");

            var completed = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
            string? delivered = null;
            var runner = new Runner(tools, sourceArguments[2].ToList(), 3);
            runner.Line += line => delivered = line;
            runner.Finished += failure => completed.TrySetResult(failure);
            runner.Start();
            Require(await completed.Task.WaitAsync(TimeSpan.FromSeconds(15)) is null, "Actual Runner.Start did not finish its authored child.");
            using (var value = JsonDocument.Parse(delivered!))
                Require(value.RootElement.GetProperty("jobs").GetString() == "3", "Runner.Start dropped the selected count.");
            Require(Environment.GetEnvironmentVariable(Key) == "6", "Runner.Start changed its parent environment.");

            var self = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase))
                self.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
            self.ArgumentList.Add("--empty-parent"); self.ArgumentList.Add(python);
            self.Environment[Key] = "";
            using var empty = Process.Start(self)!;
            var emptyOutput = await empty.StandardOutput.ReadToEndAsync();
            var emptyError = await empty.StandardError.ReadToEndAsync();
            await empty.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
            Require(empty.ExitCode == 0 && emptyError.Length == 0 && emptyOutput.Contains("Empty inherited worker entry passed."), "Empty inheritance check failed: " + emptyError);
            Console.WriteLine($"Windows Builder worker checks passed ({checks} assertions). Only authored argv/HOOKSHOT_JOBS were inspected.");
        }
        finally
        {
            Environment.SetEnvironmentVariable(Key, original);
            Directory.Delete(root, recursive: true);
        }
    }
}
