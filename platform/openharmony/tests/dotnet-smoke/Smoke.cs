// Godot Engine contributors. SPDX-License-Identifier: MIT
using Godot;
using System;
using System.Linq;
using System.Reflection.Emit;
using System.Threading.Tasks;
using Label = Godot.Label;

public partial class Smoke : Node3D
{
    [Signal] public delegate void CheckedEventHandler(int value);
    private MeshInstance3D? _cube;

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
            int signal = 0;
            Checked += value => signal = value;
            EmitSignal(SignalName.Checked, 42);
            if (signal != 42 || new Vector3(3, 0, 4).Length() != 5)
                throw new Exception("Godot binding or signal mismatch");

            using (var output = FileAccess.Open("user://CSharp-验证.txt", FileAccess.ModeFlags.Write))
            {
                if (output is null) throw new Exception("Cannot open user:// for writing");
                output.StoreString("OpenHarmony C# 你好");
            }
            if (FileAccess.GetFileAsString("user://CSharp-验证.txt") != "OpenHarmony C# 你好")
                throw new Exception("Godot file round trip failed");

            int worker = await Task.Run(() =>
            {
                var method = new DynamicMethod("NativeJitCheck", typeof(int), Type.EmptyTypes);
                var il = method.GetILGenerator();
                il.Emit(OpCodes.Ldc_I4, 100);
                il.Emit(OpCodes.Ret);
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                return ((Func<int>)method.CreateDelegate(typeof(Func<int>)))();
            });
            if (worker != 100) throw new Exception("JIT worker result mismatch");
            _cube = GetNode<MeshInstance3D>("Cube");
            GetNode<Label>("Result").Text = "C# PASS: bindings, signals, user files, JIT, worker, GC";
            GD.Print("OHOS_CSHARP_SMOKE_PASS");
            if (verify) GetTree().Quit();
        }
        catch (Exception error)
        {
            GD.PushError(error.ToString());
            GetNode<Label>("Result").Text = "C# FAIL: " + error.Message;
            if (verify) GetTree().Quit(1);
        }
    }

    public override void _Process(double delta)
    {
        _cube?.RotateY((float)delta * 0.6f);
    }
}
