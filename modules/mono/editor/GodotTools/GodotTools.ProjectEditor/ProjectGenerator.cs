using System;
using System.Globalization;
using System.IO;
using System.Text;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using GodotTools.Shared;

namespace GodotTools.ProjectEditor
{
    public static class ProjectGenerator
    {
        public static string GodotSdkAttrValue => $"Godot.NET.Sdk/{GeneratedGodotNupkgsVersions.GodotNETSdk}";

        public static string GodotMinimumRequiredTfm => OperatingSystem.IsOSPlatform("OPENHARMONY") ? "net10.0" : "net8.0";

        private const string LegacyOpenHarmonyNuGetConfig =
            "<configuration><packageSources><clear/>" +
            "<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>" +
            "</packageSources></configuration>\n";

        private const string OpenHarmonyNuGetConfig =
            "<configuration><packageSources><clear/>" +
            "<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>" +
            "<add key=\"nuget.org\" value=\"https://api.nuget.org/v3/index.json\" protocolVersion=\"3\"/>" +
            "</packageSources></configuration>\n";

        private static void EnsureOpenHarmonyNuGetConfig(string dir)
        {
            string path = Path.Combine(dir, "NuGet.Config");
            var info = new FileInfo(path);
            if (info.LinkTarget != null || Directory.Exists(path))
                return;

            // File.ReadAllText strips a BOM. Decode the bytes directly so only
            // the exact previous auto-generated file is migrated.
            if (info.Exists && Encoding.UTF8.GetString(File.ReadAllBytes(path)) != LegacyOpenHarmonyNuGetConfig)
                return;

            File.WriteAllText(path, OpenHarmonyNuGetConfig, new UTF8Encoding(false));
        }

        public static ProjectRootElement GenGameProject(string name)
        {
            if (name.Length == 0)
                throw new ArgumentException("Project name is empty.", nameof(name));

            var root = ProjectRootElement.Create(NewProjectFileOptions.None);

            root.Sdk = GodotSdkAttrValue;

            var mainGroup = root.AddPropertyGroup();
            mainGroup.AddProperty("TargetFramework", GodotMinimumRequiredTfm);

            // Non-gradle builds require .NET 9 to match the jar libraries included in the export template.
            var net9 = mainGroup.AddProperty("TargetFramework", "net9.0");
            net9.Condition = " '$(GodotTargetPlatform)' == 'android' ";

            var net10 = mainGroup.AddProperty("TargetFramework", "net10.0");
            net10.Condition = " '$(GodotTargetPlatform)' == 'openharmony' ";

            mainGroup.AddProperty("EnableDynamicLoading", "true");

            var openHarmonyExport = root.AddPropertyGroup();
            openHarmonyExport.Condition = " '$(GodotTargetPlatform)' == 'openharmony' and ('$(Configuration)' == 'ExportDebug' or '$(Configuration)' == 'ExportRelease') ";
            openHarmonyExport.AddProperty("PublishAot", "true");
            openHarmonyExport.AddProperty("NativeLib", "Shared");

            string sanitizedName = IdentifierUtils.SanitizeQualifiedIdentifier(name, allowEmptyIdentifiers: true);

            // If the name is not a valid namespace, manually set RootNamespace to a sanitized one.
            if (sanitizedName != name)
                mainGroup.AddProperty("RootNamespace", sanitizedName);

            return root;
        }

        public static string GenAndSaveGameProject(string dir, string name)
        {
            if (name.Length == 0)
                throw new ArgumentException("Project name is empty.", nameof(name));

            string path = Path.Combine(dir, name + ".csproj");

            var root = GenGameProject(name);

            // Save (without BOM)
            root.Save(path, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

            // Keep only Godot's matching packages local; normal dependencies
            // restore from NuGet.org using the external SDK. Preserve custom config.
            if (OperatingSystem.IsOSPlatform("OPENHARMONY") &&
                !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("GODOT_NUGET_SOURCE")))
            {
                EnsureOpenHarmonyNuGetConfig(dir);
            }

            return Guid.NewGuid().ToString().ToUpperInvariant();
        }
    }
}
