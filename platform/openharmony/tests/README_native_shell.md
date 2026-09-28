# Native shell regression fixture

Run this test **on HarmonyOS**, using a native OpenHarmony SDK compiler, native Node.js (N-API), Python 3, and `binary-sign-tool`. It does not run SCons or change the application bundle. The compiler and self-signed test addon, fixture data, logs, and temporary directories are created under `$TMPDIR` and removed on completion.

```sh
export OPENHARMONY_SDK_PATH=/absolute/path/to/ohos-sdk-native
python3 platform/openharmony/tests/test_native_shell.py

# All executable locations can be provided explicitly:
python3 platform/openharmony/tests/test_native_shell.py \
  --sdk "$OPENHARMONY_SDK_PATH" \
  --cxx /absolute/path/to/clang++ \
  --node /absolute/path/to/node \
  --sign-tool /absolute/path/to/binary-sign-tool
```

`--sdk` accepts either the SDK root (with a `native` child) or the native SDK directory itself. `CXX` and `OPENHARMONY_SDK_PATH` are the corresponding environment defaults. `$TMPDIR` must already exist and be writable; no fallback to `/tmp` is used.

## What it verifies

The test compiles the actual shared [NAPI implementation](../../../misc/dist/openharmony_template/entry/src/main/cpp/napi_init.cpp) and [runtime path helper](../../../misc/dist/openharmony_template/entry/src/main/cpp/runtime_paths.cpp) against SDK headers, signs a test Node addon, then runs isolated native Node processes. Only the engine host, ResourceManager and native-window boundary functions are mocked by [stubs.cpp](native_shell_fixture/stubs.cpp).

- Startup prerequisites in multiple deterministic permutations, including `configure`, `setup`, resources, window ID, surface creation and surface size arriving last/first.
- `none` mode with an empty runtime and no SDK/managed assets: sandbox paths and TMPDIR only, no GodotSharp/NuGet/project setup or managed environment variables.
- `sdk` mode's GodotSharp validation, example copy and NuGet configuration, including missing assets and retrying a rejected configuration.
- Unknown managed modes, malformed arrays/elements, wrong argument counts/types, NUL-containing strings, nonfinite/fractional/out-of-range inputs and mismatched surfaces.
- Empty and nonempty granted-permission arrays, forwarded unchanged to native start; both packaged and nonpackaged flags.
- Launcher registration after startup, terminal startup-error propagation, repeated setup rejection, early destroy, and idempotent destroy.
- Assertions that the engine has stopped **before** destroying the window or releasing the ResourceManager.

## Real engine argument helper

Run the companion test with the same SDK and signing-tool options (Node is not required):

```sh
python3 platform/openharmony/tests/test_engine_arguments.py \
  --sdk "$OPENHARMONY_SDK_PATH"
```

This compiles and self-signs two native executables including the **actual** [engine_arguments_openharmony.h](../engine_arguments_openharmony.h) used by the host: one with `OVERRIDE_PATH_ENABLED`, one without it (`disable_path_overrides=yes`). The fixture does not copy the argument algorithm. It covers `_cl_` line/CRLF parsing, all `--`/`++` separator combinations, explicit engine options staying ahead of user arguments, missing operands, user tokens that look like engine options, input/output aliasing, explicit rawfile and filesystem pack paths, and default pack existence checks. Hardened mode must not inject `--main-pack` and must reject explicit path overrides; the engine retains `EXEC_PATH=template` for automatic bundle pack discovery. Both modes reject a missing default rawfile pack before engine setup.

The host also checks the editor/project-manager hints after `Main::setup`: an editor binary must reject packaged launches whose invalid/corrupt project would otherwise fall back to the project manager. That guard and actual pack parsing still require real-engine/HAP validation; the helper fixture verifies argument construction, not Godot pack loading.

## What it does not verify

This fixture is **not signed-HAP validation**. It does not initialize the real Godot engine, ArkUI XComponent, VSync, renderer or .NET runtime. The tiny managed-asset files are fixtures, not working assemblies. Node supplies the compatible N-API runtime; Ark's behavior must still be verified on-device in the app.

Before release, exercise real packaged games (`_cl_`, default `template.pck`, explicit `--main-pack`, extra args and missing-pack errors), unmanaged editor/project launches, SDK-enabled C# editing/running, denied permissions, startup/quit/crash logs, foreground/background/focus/input/resize, natural exit, and surface destruction during engine startup. Verify a second setup in the same process fails and a new UIAbility process is used for a new engine lifetime.

## Public native integration

Package [engine_host_openharmony.h](../engine_host_openharmony.h) and [bridge_openharmony.h](../bridge_openharmony.h) into the common native include directory. The former exposes `godot_host_*`; the latter owns the shared input DTOs and engine-thread-only `godot_touch/mouse/key` conversion API. Native start receives copied arguments, the actual comma-separated granted permissions, and `packaged_game`; caller-owned resources and window must remain alive through stop/join.

The common shell requires C++17 and [runtime_paths.cpp](../../../misc/dist/openharmony_template/entry/src/main/cpp/runtime_paths.cpp). Its CMake imported library is `entry/libs/${OHOS_ARCH}/libgodot.so`; it links the engine plus `ace_napi.z`, `native_window`, `rawfile.z`, and `dl`. Engine builds compile the new host via the platform's existing C++ source glob and retain the platform VSync/pthread/crash-handler dependencies.
