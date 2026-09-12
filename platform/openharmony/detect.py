import os
from pathlib import Path

from methods import print_error
from platform_methods import validate_arch


def get_name():
    return "OpenHarmony"


def can_build():
    return True


def get_tools(env):
    return ["clang", "clang++", "as", "ar", "link"]


def get_opts():
    from SCons.Variables import BoolVariable
    return [
        ("OPENHARMONY_SDK_PATH", "OpenHarmony SDK root (or its native component)", os.environ.get("OHOS_SDK_HOME", "")),
        BoolVariable("generate_bundle", "Generate the DevEco project archive", False),
    ]


def get_doc_classes():
    return ["EditorExportPlatformOpenHarmony"]


def get_doc_path():
    return "doc_classes"


def get_flags():
    return {
        "arch": "arm64",
        "target": "editor",
        "supported": ["mono"],
        "builtin_pcre2_with_jit": False,
        "vulkan": True,
        "opengl3": False,
    }


def configure(env):
    validate_arch(env["arch"], get_name(), ["arm64", "x86_64"])
    sdk = Path(env["OPENHARMONY_SDK_PATH"]).expanduser().resolve()
    native = sdk / "native" if (sdk / "native/sysroot").is_dir() else sdk
    if not (native / "sysroot/usr/include").is_dir():
        print_error("Set OPENHARMONY_SDK_PATH to a prepared OpenHarmony SDK.")
        raise SystemExit(255)
    if env["opengl3"] or not env["vulkan"]:
        print_error("This OpenHarmony port requires vulkan=yes opengl3=no.")
        raise SystemExit(255)
    env["OPENHARMONY_NATIVE_SDK"] = str(native)
    target = "aarch64-linux-ohos" if env["arch"] == "arm64" else "x86_64-linux-ohos"
    compiler_dir = native / "llvm/bin"
    exe = ".exe" if os.name == "nt" else ""
    env["CC"] = str(compiler_dir / ("clang" + exe))
    env["CXX"] = str(compiler_dir / ("clang++" + exe))
    env["LINK"] = env["CXX"]
    env["SHLINK"] = env["CXX"]
    env["S_compiler"] = env["CC"]
    env["AR"] = str(compiler_dir / ("llvm-ar" + exe))
    env["RANLIB"] = str(compiler_dir / ("llvm-ranlib" + exe))
    flags = ["--target=" + target, "--sysroot=" + str(native / "sysroot")]
    env.Append(CCFLAGS=flags + ["-fPIC", "-pthread"])
    env.Append(LINKFLAGS=flags + ["-fuse-ld=lld", "-pthread", "-Wl,--no-undefined", "-Wl,--build-id"])
    env.Append(SHLINKFLAGS=["-shared", "-Wl,-soname,libgodot.so"])
    env.Append(CPPPATH=["#platform/openharmony"])
    env.Append(CPPDEFINES=["OPENHARMONY_ENABLED", "UNIX_ENABLED", "__OPEN_HARMONY__", "_GNU_SOURCE", "VULKAN_ENABLED", "RD_ENABLED"])
    env.Append(LIBS=["m", "dl", "z", "hilog_ndk.z", "ace_ndk.z", "native_window", "ace_napi.z", "rawfile.z", "native_vsync", "ohaudio", "ohinput", "ohinputmethod", "native_display_manager", "native_window_manager", "native_drawing", "udmf", "pasteboard"])
    if not env["use_volk"]:
        env.Append(LIBS=["vulkan"])
    env["SHLIBSUFFIX"] = ".so"
    env["SHLINKCOM"] = '${TEMPFILE("$SHLINK -o $TARGET $SHLINKFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS", "$SHLINKCOMSTR")}'
