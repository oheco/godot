// Godot Engine contributors. SPDX-License-Identifier: MIT
using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using GodotTools.Export;

internal static class HelperTests
{
    private static int _checks;
    private static void Check(bool value, string name)
    {
        if (!value) throw new Exception(name);
        Console.WriteLine("PASS: " + name);
        _checks++;
    }

    private static void Throws<T>(Action action, string name) where T : Exception
    {
        try { action(); } catch (T) { Check(true, name); return; }
        throw new Exception("Expected " + typeof(T).Name + ": " + name);
    }

    private static byte[] Elf(bool signed, bool entryPoint = true, bool debugOnly = false)
    {
        byte[] data = new byte[1024];
        Encoding.ASCII.GetBytes("\x7f" + "ELF").CopyTo(data, 0);
        data[4] = 2;
        data[5] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(16), 3);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(18), 183);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(20), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(32), 64);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(40), 256);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(52), 64);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(54), 56);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(56), 2);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(58), 64);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(60), 6);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(62), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(64), 1); // PT_LOAD, executable
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(68), 5);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(64 + 32), debugOnly ? 0UL : (ulong)data.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(120), 2); // PT_DYNAMIC
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(120 + 8), 208);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(120 + 32), debugOnly ? 0UL : 32);
        byte[] names = Encoding.ASCII.GetBytes("\0.shstrtab\0.codesign\0.text\0.dynstr\0.dynsym\0");
        names.CopyTo(data, 640);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(320), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(320 + 4), 3);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(320 + 24), 640);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(320 + 32), (ulong)names.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(384), 21);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(384 + 4), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(384 + 8), 6);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(384 + 24), 192);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(384 + 32), 8);
        byte[] strings = Encoding.ASCII.GetBytes("\0" + (entryPoint ? "godotsharp_game_main_init" : "different_function") + "\0");
        strings.CopyTo(data, 700);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(448 + 4), 3);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(448 + 24), 700);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(448 + 32), (ulong)strings.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(512 + 4), 11);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(512 + 24), 800);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(512 + 32), 48);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(512 + 40), 3);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(512 + 56), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(824), 1);
        data[828] = 0x12; // STB_GLOBAL, STT_FUNC
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(830), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(576), 11);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(576 + 24), 900);
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan(576 + 32), signed ? 32UL : 0);
        return data;
    }

    private static readonly string[] SigningVariables =
    {
        "PATH", "OHECO_ROOT", "GODOT_OHOS_SDK_ROOT", "OHOS_SDK_HOME", "OHOS_BASE_SDK_HOME", "OpenHarmonySigningTool", "CppCompilerAndLinker",
    };

    private static void WithIsolatedSigningEnvironment(Action action)
    {
        var original = SigningVariables.ToDictionary(name => name, Environment.GetEnvironmentVariable);
        try
        {
            foreach (string name in SigningVariables)
                Environment.SetEnvironmentVariable(name, null);
            action();
        }
        finally
        {
            foreach (var entry in original)
                Environment.SetEnvironmentVariable(entry.Key, entry.Value);
        }
    }

    private static string FakeSignTool(params string[] directories)
    {
        string directory = Path.Combine(directories);
        Directory.CreateDirectory(directory);
        string tool = Path.GetFullPath(Path.Combine(directory, "binary-sign-tool"));
        // Deliberately not executable: discovery needs File.Exists, not execution,
        // permission changes or a real SDK/signing-material dependency.
        File.WriteAllText(tool, "resolve-only fake signer; never execute this file");
        return tool;
    }

    private static void CheckSigningResolution(string fixture)
    {
        WithIsolatedSigningEnvironment(() =>
        {
            string sdk = Path.GetFullPath(Path.Combine(fixture, "SDK view with spaces"));
            FakeSignTool(sdk, "9.0.0", "toolchains", "lib");
            string selected = FakeSignTool(sdk, "26.0.0", "toolchains", "lib");
            FakeSignTool(sdk, "not-a-version", "toolchains", "lib");
            Directory.CreateDirectory(Path.Combine(sdk, "99.0.0", "toolchains", "lib"));
            string oo = Path.GetFullPath(Path.Combine(fixture, "oo root with spaces"));
            string ooSigner = FakeSignTool(oo, "bin");
            string pathSigner = FakeSignTool(fixture, "PATH tools with spaces");
            string envSdk = Path.GetFullPath(Path.Combine(fixture, "environment SDK view"));
            string envSigner = FakeSignTool(envSdk, "25.0.0", "toolchains", "lib");
            Environment.SetEnvironmentVariable("OHECO_ROOT", oo);
            Environment.SetEnvironmentVariable("PATH", Path.GetDirectoryName(pathSigner));
            Environment.SetEnvironmentVariable("GODOT_OHOS_SDK_ROOT", envSdk);
            Check(OpenHarmonyExport.FindSignTool(sdk) == selected,
                "configured SDK with spaces wins over environment SDK, oo and PATH; highest usable numeric version");
            Check(Path.IsPathFullyQualified(OpenHarmonyExport.FindSignTool(sdk)!), "SDK signer is absolute");

            string missingSdk = Path.Combine(fixture, "missing SDK view");
            string emptySdk = Path.Combine(fixture, "empty SDK view");
            Directory.CreateDirectory(emptySdk);
            Throws<DirectoryNotFoundException>(() => OpenHarmonyExport.FindSignTool(missingSdk),
                "explicit missing SDK root does not fall through to working environment, oo or PATH");
            Throws<DirectoryNotFoundException>(() => OpenHarmonyExport.FindSignTool("relative SDK view"),
                "explicit relative SDK root is rejected");
            Throws<FileNotFoundException>(() => OpenHarmonyExport.FindSignTool(emptySdk),
                "explicit SDK root without signer does not fall through");
            Throws<FileNotFoundException>(() => OpenHarmonyExport.FindSignTool(Path.Combine(sdk, "26.0.0")),
                "inner API directory is not an SDK view root");

            Check(OpenHarmonyExport.FindSignTool() == envSigner, "environment SDK wins over oo and PATH");
            foreach (string variable in new[] { "GODOT_OHOS_SDK_ROOT", "OHOS_SDK_HOME", "OHOS_BASE_SDK_HOME" })
            {
                Environment.SetEnvironmentVariable(variable, envSdk);
                Check(OpenHarmonyExport.FindSignTool() == envSigner, "resolve SDK variable: " + variable);
                Environment.SetEnvironmentVariable(variable, emptySdk);
                Throws<FileNotFoundException>(() => OpenHarmonyExport.FindSignTool(),
                    "bad SDK variable does not silently fall through: " + variable);
                Environment.SetEnvironmentVariable(variable, null);
            }
            Environment.SetEnvironmentVariable("PATH", "/system/bin");
            Check(OpenHarmonyExport.FindSignTool() == ooSigner, "system-only PATH still resolves OHECO_ROOT/bin signer");
            Environment.SetEnvironmentVariable("PATH", Path.GetDirectoryName(pathSigner));
            Check(OpenHarmonyExport.FindSignTool() == ooSigner, "oo signer wins over PATH signer");
            Environment.SetEnvironmentVariable("OHECO_ROOT", emptySdk);
            Check(OpenHarmonyExport.FindSignTool() == pathSigner, "PATH fallback returns absolute signer");
            Environment.SetEnvironmentVariable("PATH", "/system/bin");
            Check(OpenHarmonyExport.FindSignTool() == null, "no configured SDK, oo signer or PATH signer returns null");

            Environment.SetEnvironmentVariable("OpenHarmonySigningTool", "parent-signing-property-sentinel");
            var startInfo = new ProcessStartInfo("resolve-only-child") { UseShellExecute = false };
            OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, selected);
            Check(startInfo.Environment["OpenHarmonySigningTool"] == selected &&
                Path.IsPathFullyQualified(startInfo.Environment["OpenHarmonySigningTool"]!),
                "child signing property is the selected absolute SDK signer, without shell quoting");
            Check(startInfo.Environment["PATH"] == Path.GetDirectoryName(selected) + Path.PathSeparator + "/system/bin",
                "signer directory with spaces prepends only the child PATH");
            Check(Environment.GetEnvironmentVariable("PATH") == "/system/bin" &&
                Environment.GetEnvironmentVariable("OpenHarmonySigningTool") == "parent-signing-property-sentinel",
                "ConfigureSigningEnvironment leaves parent PATH and signing property unchanged");
            var independentChild = new ProcessStartInfo("not-executed");
            Check(independentChild.Environment["PATH"] == "/system/bin" &&
                independentChild.Environment["OpenHarmonySigningTool"] == "parent-signing-property-sentinel",
                "another child still inherits unchanged parent environment");
            startInfo.Environment.Remove("PATH");
            OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, selected);
            Check(startInfo.Environment["PATH"] == Path.GetDirectoryName(selected) + Path.PathSeparator,
                "configure signing when child PATH is absent");
            Throws<FileNotFoundException>(() => OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, "binary-sign-tool"),
                "reject relative child signing property");
            Throws<FileNotFoundException>(() => OpenHarmonyExport.ConfigureSigningEnvironment(startInfo, Path.Combine(emptySdk, "binary-sign-tool")),
                "reject missing absolute child signing property");
        });
    }

    private static string FakeNativeTools(string directory)
    {
        Directory.CreateDirectory(directory);
        foreach (string tool in new[] { "clang", "ld.lld", "llvm-objcopy" })
            File.WriteAllText(Path.Combine(directory, tool), "resolve-only native tool; never execute");
        return Path.GetFullPath(Path.Combine(directory, "clang"));
    }

    private static void CheckNativeCompilerResolution(string fixture)
    {
        WithIsolatedSigningEnvironment(() =>
        {
            string sdk = Path.GetFullPath(Path.Combine(fixture, "native SDK view with spaces"));
            FakeNativeTools(Path.Combine(sdk, "9.0.0", "native", "llvm", "bin"));
            string selected = FakeNativeTools(Path.Combine(sdk, "26.0.0", "native", "llvm", "bin"));
            string oo = Path.GetFullPath(Path.Combine(fixture, "native oo root"));
            string packaged = FakeNativeTools(Path.Combine(oo, "packages", "ohos-sdk-native", "26.0.0.35-Beta", "llvm", "bin"));
            Environment.SetEnvironmentVariable("PATH", "/system/bin");
            Environment.SetEnvironmentVariable("OHECO_ROOT", oo);
            Check(OpenHarmonyExport.FindNativeCompiler(sdk) == selected, "configured SDK native LLVM wins over installed package");
            Check(OpenHarmonyExport.FindNativeCompiler() == packaged, "native package Beta version works with system-only PATH");
            Environment.SetEnvironmentVariable("GODOT_OHOS_SDK_ROOT", sdk);
            Check(OpenHarmonyExport.FindNativeCompiler() == selected, "environment SDK native LLVM wins over oo package");
            string empty = Path.Combine(fixture, "empty native SDK view");
            Directory.CreateDirectory(empty);
            Throws<FileNotFoundException>(() => OpenHarmonyExport.FindNativeCompiler(empty), "invalid explicit native SDK does not fall through");
            Throws<DirectoryNotFoundException>(() => OpenHarmonyExport.FindNativeCompiler("relative SDK"), "relative native SDK rejected");
            Environment.SetEnvironmentVariable("CppCompilerAndLinker", "parent-compiler-sentinel");
            var child = new ProcessStartInfo("not-executed");
            OpenHarmonyExport.ConfigureNativeAotEnvironment(child, selected);
            Check(child.Environment["CppCompilerAndLinker"] == selected &&
                child.Environment["PATH"] == Path.GetDirectoryName(selected) + Path.PathSeparator + "/system/bin", "native publish child gets absolute clang and LLVM tools on PATH");
            Check(Environment.GetEnvironmentVariable("PATH") == "/system/bin" &&
                Environment.GetEnvironmentVariable("CppCompilerAndLinker") == "parent-compiler-sentinel", "native tool configuration does not modify parent environment");
            string incomplete = Path.Combine(fixture, "incomplete LLVM");
            Directory.CreateDirectory(incomplete);
            string compiler = Path.Combine(incomplete, "clang");
            File.WriteAllText(compiler, "not executed");
            Throws<FileNotFoundException>(() => OpenHarmonyExport.ConfigureNativeAotEnvironment(child, compiler), "missing linker or objcopy fails before publish");
        });
    }

    public static void Main(string[] args)
    {
        Check(!OpenHarmonyExport.HasCodeSignature(Encoding.UTF8.GetBytes("managed")), "managed resource is not ELF");
        Check(OpenHarmonyExport.HasCodeSignature(Elf(true)), "recognize code signature");
        Check(!OpenHarmonyExport.HasCodeSignature(Elf(false)), "reject empty code signature");
        byte[] wrongArch = Elf(true);
        wrongArch[18] = 62;
        Throws<NotSupportedException>(() => OpenHarmonyExport.HasCodeSignature(wrongArch), "reject x86-64 ELF");
        byte[] bounds = Elf(true);
        BinaryPrimitives.WriteUInt64LittleEndian(bounds.AsSpan(40), ulong.MaxValue);
        Throws<InvalidDataException>(() => OpenHarmonyExport.HasCodeSignature(bounds), "reject invalid section bounds");

        if (args.Length is < 1 or > 3) throw new Exception("Usage: HelperTests <private-fixture-directory> [native-aot-publish] [assembly-name]");
        string fixture = args[0];
        Directory.CreateDirectory(fixture);
        CheckSigningResolution(fixture);
        CheckNativeCompilerResolution(fixture);
        OpenHarmonyExport.ValidateSharedLibrary(Elf(true), true);
        Check(true, "accept a defined global function in the ELF dynamic symbol table");
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidateSharedLibrary(wrongArch, true), "AOT game rejects wrong architecture");
        byte[] executable = Elf(true);
        BinaryPrimitives.WriteUInt16LittleEndian(executable.AsSpan(16), 2);
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidateSharedLibrary(executable, true), "AOT game rejects executable instead of ET_DYN");
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidateSharedLibrary(Elf(true, false), true), "missing initialization export fails");
        foreach (int variant in new[] { 0, 1, 2 })
        {
            byte[] badExport = Elf(true);
            if (variant == 0) BinaryPrimitives.WriteUInt16LittleEndian(badExport.AsSpan(830), 0);
            if (variant == 1) badExport[828] = 2; // local function
            if (variant == 2) badExport[829] = 2; // hidden function
            Throws<NotSupportedException>(() => OpenHarmonyExport.ValidateSharedLibrary(badExport, true), "undefined/local/hidden export rejected: " + variant);
        }
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidateSharedLibrary(Elf(false, true, true), true), "detached ELF debug file is not a library");
        byte[] truncatedSymbols = Elf(true);
        BinaryPrimitives.WriteUInt64LittleEndian(truncatedSymbols.AsSpan(512 + 24), ulong.MaxValue);
        Throws<InvalidDataException>(() => OpenHarmonyExport.ValidateSharedLibrary(truncatedSymbols, true), "reject invalid dynamic symbol bounds");
        using (var metadata = System.Text.Json.JsonDocument.Parse(OpenHarmonyExport.NativeAotMetadata()))
        {
            var root = metadata.RootElement;
            Check(root.EnumerateObject().Count() == 3 && root.GetProperty("schemaVersion").GetInt32() == 1 &&
                root.GetProperty("mode").GetString() == "native-aot" &&
                root.GetProperty("library").GetString() == "libgodot-csharp-game.so", "exact small PCK initialization contract");
        }
        string publishFixture = Path.Combine(fixture, "AOT publish with spaces");
        Directory.CreateDirectory(publishFixture);
        string assembly = "Smoke with spaces";
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidatePublish(publishFixture, assembly), "missing AOT output cannot fall back");
        string main = Path.Combine(publishFixture, assembly + ".so");
        File.WriteAllBytes(main, Elf(true));
        string[] dependencies = { "libicudata.so.78", "libicuuc.so.78", "libicui18n.so.78", "libcrypto.so.3", "libssl.so.3", "libc++_shared.so" };
        foreach (string dependency in dependencies)
            File.WriteAllBytes(Path.Combine(publishFixture, dependency), Elf(true, false));
        File.WriteAllBytes(Path.Combine(publishFixture, assembly + ".so.dbg"), Elf(false, true, true));
        Check(OpenHarmonyExport.ValidatePublish(publishFixture, assembly).Length == 7, "main plus six native dependencies; detached symbols excluded");
        foreach (string forbidden in new[] { assembly + ".dll", "GodotSharpEditor.dll", "libcoreclr.so", assembly + ".runtimeconfig.json" })
        {
            string path = Path.Combine(publishFixture, forbidden);
            File.WriteAllText(path, "forbidden output");
            Throws<NotSupportedException>(() => OpenHarmonyExport.ValidatePublish(publishFixture, assembly), "reject managed/CoreCLR publish output: " + forbidden);
            File.Delete(path);
        }
        string missing = Path.Combine(publishFixture, "libssl.so.3");
        File.Delete(missing);
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidatePublish(publishFixture, assembly), "missing delayed OpenSSL dependency fails");
        File.WriteAllBytes(missing, Elf(true, false));
        string nested = Path.Combine(publishFixture, "nested");
        Directory.CreateDirectory(nested);
        string collision = Path.Combine(nested, "libssl.so.3");
        File.WriteAllBytes(collision, Elf(true, false));
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidatePublish(publishFixture, assembly), "flattened native basename collision fails");
        File.Delete(collision);
        string reserved = Path.Combine(publishFixture, OpenHarmonyExport.NativeAotLibraryName);
        File.WriteAllBytes(reserved, Elf(true));
        Throws<NotSupportedException>(() => OpenHarmonyExport.ValidatePublish(publishFixture, assembly), "reserved game library basename collision fails");
        File.Delete(reserved);
        string stage = Path.Combine(fixture, "native deployment stage");
        string[] staged = OpenHarmonyExport.StageNativeLibraries(publishFixture, assembly, stage, null);
        Check(staged.Length == 7 && Path.GetFileName(staged[0]) == OpenHarmonyExport.NativeAotLibraryName, "native staging renames only the game library");
        Check(dependencies.All(name => File.Exists(Path.Combine(stage, name))), "versioned ICU/OpenSSL dependency names are preserved");
        Check(staged.All(path => !path.EndsWith(".dbg")) && !File.Exists(Path.Combine(stage, assembly + ".so")), "native payload contains no old game basename or debug files");
        Check(File.ReadAllBytes(main).SequenceEqual(Elf(true)), "native staging leaves source input unchanged");
        string debug = Path.Combine(publishFixture, assembly + ".so.dbg");
        Check(OpenHarmonyExport.ReadPackagedFile(debug, "never-execute") .SequenceEqual(File.ReadAllBytes(debug)), "debug ELF is never signed");
        string symbols = OpenHarmonyExport.ExportDebugSymbols(publishFixture, assembly, Path.Combine(fixture, "export target.hap"));
        Check(symbols == Path.GetFullPath(Path.Combine(fixture, "export target.symbols", "arm64")), "requested symbols go beside the export target");
        Check(File.ReadAllBytes(Path.Combine(symbols, assembly + ".so.dbg")).SequenceEqual(File.ReadAllBytes(debug)) &&
            File.ReadAllBytes(Path.Combine(symbols, "libgodot-csharp-game.so.dbg")).SequenceEqual(File.ReadAllBytes(debug)), "preserve original debuglink basename and reserved symbol alias");
        Check(File.ReadAllBytes(main).SequenceEqual(Elf(true)), "debug symbol export leaves signed native ELF unchanged");
        using (var mapping = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(Path.Combine(symbols, "native-aot-symbols.json"))))
            Check(mapping.RootElement.GetProperty("debugLink").GetString() == assembly + ".so.dbg", "symbol mapping records original debuglink");
        string unsigned = Path.Combine(fixture, "unsigned.so");
        File.WriteAllBytes(unsigned, Elf(false));
        WithIsolatedSigningEnvironment(() =>
        {
            Environment.SetEnvironmentVariable("PATH", fixture);
            Environment.SetEnvironmentVariable("OHECO_ROOT", fixture);
            Throws<FileNotFoundException>(() => OpenHarmonyExport.ReadPackagedFile(unsigned), "unsigned native deployment requires signer");
        });
        int nativeFiles = 0;
        if (args.Length >= 2)
        {
            string actualAssembly = args.Length == 3 ? args[2] : "Smoke";
            string[] real = OpenHarmonyExport.ValidatePublish(args[1], actualAssembly);
            nativeFiles = real.Length;
            Check(nativeFiles >= 7, "real NativeAOT game shared library and bundled dependencies validate");
            Check(!Directory.GetFiles(args[1], "*.dll", SearchOption.AllDirectories).Any(), "real publish carries no managed/CoreCLR fallback");
        }
        Console.WriteLine($"OHOS_EXPORT_HELPER_PASS checks={_checks} nativeFiles={nativeFiles}");
    }
}
