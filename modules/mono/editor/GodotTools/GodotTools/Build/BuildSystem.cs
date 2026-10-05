using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Godot;
using GodotTools.BuildLogger;
using GodotTools.Export;
using GodotTools.Internals;
using GodotTools.Utils;
using Directory = GodotTools.Utils.Directory;

namespace GodotTools.Build
{
    public static class BuildSystem
    {
        internal static string ResolveOpenHarmonySigningTool()
        {
            var settings = EditorInterface.Singleton.GetEditorSettings();
            string? sdkRoot = settings.HasSetting("export/openharmony/sdk_root")
                ? settings.GetSetting("export/openharmony/sdk_root").AsString() : null;
            return OpenHarmonyExport.FindSignTool(sdkRoot) ?? throw new FileNotFoundException(
                "Cannot locate OpenHarmony binary-sign-tool. Install ohos-sdk-toolchains with oo, or select its SDK view in Export > OpenHarmony > SDK Root.");
        }

        internal static string ResolveOpenHarmonyNativeCompiler()
        {
            var settings = EditorInterface.Singleton.GetEditorSettings();
            string? sdkRoot = settings.HasSetting("export/openharmony/sdk_root")
                ? settings.GetSetting("export/openharmony/sdk_root").AsString() : null;
            return OpenHarmonyExport.FindNativeCompiler(sdkRoot) ?? throw new FileNotFoundException(
                "Cannot locate OpenHarmony NativeAOT clang. Install ohos-sdk-native with oo, or select its SDK view in Export > OpenHarmony > SDK Root.");
        }

        private static Process LaunchBuild(BuildInfo buildInfo, Action<string?>? stdOutHandler,
            Action<string?>? stdErrHandler)
        {
            string? dotnetPath = DotNetFinder.FindDotNetExe();

            if (dotnetPath == null)
                throw new FileNotFoundException("Cannot find the dotnet executable.");

            var editorSettings = EditorInterface.Singleton.GetEditorSettings();

            var startInfo = new ProcessStartInfo(dotnetPath);

            BuildArguments(buildInfo, startInfo.ArgumentList, editorSettings);
            if (Utils.OS.IsOpenHarmony && !buildInfo.OnlyClean &&
                (string.IsNullOrEmpty(buildInfo.RuntimeIdentifier) || buildInfo.RuntimeIdentifier.StartsWith("openharmony-", StringComparison.Ordinal)))
                OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, ResolveOpenHarmonySigningTool());

            string launchMessage = startInfo.GetCommandLineDisplay(new StringBuilder("Running: ")).ToString();
            stdOutHandler?.Invoke(launchMessage);
            if (Godot.OS.IsStdOutVerbose())
                Console.WriteLine(launchMessage);

            startInfo.RedirectStandardOutput = true;
            startInfo.RedirectStandardError = true;
            startInfo.UseShellExecute = false;
            startInfo.CreateNoWindow = true;
            startInfo.EnvironmentVariables["DOTNET_CLI_UI_LANGUAGE"]
                = ((string)EditorInterface.Singleton.GetEditorLanguage()).Replace('_', '-');

            if (OperatingSystem.IsWindows())
            {
                startInfo.StandardOutputEncoding = Encoding.UTF8;
                startInfo.StandardErrorEncoding = Encoding.UTF8;
            }

            // Needed when running from Developer Command Prompt for VS
            RemovePlatformVariable(startInfo.EnvironmentVariables);

            var process = new Process { StartInfo = startInfo };

            if (stdOutHandler != null)
                process.OutputDataReceived += (_, e) => stdOutHandler.Invoke(e.Data);
            if (stdErrHandler != null)
                process.ErrorDataReceived += (_, e) => stdErrHandler.Invoke(e.Data);

            process.Start();

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            return process;
        }

        public static int Build(BuildInfo buildInfo, Action<string?>? stdOutHandler, Action<string?>? stdErrHandler)
        {
            using (var process = LaunchBuild(buildInfo, stdOutHandler, stdErrHandler))
            {
                process.WaitForExit();

                return process.ExitCode;
            }
        }

        public static async Task<int> BuildAsync(BuildInfo buildInfo, Action<string?>? stdOutHandler,
            Action<string?>? stdErrHandler)
        {
            using (var process = LaunchBuild(buildInfo, stdOutHandler, stdErrHandler))
            {
                await process.WaitForExitAsync();

                return process.ExitCode;
            }
        }

