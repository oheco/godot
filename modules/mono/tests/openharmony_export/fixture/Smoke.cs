// Godot Engine contributors. SPDX-License-Identifier: MIT
using Godot;
using System;
using System.Globalization;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Threading.Tasks;

public partial class Smoke : Node3D
{
    [Signal] public delegate void CheckedEventHandler(int value);
    [Export] public int ExportedValue { get; set; } = 23;
    [Export] public Godot.Collections.Array<int> ExportedNumbers { get; set; } = new() { 3, 7 };
    private readonly bool _constructorRan;
    private MeshInstance3D? _cube;
    private int _processed;
    private int _rpcValue;

    public Smoke() { _constructorRan = true; }

    public int AddValues(int left, int right) => left + right;

    [Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)]
    public void RpcProbe(int value) { _rpcValue = value; }

    public override async void _Ready()
    {
        bool verify = OS.GetCmdlineUserArgs().Contains("--verify");
        try
        {
#if !GODOT_OPENHARMONY
            throw new Exception("Godot.NET.Sdk did not select OpenHarmony");
#endif
            if (OS.GetName() != "OpenHarmony" || !OperatingSystem.IsOSPlatform("OPENHARMONY"))
                throw new Exception("Unexpected native/managed platform");
            if (RuntimeFeature.IsDynamicCodeSupported || RuntimeFeature.IsDynamicCodeCompiled)
                throw new Exception("The game is using a JIT runtime instead of NativeAOT");
            if (!_constructorRan || ExportedValue != 47 || ExportedNumbers.Count != 2)
                throw new Exception("Script constructor or exported scene properties were removed");
            if (!GetPropertyList().Any(item => item["name"].AsString() == nameof(ExportedValue)))
                throw new Exception("Export property metadata is missing");
            if (Get(PropertyName.ExportedValue).AsInt32() != 47)
                throw new Exception("Generated property bridge failed");
            Set(PropertyName.ExportedValue, 53);
            if (ExportedValue != 53) throw new Exception("Generated property setter failed");

            int signal = 0;
            Checked += value => signal = value;
            EmitSignal(SignalName.Checked, 42);
            if (signal != 42 || new Vector3(3, 0, 4).Length() != 5)
                throw new Exception("Godot binding or signal mismatch");
            int callableValue = 0;
            Callable.From<int>(value => callableValue = value).Call(61);
            if (callableValue != 61 || Call(MethodName.AddValues, 7, 8).AsInt32() != 15)
                throw new Exception("Callable or generated script method bridge failed");
            var numbers = new Godot.Collections.Array<int> { 2, 4, 8 };
            var dictionary = new Godot.Collections.Dictionary<string, int> { ["answer"] = 42 };
            if (numbers.Sum() != 14 || dictionary["answer"] != 42 || Variant.From(numbers).AsGodotArray<int>()[1] != 4)
                throw new Exception("Typed collections or generic Variant conversion failed");

            var script = GetScript().As<Script>();
            if (!script.GetRpcConfig().AsGodotDictionary().ContainsKey(MethodName.RpcProbe))
                throw new Exception("RPC metadata was removed");
            Multiplayer.MultiplayerPeer = new OfflineMultiplayerPeer();
            if (Rpc(MethodName.RpcProbe, 73) != Error.Ok || _rpcValue != 73)
                throw new Exception("RPC metadata/local dispatch failed");
            if (string.IsNullOrEmpty(CultureInfo.GetCultureInfo("zh-CN").DisplayName) ||
                SHA256.HashData("native-aot"u8).Length != 32)
                throw new Exception("Bundled ICU/OpenSSL dependency failed");
            using (var output = FileAccess.Open("user://CSharp-验证.txt", FileAccess.ModeFlags.Write))
            {
                if (output is null) throw new Exception("Cannot open user:// for writing");
                output.StoreString("OpenHarmony C# 你好");
            }
            if (FileAccess.GetFileAsString("user://CSharp-验证.txt") != "OpenHarmony C# 你好")
                throw new Exception("Godot file round trip failed");
            _cube = GetNode<MeshInstance3D>("Cube");
            int worker = await Task.Run(() =>
            {
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                return Enumerable.Range(1, 10).Sum();
            });
            if (worker != 55) throw new Exception("NativeAOT worker/GC failed");
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
            await ToSignal(GetTree(), SceneTree.SignalName.ProcessFrame);
            if (_processed == 0) throw new Exception("Generated _Process bridge was removed");
            GetNode<Label>("Result").Text = "NativeAOT PASS: constructor, exports, signals, callables, collections, RPC, worker, GC";
            GD.Print("GODOT_NATIVEAOT_RUNTIME_CONFIRMED");
            GD.Print("OHOS_CSHARP_SMOKE_PASS");
            if (verify) GetTree().Quit();
        }
        catch (Exception error)
        {
            GD.PushError(error.ToString());
            GetNode<Label>("Result").Text = "NativeAOT FAIL: " + error.Message;
            if (verify) GetTree().Quit(1);
        }
    }

    public override void _Process(double delta)
    {
        _processed++;
        _cube?.RotateY((float)delta * 0.6f);
    }
}
