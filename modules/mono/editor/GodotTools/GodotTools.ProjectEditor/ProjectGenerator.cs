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

            // The HAP carries a pinned local feed. Keep its location relocatable
            // and leave an existing project's NuGet configuration intact.
            string nugetConfig = Path.Combine(dir, "NuGet.Config");
            if (OperatingSystem.IsOSPlatform("OPENHARMONY") &&
                !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("GODOT_NUGET_SOURCE")) &&
                !File.Exists(nugetConfig))
            {
                File.WriteAllText(nugetConfig,
                    "<configuration><packageSources><clear/>" +
                    "<add key=\"godot-bundled\" value=\"%GODOT_NUGET_SOURCE%\"/>" +
                    "</packageSources></configuration>\n", new UTF8Encoding(false));
            }

            return Guid.NewGuid().ToString().ToUpperInvariant();
        }
    }
}
