// Godot Engine contributors. SPDX-License-Identifier: MIT
using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace GodotTools.Export
{
    // NativeAOT games deploy a shared library and its native dependencies.
    // The PCK contains only the small initialization record.
    internal static class OpenHarmonyExport
    {
        internal const string NativeAotLibraryName = "libgodot-csharp-game.so";
        internal const string NativeAotMetadataPath = "res://.godot/mono/openharmony_aot.json";
        internal static byte[] NativeAotMetadata() => Encoding.UTF8.GetBytes(
            "{\"schemaVersion\":1,\"mode\":\"native-aot\",\"library\":\"libgodot-csharp-game.so\"}");

        internal static string[] ValidatePublish(string directory, string assemblyName) => NativeLibraries(directory, assemblyName).ToArray();

        private static List<string> NativeLibraries(string directory, string assemblyName)
        {
            string main = Path.Combine(directory, assemblyName + ".so");
            if (!File.Exists(main))
                throw new NotSupportedException($"OpenHarmony NativeAOT publish is missing '{assemblyName}.so'. PublishAot=true and NativeLib=Shared are required; CoreCLR/JIT fallback is not supported.");
            ValidateSharedLibrary(File.ReadAllBytes(main), requireGameEntryPoint: true);
            var libraries = new List<string> { main };
            var destinations = new HashSet<string>(StringComparer.Ordinal) { NativeAotLibraryName };
            foreach (string file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
            {
                string name = Path.GetFileName(file);
                if (name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ||
                    name.EndsWith(".deps.json", StringComparison.OrdinalIgnoreCase) ||
                    name.EndsWith(".runtimeconfig.json", StringComparison.OrdinalIgnoreCase) ||
                    name is "libhostfxr.so" or "libhostpolicy.so" or "libcoreclr.so" or "libclrjit.so" or "libclrgc.so" or "libclrgcexp.so")
                    throw new NotSupportedException($"OpenHarmony NativeAOT publish contains managed/CoreCLR output '{name}'. Do not fall back to a JIT runtime.");
                if (file == main || name.EndsWith(".dbg", StringComparison.OrdinalIgnoreCase) ||
                    !(name.EndsWith(".so", StringComparison.Ordinal) || name.Contains(".so.", StringComparison.Ordinal)))
                    continue;
                ValidateSharedLibrary(File.ReadAllBytes(file), requireGameEntryPoint: false);
                if (!destinations.Add(name))
                    throw new NotSupportedException($"OpenHarmony native publish has colliding or reserved library basename '{name}'. HAP libraries share one directory.");
                libraries.Add(file);
            }
            foreach (string required in new[] { "libicudata.so.", "libicuuc.so.", "libicui18n.so.", "libcrypto.so.", "libssl.so.", "libc++_shared.so" })
            {
                if (!libraries.Any(path => Path.GetFileName(path).StartsWith(required, StringComparison.Ordinal)))
                    throw new NotSupportedException($"OpenHarmony NativeAOT publish is missing bundled native dependency '{required}'. Use the oo-installed .NET 10 NativeAOT SDK and keep ICU/OpenSSL/C++ libraries beside the game library.");
            }
            return libraries;
        }

        internal static string[] StageNativeLibraries(string directory, string assemblyName, string stagingDirectory, string? signingTool)
        {
            var libraries = NativeLibraries(directory, assemblyName);
            Directory.CreateDirectory(stagingDirectory);
            var staged = new List<string>();
            for (int index = 0; index < libraries.Count; index++)
            {
                string destination = Path.Combine(stagingDirectory, index == 0 ? NativeAotLibraryName : Path.GetFileName(libraries[index]));
                File.Copy(libraries[index], destination, overwrite: false);
                // Only the private export copy can be signed. SDK packs and the
                // user's project native inputs are never passed to the signer.
                ReadPackagedFile(destination, signingTool);
                staged.Add(destination);
            }
            return staged.ToArray();
        }

        internal static string ExportDebugSymbols(string directory, string assemblyName, string exportPath)
        {
            string originalName = assemblyName + ".so.dbg";
            string source = Path.Combine(directory, originalName);
            if (!File.Exists(source))
                throw new NotSupportedException("OpenHarmony native debug symbols were requested but the NativeAOT publish produced no .so.dbg file.");
            string symbols = Path.GetFullPath(Path.ChangeExtension(exportPath, null) + ".symbols/arm64");
            Directory.CreateDirectory(symbols);
            // Keep the original basename used by the signed ELF's .gnu_debuglink.
            // The reserved alias makes the renamed deployment library easy to find.
            File.Copy(source, Path.Combine(symbols, originalName), overwrite: true);
            string alias = NativeAotLibraryName + ".dbg";
            if (alias != originalName)
                File.Copy(source, Path.Combine(symbols, alias), overwrite: true);
            foreach (string file in Directory.EnumerateFiles(directory, "*.dbg", SearchOption.AllDirectories))
            {
                if (file == source)
                    continue;
                string name = Path.GetFileName(file);
                if (name == originalName || name == alias)
                    throw new NotSupportedException($"OpenHarmony native symbols have colliding basename '{name}'.");
                File.Copy(file, Path.Combine(symbols, name), overwrite: true);
            }
            File.WriteAllBytes(Path.Combine(symbols, "native-aot-symbols.json"), System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new
            {
                library = NativeAotLibraryName,
                symbols = alias,
                debugLink = originalName,
            }));
            return symbols;
        }

        internal static void ValidateSharedLibrary(ReadOnlySpan<byte> data, bool requireGameEntryPoint)
        {
            if (!IsElf(data) || data.Length < 64 || data[4] != 2 || data[5] != 1 ||
                BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(16, 2)) != 3 ||
                BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(18, 2)) != 183)
                throw new NotSupportedException("OpenHarmony NativeAOT libraries must be little-endian AArch64 ELF64 ET_DYN shared objects.");
            ulong programOffset = BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(32, 8));
            ushort programSize = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(54, 2));
            ushort programCount = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(56, 2));
            if (programSize < 56 || programCount == 0)
                throw new InvalidDataException("Native library has no valid program headers.");
            var programs = CheckedSlice(data, programOffset, (ulong)programSize * programCount);
            bool dynamic = false, executable = false;
            for (int index = 0; index < programCount; index++)
            {
                var program = programs.Slice(index * programSize, programSize);
                uint type = BinaryPrimitives.ReadUInt32LittleEndian(program);
                uint flags = BinaryPrimitives.ReadUInt32LittleEndian(program.Slice(4, 4));
                ulong size = BinaryPrimitives.ReadUInt64LittleEndian(program.Slice(32, 8));
                if (size > 0)
                    _ = CheckedSlice(data, BinaryPrimitives.ReadUInt64LittleEndian(program.Slice(8, 8)), size);
                dynamic |= type == 2 && size > 0;
                executable |= type == 1 && (flags & 1) != 0 && size > 0;
            }
            if (!dynamic || !executable)
                throw new NotSupportedException("Native library contains no loadable code/dynamic segment. Detached ELF debug files cannot be deployed as libraries.");
            if (requireGameEntryPoint && !HasExportedGameEntryPoint(data))
                throw new NotSupportedException("OpenHarmony NativeAOT game library does not export godotsharp_game_main_init. Check Godot source generators and NativeLib=Shared.");
        }

        private static bool HasExportedGameEntryPoint(ReadOnlySpan<byte> data)
        {
            ulong offset = BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(40, 8));
            ushort entrySize = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(58, 2));
            ushort count = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(60, 2));
            if (entrySize < 64 || count == 0)
                throw new InvalidDataException("NativeAOT game library has no valid section table.");
            var sections = CheckedSlice(data, offset, (ulong)entrySize * count);
            for (int index = 0; index < count; index++)
            {
                var section = sections.Slice(index * entrySize, entrySize);
                if (BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(4, 4)) != 11)
                    continue; // SHT_DYNSYM, not an unexported debug symbol table.
                uint stringsIndex = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(40, 4));
                ulong symbolSize = BinaryPrimitives.ReadUInt64LittleEndian(section.Slice(56, 8));
                if (stringsIndex >= count || symbolSize < 24 || symbolSize > int.MaxValue)
                    throw new InvalidDataException("Invalid ELF dynamic symbol table.");
                var symbols = CheckedSlice(data, BinaryPrimitives.ReadUInt64LittleEndian(section.Slice(24, 8)), BinaryPrimitives.ReadUInt64LittleEndian(section.Slice(32, 8)));
                var stringsHeader = sections.Slice((int)stringsIndex * entrySize, entrySize);
                var strings = CheckedSlice(data, BinaryPrimitives.ReadUInt64LittleEndian(stringsHeader.Slice(24, 8)), BinaryPrimitives.ReadUInt64LittleEndian(stringsHeader.Slice(32, 8)));
                if (symbols.Length % (int)symbolSize != 0)
                    throw new InvalidDataException("Truncated ELF dynamic symbols.");
                for (int symbolOffset = 0; symbolOffset < symbols.Length; symbolOffset += (int)symbolSize)
                {
                    var symbol = symbols.Slice(symbolOffset, (int)symbolSize);
                    uint nameOffset = BinaryPrimitives.ReadUInt32LittleEndian(symbol);
                    ushort definition = BinaryPrimitives.ReadUInt16LittleEndian(symbol.Slice(6, 2));
                    if (definition == 0 || (symbol[4] & 15) != 2 || (symbol[4] >> 4) is not (1 or 2) || (symbol[5] & 3) is not (0 or 3))
                        continue;
                    if (nameOffset >= strings.Length)
                        throw new InvalidDataException("ELF symbol string is outside its string table.");
                    var name = strings.Slice((int)nameOffset);
                    int terminator = name.IndexOf((byte)0);
                    if (terminator < 0)
                        throw new InvalidDataException("ELF symbol string is unterminated.");
                    if (name.Slice(0, terminator).SequenceEqual("godotsharp_game_main_init"u8))
                        return true;
                }
            }
            return false;
        }

        internal static byte[] ReadPackagedFile(string path, string? signingTool = null)
        {
            byte[] data = File.ReadAllBytes(path);
            if (!IsElf(data) || path.EndsWith(".dbg", StringComparison.OrdinalIgnoreCase) || HasCodeSignature(data))
                return data;

            // Sign only the private deployment copy, before handing it to the
            // native exporter. Never sign SDK packs or project input libraries.
            string? signer = signingTool ?? FindSignTool();
            if (signer == null)
                throw new FileNotFoundException($"OpenHarmony native publish file '{path}' is unsigned. Install ohos-sdk-toolchains with oo and put binary-sign-tool on PATH.");
            string signedPath = path + ".godot-signed-" + Guid.NewGuid().ToString("N");
            try
            {
                var startInfo = new ProcessStartInfo(signer)
                {
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                foreach (string argument in new[] { "sign", "-selfSign", "1", "-inFile", path, "-outFile", signedPath })
                    startInfo.ArgumentList.Add(argument);
                using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("Cannot start binary-sign-tool.");
                var stdout = process.StandardOutput.ReadToEndAsync();
                var stderr = process.StandardError.ReadToEndAsync();
                process.WaitForExit();
                string output = stdout.GetAwaiter().GetResult() + stderr.GetAwaiter().GetResult();
                if (process.ExitCode != 0 || !File.Exists(signedPath))
                    throw new InvalidOperationException($"binary-sign-tool failed for '{path}' (exit {process.ExitCode}): {output}");
                data = File.ReadAllBytes(signedPath);
                if (!IsElf(data) || !HasCodeSignature(data))
                    throw new InvalidOperationException($"binary-sign-tool produced no ELF code signature for '{path}'.");
                // Replace, rather than editing in place, even if publish used a
                // symlink or a hard link to an input file.
                File.Move(signedPath, path, overwrite: true);
                return data;
            }
            finally
            {
                if (File.Exists(signedPath))
                    File.Delete(signedPath);
            }
        }

        internal static string? FindNativeCompiler(string? configuredSdkRoot = null)
        {
            if (!string.IsNullOrEmpty(configuredSdkRoot))
                return NativeCompilerInSdk(configuredSdkRoot);
            foreach (string variable in new[] { "GODOT_OHOS_SDK_ROOT", "OHOS_SDK_HOME", "OHOS_BASE_SDK_HOME" })
            {
                string? sdk = Environment.GetEnvironmentVariable(variable);
                if (!string.IsNullOrEmpty(sdk))
                    return NativeCompilerInSdk(sdk);
            }
            string root = Environment.GetEnvironmentVariable("OHECO_ROOT") ?? "/storage/Users/currentUser/.oheco";
            string packages = Path.Combine(root, "packages", "ohos-sdk-native");
            string? compiler = Directory.Exists(packages) ? FindVersionedTool(packages, Path.Combine("llvm", "bin", "clang")) : null;
            if (compiler != null)
                return compiler;
            foreach (string directory in new[] { Path.Combine(root, "bin") }.Concat(
                         (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)))
            {
                if (string.IsNullOrEmpty(directory))
                    continue;
                string candidate = Path.Combine(directory, "clang");
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
            return null;
        }

        private static string NativeCompilerInSdk(string root)
        {
            if (!Path.IsPathFullyQualified(root) || !Directory.Exists(root))
                throw new DirectoryNotFoundException("OpenHarmony SDK Root must be an existing absolute SDK view root (oo sdk path), not a component directory.");
            return FindVersionedTool(root, Path.Combine("native", "llvm", "bin", "clang")) ??
                throw new FileNotFoundException("OpenHarmony SDK Root contains no native/llvm/bin/clang. Install the matching ohos-sdk-native component and select its SDK view.");
        }

        private static string? FindVersionedTool(string root, string relativePath)
        {
            string? tool = null;
            Version? selected = null;
            foreach (string directory in Directory.EnumerateDirectories(root))
            {
                if (!Version.TryParse(Path.GetFileName(directory).Split('-')[0], out var version))
                    continue;
                string candidate = Path.Combine(directory, relativePath);
                if (File.Exists(candidate) && (selected == null || version > selected))
                {
                    selected = version;
                    tool = candidate;
                }
            }
            return tool == null ? null : Path.GetFullPath(tool);
        }

        internal static void ConfigureNativeAotEnvironment(ProcessStartInfo startInfo, string compiler)
        {
            if (!Path.IsPathFullyQualified(compiler) || !File.Exists(compiler))
                throw new FileNotFoundException("OpenHarmony NativeAOT requires an existing absolute clang path.");
            string directory = Path.GetDirectoryName(compiler)!;
            if (!File.Exists(Path.Combine(directory, "llvm-objcopy")) || !File.Exists(Path.Combine(directory, "ld.lld")))
                throw new FileNotFoundException("OpenHarmony NativeAOT LLVM directory must contain clang, ld.lld and llvm-objcopy.");
            startInfo.Environment["CppCompilerAndLinker"] = compiler;
            string existing = startInfo.Environment.TryGetValue("PATH", out string? value) ? value ?? "" : "";
            startInfo.Environment["PATH"] = directory + Path.PathSeparator + existing;
        }

        internal static string? FindSignTool(string? configuredSdkRoot = null)
        {
            // SDK Root is shared with the native exporter. Resolve the native
            // ELF signer before starting MSBuild; a desktop-launched HAP does
            // not inherit the user's terminal PATH.
            if (!string.IsNullOrEmpty(configuredSdkRoot))
                return SignToolInSdk(configuredSdkRoot);
            foreach (string variable in new[] { "GODOT_OHOS_SDK_ROOT", "OHOS_SDK_HOME", "OHOS_BASE_SDK_HOME" })
            {
                string? sdk = Environment.GetEnvironmentVariable(variable);
                if (!string.IsNullOrEmpty(sdk))
                    return SignToolInSdk(sdk);
            }
            // Match the shared host's default; HOME can be application-private.
            string root = Environment.GetEnvironmentVariable("OHECO_ROOT") ?? "/storage/Users/currentUser/.oheco";
            string installed = Path.Combine(root, "bin", "binary-sign-tool");
            if (File.Exists(installed))
                return Path.GetFullPath(installed);
            foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
            {
                if (string.IsNullOrEmpty(directory))
                    continue;
                string candidate = Path.Combine(directory, "binary-sign-tool");
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
            return null;
        }

        private static string SignToolInSdk(string root)
        {
            if (!Path.IsPathFullyQualified(root) || !Directory.Exists(root))
                throw new DirectoryNotFoundException("OpenHarmony SDK Root must be an existing absolute SDK view root (oo sdk path), not a component directory.");
            string? tool = null;
            Version? selected = null;
            foreach (string directory in Directory.EnumerateDirectories(root))
            {
                if (!Version.TryParse(Path.GetFileName(directory), out var version))
                    continue;
                string candidate = Path.Combine(directory, "toolchains", "lib", "binary-sign-tool");
                if (File.Exists(candidate) && (selected == null || version > selected))
                {
                    selected = version;
                    tool = candidate;
                }
            }
            return tool != null ? Path.GetFullPath(tool) : throw new FileNotFoundException("OpenHarmony SDK Root contains no toolchains/lib/binary-sign-tool. Install the matching ohos-sdk-toolchains and create the SDK view with oo; do not select the inner API or native component directory.");
        }

        internal static void ConfigureSigningEnvironment(ProcessStartInfo startInfo, string signer)
        {
            if (!Path.IsPathFullyQualified(signer) || !File.Exists(signer))
                throw new FileNotFoundException("OpenHarmony signing requires an existing absolute binary-sign-tool path.");
            // MSBuild imports this environment property unless the project
            // explicitly overrides it. PATH is changed only for this child,
            // also supporting projects that retain the SDK's bare tool name.
            startInfo.Environment["OpenHarmonySigningTool"] = signer;
            string existing = startInfo.Environment.TryGetValue("PATH", out string? value) ? value ?? "" : "";
            startInfo.Environment["PATH"] = Path.GetDirectoryName(signer) + Path.PathSeparator + existing;
        }

        private static bool IsElf(ReadOnlySpan<byte> data) => data.Length >= 4 &&
            data[0] == 0x7f && data[1] == (byte)'E' && data[2] == (byte)'L' && data[3] == (byte)'F';

        internal static bool HasCodeSignature(ReadOnlySpan<byte> data)
        {
            if (!IsElf(data))
                return false;
            if (data.Length < 64 || data[4] != 2 || data[5] != 1 ||
                BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(18, 2)) != 183)
                throw new NotSupportedException("OpenHarmony publish contains native code that is not little-endian AArch64 ELF64.");
            ulong offset = BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(40, 8));
            ushort entrySize = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(58, 2));
            ushort count = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(60, 2));
            ushort namesIndex = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(62, 2));
            if (count == 0)
                return false;
            if (entrySize < 64 || namesIndex >= count)
                throw new InvalidDataException("Invalid ELF section table in OpenHarmony publish.");
            var sections = CheckedSlice(data, offset, (ulong)entrySize * count);
            var namesHeader = sections.Slice(namesIndex * entrySize, entrySize);
            var names = CheckedSlice(data, BinaryPrimitives.ReadUInt64LittleEndian(namesHeader.Slice(24, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(namesHeader.Slice(32, 8)));
            byte[] signatureName = Encoding.ASCII.GetBytes(".codesign\0");
            for (int index = 0; index < count; index++)
            {
                var section = sections.Slice(index * entrySize, entrySize);
                uint nameOffset = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(0, 4));
                if (nameOffset <= names.Length && names.Slice((int)nameOffset).StartsWith(signatureName))
                {
                    ulong size = BinaryPrimitives.ReadUInt64LittleEndian(section.Slice(32, 8));
                    if (size > 0)
                    {
                        _ = CheckedSlice(data, BinaryPrimitives.ReadUInt64LittleEndian(section.Slice(24, 8)), size);
                        return true;
                    }
                }
            }
            return false;
        }

        private static ReadOnlySpan<byte> CheckedSlice(ReadOnlySpan<byte> data, ulong offset, ulong size)
        {
            if (offset > (ulong)data.Length || size > (ulong)data.Length - offset)
                throw new InvalidDataException("Invalid ELF section bounds in OpenHarmony publish.");
            return data.Slice((int)offset, (int)size);
        }
    }
}