        private static Process LaunchPublish(BuildInfo buildInfo, Action<string?>? stdOutHandler,
            Action<string?>? stdErrHandler)
        {
            string? dotnetPath = DotNetFinder.FindDotNetExe();

            if (dotnetPath == null)
                throw new FileNotFoundException("Cannot find the dotnet executable.");

            var editorSettings = EditorInterface.Singleton.GetEditorSettings();

            var startInfo = new ProcessStartInfo(dotnetPath);

            BuildPublishArguments(buildInfo, startInfo.ArgumentList, editorSettings);
            if (buildInfo.RuntimeIdentifier?.StartsWith("openharmony-", StringComparison.Ordinal) == true)
            {
                if (Utils.OS.IsOpenHarmony)
                {
                    OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, ResolveOpenHarmonySigningTool());
                    OpenHarmonyExport.ConfigureNativeAotEnvironment(startInfo, ResolveOpenHarmonyNativeCompiler());
                }
                RequireOpenHarmonyNativeAotSdk(buildInfo, startInfo);
            }

            string launchMessage = startInfo.GetCommandLineDisplay(new StringBuilder("Running: ")).ToString();
            stdOutHandler?.Invoke(launchMessage);
            if (Godot.OS.IsStdOutVerbose())
                Console.WriteLine(launchMessage);

            startInfo.RedirectStandardOutput = true;
            startInfo.RedirectStandardError = true;
            startInfo.UseShellExecute = false;
            startInfo.EnvironmentVariables["DOTNET_CLI_UI_LANGUAGE"]
                = ((string)EditorInterface.Singleton.GetEditorLanguage()).Replace('_', '-');

            if (OperatingSystem.IsWindows())
            {
                startInfo.StandardOutputEncoding = Encoding.UTF8;
                startInfo.StandardErrorEncoding = Encoding.UTF8;
            }

            // Needed when running from Developer Command Prompt for VS
            RemovePlatformVariable(startInfo.EnvironmentVariables);

            var process = new Process { StartInfo = startInfo };

            if (stdOutHandler != null)
                process.OutputDataReceived += (_, e) => stdOutHandler.Invoke(e.Data);
            if (stdErrHandler != null)
                process.ErrorDataReceived += (_, e) => stdErrHandler.Invoke(e.Data);

