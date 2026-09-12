#!/usr/bin/env python3
"""Refresh the fixed Vulkan inputs from pre-downloaded, hash-verified archives."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--archives", type=Path, required=True)
args = parser.parse_args()
inputs = json.loads((root / "thirdparty/vulkan/openharmony-inputs.json").read_text())
for item in inputs["archives"]:
    archive = args.archives / item["file"]
    if hashlib.sha256(archive.read_bytes()).hexdigest() != item["sha256"]:
        raise SystemExit("Archive SHA-256 mismatch: " + str(archive))


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def copy_tree(source, destination):
    # The shared document filesystem does not support Unix copystat/chmod.
    destination.mkdir(parents=True, exist_ok=True)
    for path in source.rglob("*"):
        target = destination / path.relative_to(source)
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            copy_file(path, target)


with tempfile.TemporaryDirectory(prefix="godot-vulkan-update-") as temporary:
    source = {}
    for item in inputs["archives"]:
        destination = Path(temporary) / item["name"]
        destination.mkdir()
        with tarfile.open(args.archives / item["file"]) as archive:
            archive.extractall(destination, filter="data")
        source[item["name"]] = next(destination.iterdir())

    headers = source["Vulkan-Headers"]
    shutil.rmtree(root / "thirdparty/vulkan/include")
    copy_tree(headers / "include", root / "thirdparty/vulkan/include")
    for name in ["LICENSE.md"]:
        copy_file(headers / name, root / "thirdparty/vulkan" / name)
    copy_tree(headers / "LICENSES", root / "thirdparty/vulkan/LICENSES")
    # Keep registry and generator as offline reproducible inputs for volk.
    copy_file(headers / "registry/vk.xml", root / "thirdparty/vulkan/registry/vk.xml")
    volk = source["volk"]
    subprocess.run([sys.executable, str(volk / "generate.py"), str(headers / "registry/vk.xml")], cwd=volk, check=True)
    for name in ["volk.h", "volk.c", "LICENSE.md", "generate.py", "CMakeLists.txt"]:
        copy_file(volk / name, root / "thirdparty/volk" / name)

    glslang = source["glslang"]
    for folder in ["glslang", "SPIRV"]:
        target = root / "thirdparty/glslang" / folder
        shutil.rmtree(target)
        for path in (glslang / folder).rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(glslang)
            if any(part in {"HLSL", "ExtensionHeaders", "CInterface"} for part in relative.parts):
                continue
            if re.search(r"_c[_.]", path.name) or str(relative) in {"glslang/stub.cpp", "SPIRV/spirv.hpp11"}:
                continue
            copy_file(path, root / "thirdparty/glslang" / relative)
    copy_file(glslang / "LICENSE.txt", root / "thirdparty/glslang/LICENSE.txt")
    version = re.search(r"#+ *([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([a-zA-Z0-9]+))?", (glslang / "CHANGES.md").read_text())
    assert version
    build_info = (glslang / "build_info.h.tmpl").read_text()
    for key, value in zip(["major", "minor", "patch", "flavor"], version.groups()):
        build_info = build_info.replace("@" + key + "@", value or "")
    (root / "thirdparty/glslang/glslang/build_info.h").write_text(build_info)

    spirv = source["SPIRV-Headers"]
    for name in ["spirv.h", "spirv.hpp", "spirv.hpp11"]:
        relative = Path("include/spirv/unified1") / name
        copy_file(spirv / relative, root / "thirdparty/spirv-headers" / relative)
    copy_file(spirv / "LICENSE", root / "thirdparty/spirv-headers/LICENSE")

    reflect = source["SPIRV-Reflect"]
    for name in ["spirv_reflect.h", "spirv_reflect.c", "LICENSE"]:
        copy_file(reflect / name, root / "thirdparty/spirv-reflect" / name)

    cross = source["SPIRV-Cross"]
    for path in cross.iterdir():
        if path.suffix not in {".cpp", ".hpp", ".h"} or path.name == "main.cpp":
            continue
        if any(path.name.startswith(prefix) for prefix in ["spirv.h", "spirv_cross_c.", "spirv_hlsl.", "spirv_cpp."]):
            continue
        copy_file(path, root / "thirdparty/spirv-cross" / path.name)
    for folder in ["include", "LICENSES"]:
        copy_tree(cross / folder, root / "thirdparty/spirv-cross" / folder)
    (root / "thirdparty/spirv-cross/LICENSES/CC-BY-4.0.txt").unlink(missing_ok=True)
    copy_file(cross / "LICENSE", root / "thirdparty/spirv-cross/LICENSE")

    utility = source["Vulkan-Utility-Libraries"]
    copy_file(utility / "LICENSE.md", root / "thirdparty/vulkan/UTILITY-LICENSE.md")
    copy_file(utility / "include/vulkan/vk_enum_string_helper.h", root / "thirdparty/vulkan/vk_enum_string_helper.h")
    for patch in [
        "thirdparty/glslang/patches/0001-apple-disable-absolute-paths.patch",
        "thirdparty/glslang/patches/0002-apple-m1-msaa-fix.patch",
        "thirdparty/vulkan/patches/0001-VKEnumStringHelper-godot-vulkan.patch",
        "thirdparty/spirv-reflect/patches/0001-zero-size-for-sc-sized-arrays.patch",
        "thirdparty/spirv-reflect/patches/0002-spirv-headers.patch",
    ]:
        subprocess.run(["git", "apply", str(root / patch)], cwd=root, check=True)

print("Updated Vulkan headers/volk to 1.4.362; shader components to SDK 1.4.357.0")