            process.Start();

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            return process;
        }

        private static void RequireOpenHarmonyNativeAotSdk(BuildInfo buildInfo, ProcessStartInfo publish)
        {
            var query = new ProcessStartInfo(publish.FileName)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            query.Environment.Clear();
            foreach (var variable in publish.Environment)
                query.Environment[variable.Key] = variable.Value;
            query.ArgumentList.Add("msbuild");
            query.ArgumentList.Add(buildInfo.Project);
            query.ArgumentList.Add("-p:Configuration=" + buildInfo.Configuration);
            query.ArgumentList.Add("-p:RuntimeIdentifier=" + buildInfo.RuntimeIdentifier);
            foreach (var property in buildInfo.CustomProperties)
                query.ArgumentList.Add("-p:" + (string)property);
            query.ArgumentList.Add("-getProperty:GodotOpenHarmonyNativeAotSupported");
            query.ArgumentList.Add("-verbosity:quiet");
            using var process = Process.Start(query) ?? throw new InvalidOperationException("Cannot query the project's Godot .NET SDK.");
            var stdout = process.StandardOutput.ReadToEndAsync();
            var stderr = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            string output = stdout.GetAwaiter().GetResult();
            string errors = stderr.GetAwaiter().GetResult();
            if (process.ExitCode != 0)
                throw new InvalidOperationException($"Cannot evaluate the Godot .NET SDK for OpenHarmony NativeAOT: {output}{errors}");
            string? supported = output.Split('\n', StringSplitOptions.RemoveEmptyEntries).LastOrDefault()?.Trim();
            if (!string.Equals(supported, "true", StringComparison.OrdinalIgnoreCase))
                throw new NotSupportedException("OpenHarmony NativeAOT export requires the Godot.NET.Sdk supplied by this editor (4.7.2-ohos.3 or newer), which preserves game scripts and GodotSharp. Update the project's Godot.NET.Sdk version and restore from the editor's local feed. Older .2 projects cannot be exported safely with AOT.");
        }

        public static int Publish(BuildInfo buildInfo, Action<string?>? stdOutHandler, Action<string?>? stdErrHandler)
        {
            using (var process = LaunchPublish(buildInfo, stdOutHandler, stdErrHandler))
            {
                process.WaitForExit();

                return process.ExitCode;
            }
        }

        private static void BuildArguments(BuildInfo buildInfo, Collection<string> arguments,
            EditorSettings editorSettings)
        {
            // `dotnet clean` / `dotnet build` commands
            arguments.Add(buildInfo.OnlyClean ? "clean" : "build");

            // C# Project
            string projectPath = buildInfo.Project;
            if (OperatingSystem.IsWindows())
            {
                // On Windows, when using a network share path, the path may start with "//"
                // which MSBuild will treat like a switch. Convert to "\\" to make it an absolute path.
                if (projectPath.StartsWith("//"))
                {
                    projectPath = "\\\\" + projectPath.Substring(2);
                }
            }
            arguments.Add(projectPath);

            // `dotnet clean` doesn't recognize these options
            if (!buildInfo.OnlyClean)
            {
                // Restore
                // `dotnet build` restores by default, unless requested not to
                if (!buildInfo.Restore)
                    arguments.Add("--no-restore");

                // Incremental or rebuild
                if (buildInfo.Rebuild)
                    arguments.Add("--no-incremental");
            }

            // Configuration
            arguments.Add("-c");
            arguments.Add(buildInfo.Configuration);

            // Verbosity
            AddVerbosityArguments(buildInfo, arguments, editorSettings);

            // Logger
            AddLoggerArgument(buildInfo, arguments);

            // Binary log
            AddBinaryLogArgument(buildInfo, arguments, editorSettings);

            // Custom properties
            foreach (var customProperty in buildInfo.CustomProperties)
            {
                arguments.Add("-p:" + (string)customProperty);
            }
        }

        private static void BuildPublishArguments(BuildInfo buildInfo, Collection<string> arguments,
            EditorSettings editorSettings)
        {
            arguments.Add("publish"); // `dotnet publish` command

            // C# Project
            string projectPath = buildInfo.Project;
            if (OperatingSystem.IsWindows())
            {
                // On Windows, when using a network share path, the path may start with "//"
                // which MSBuild will treat like a switch. Convert to "\\" to make it an absolute path.
                if (projectPath.StartsWith("//"))
                {
                    projectPath = "\\\\" + projectPath.Substring(2);
                }
            }
            arguments.Add(projectPath);

            // Restore
            // `dotnet publish` restores by default, unless requested not to
            if (!buildInfo.Restore)
                arguments.Add("--no-restore");

            // Incremental or rebuild
            // TODO: Not supported in `dotnet publish` (https://github.com/dotnet/sdk/issues/11099)
            // if (buildInfo.Rebuild)
            //     arguments.Add("--no-incremental");

            // Configuration
            arguments.Add("-c");
            arguments.Add(buildInfo.Configuration);

            // Runtime Identifier
            arguments.Add("-r");
            arguments.Add(buildInfo.RuntimeIdentifier!);

            // Self-published
            arguments.Add("--self-contained");
            arguments.Add("true");

            // Verbosity
            AddVerbosityArguments(buildInfo, arguments, editorSettings);

            // Logger
            AddLoggerArgument(buildInfo, arguments);

            // Binary log
            AddBinaryLogArgument(buildInfo, arguments, editorSettings);

            // Custom properties
            foreach (var customProperty in buildInfo.CustomProperties)
            {
                arguments.Add("-p:" + (string)customProperty);
            }

            // Publish output directory
            if (buildInfo.PublishOutputDir != null)
            {
                arguments.Add("-o");
                arguments.Add(buildInfo.PublishOutputDir);
            }
        }

        private static void AddVerbosityArguments(BuildInfo buildInfo, Collection<string> arguments,
            EditorSettings editorSettings)
        {
            var verbosityLevel =
                editorSettings.GetSetting(GodotSharpEditor.Settings.VerbosityLevel).As<VerbosityLevelId>();
            arguments.Add("-v");
            arguments.Add(verbosityLevel switch
            {
                VerbosityLevelId.Quiet => "quiet",
                VerbosityLevelId.Minimal => "minimal",
                VerbosityLevelId.Detailed => "detailed",
                VerbosityLevelId.Diagnostic => "diagnostic",
                _ => "normal",
            });

            if ((bool)editorSettings.GetSetting(GodotSharpEditor.Settings.NoConsoleLogging))
                arguments.Add("-noconlog");
        }

        private static void AddLoggerArgument(BuildInfo buildInfo, Collection<string> arguments)
        {
            string buildLoggerPath = Path.Combine(Internals.GodotSharpDirs.DataEditorToolsDir,
                "GodotTools.BuildLogger.dll");

            arguments.Add(
                $"-l:{typeof(GodotBuildLogger).FullName},{buildLoggerPath};{buildInfo.LogsDirPath}");
        }

        private static void AddBinaryLogArgument(BuildInfo buildInfo, Collection<string> arguments,
            EditorSettings editorSettings)
        {
            if (!(bool)editorSettings.GetSetting(GodotSharpEditor.Settings.CreateBinaryLog))
                return;

            arguments.Add($"-bl:{Path.Combine(buildInfo.LogsDirPath, "msbuild.binlog")}");
            arguments.Add("-ds:False"); // Honestly never understood why -bl also switches -ds on.
        }

        private static void RemovePlatformVariable(StringDictionary environmentVariables)
        {
            // EnvironmentVariables is case sensitive? Seriously?

            var platformEnvironmentVariables = new List<string>();

            foreach (string env in environmentVariables.Keys)
            {
                if (env.ToUpperInvariant() == "PLATFORM")
                    platformEnvironmentVariables.Add(env);
            }

            foreach (string env in platformEnvironmentVariables)
                environmentVariables.Remove(env);
        }

        private static Process DoGenerateXCFramework(List<string> outputPaths, string xcFrameworkPath,
            Action<string?>? stdOutHandler, Action<string?>? stdErrHandler)
        {
            if (Directory.Exists(xcFrameworkPath))
            {
                Directory.Delete(xcFrameworkPath, true);
            }

            var startInfo = new ProcessStartInfo("xcrun");

            BuildXCFrameworkArguments(outputPaths, xcFrameworkPath, startInfo.ArgumentList);

            string launchMessage = startInfo.GetCommandLineDisplay(new StringBuilder("Packaging: ")).ToString();
            stdOutHandler?.Invoke(launchMessage);
            if (Godot.OS.IsStdOutVerbose())
                Console.WriteLine(launchMessage);

            startInfo.RedirectStandardOutput = true;
            startInfo.RedirectStandardError = true;
            startInfo.UseShellExecute = false;

            if (OperatingSystem.IsWindows())
            {
                startInfo.StandardOutputEncoding = Encoding.UTF8;
                startInfo.StandardErrorEncoding = Encoding.UTF8;
            }

            // Needed when running from Developer Command Prompt for VS.
            RemovePlatformVariable(startInfo.EnvironmentVariables);

            var process = new Process { StartInfo = startInfo };

            if (stdOutHandler != null)
                process.OutputDataReceived += (_, e) => stdOutHandler.Invoke(e.Data);
            if (stdErrHandler != null)
                process.ErrorDataReceived += (_, e) => stdErrHandler.Invoke(e.Data);

            process.Start();

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            return process;
        }

        public static int GenerateXCFramework(List<string> outputPaths, string xcFrameworkPath, Action<string?>? stdOutHandler, Action<string?>? stdErrHandler)
        {
            using (var process = DoGenerateXCFramework(outputPaths, xcFrameworkPath, stdOutHandler, stdErrHandler))
            {
                process.WaitForExit();

                return process.ExitCode;
            }
        }

        private static void BuildXCFrameworkArguments(List<string> outputPaths,
            string xcFrameworkPath, Collection<string> arguments)
        {
            var baseDylib = $"{GodotSharpDirs.ProjectAssemblyName}.dylib";
            var baseSym = $"{GodotSharpDirs.ProjectAssemblyName}.framework.dSYM";

            arguments.Add("xcodebuild");
            arguments.Add("-create-xcframework");

            foreach (var outputPath in outputPaths)
            {
                arguments.Add("-library");
                arguments.Add(Path.Combine(outputPath, baseDylib));
                arguments.Add("-debug-symbols");
                arguments.Add(Path.Combine(outputPath, baseSym));
            }

            arguments.Add("-output");
            arguments.Add(xcFrameworkPath);
        }
    }
}
