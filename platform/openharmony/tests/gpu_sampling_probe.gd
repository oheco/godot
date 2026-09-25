# Godot Engine contributors. SPDX-License-Identifier: MIT
@tool
extends "gpu_compute_probe.gd"
## Small sampling matrix + one captured VoxelGiShaderRD:0 replay.
## Run this file via Script editor > File > Run. Keep gpu_compute_probe.gd beside it.
## No textures are allocated, no dispatch occurs, and no project settings change.

const REPLAY_DIR := "res://.godot/gpu-sampling-input"
const HEADER := "#version 450\nlayout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\nlayout(set = 0, binding = 0, std430) buffer Output { vec4 value; } output_data;\nlayout(push_constant, std430) uniform Params { vec4 uvw_lod; uvec4 control; } params;\n"
var _seen_dumps: Dictionary = {}


func _output_base() -> String:
	return "res://.godot/gpu-sampling-probe"


func _log_prefix() -> String:
	return "[GPU_SAMPLING_PROBE]"


func _make_cases() -> Array[Dictionary]:
	_report["probe_kind"] = "sampling_matrix_and_captured_shader_replay"
	_report["replay_manifest"] = REPLAY_DIR.path_join("voxelgi0.json")
	_report["native_diagnostics_dir"] = OS.get_environment("GODOT_VULKAN_PIPELINE_DIAGNOSTICS_DIR")
	_report["notes"] = [
		"Pipeline creation only, serial, on a local RenderingDevice in this HAP; no GPU dispatch or resource binding.",
		"Synthetic cases all use local_size=(1,1,1), sample into an SSBO, and never write storage images.",
		"Dynamic LOD and loop count come from push constants. No fixed trip-count clamp or forced unrolling; the loop is never dispatched.",
		"The captured replay keeps its original workgroup size, bindings and push constants, and has no specialization constants.",
		"shader_create_from_spirv can run backend transformations again. A successful replay alone does NOT prove a cache/concurrency cause.",
		"On failure, match the native diagnostic by shader name, PID and timestamp; compare its final SPIR-V with the probe input.",
		"Only native failures are dumped. For successful pipelines the final submitted bytecode cannot be verified by this script."
	]
	_seen_dumps.clear()
	var diagnostics: String = _report["native_diagnostics_dir"]
	if not diagnostics.is_empty():
		for file in DirAccess.get_files_at(diagnostics):
			_seen_dumps[file] = true
	var cases: Array[Dictionary] = [{
		"name": "sampling_00_buffer_baseline", "format": -1, "format_name": "none", "qualifier": "no sampling",
		"source": HEADER + "void main() { output_data.value = params.uvw_lod; }\n"
	}]
	var matrix: Array[Dictionary] = [
		{"label": "2d_combined_fixed_single", "dim": 2, "separate": false, "dynamic_lod": false, "loop": false},
		{"label": "3d_combined_fixed_single", "dim": 3, "separate": false, "dynamic_lod": false, "loop": false},
		{"label": "2d_separate_fixed_single", "dim": 2, "separate": true, "dynamic_lod": false, "loop": false},
		{"label": "3d_separate_fixed_single", "dim": 3, "separate": true, "dynamic_lod": false, "loop": false},
		{"label": "3d_separate_dynamic_single", "dim": 3, "separate": true, "dynamic_lod": true, "loop": false},
		{"label": "3d_separate_fixed_loop", "dim": 3, "separate": true, "dynamic_lod": false, "loop": true},
		{"label": "3d_separate_dynamic_loop", "dim": 3, "separate": true, "dynamic_lod": true, "loop": true},
		{"label": "2d_separate_dynamic_loop", "dim": 2, "separate": true, "dynamic_lod": true, "loop": true}
	]
	_report["sampling_matrix"] = matrix
	for index in matrix.size():
		var item: Dictionary = matrix[index]
		cases.append({
			"name": "sampling_%02d_%s" % [index + 1, item["label"]],
			"format": -1, "format_name": "sampled image / runtime format", "qualifier": item["label"],
			"source": _sampling_source(item)
		})
	cases.append({"name": "sampling_09_replay_voxelgi0", "replay": true})
	return cases


func _sampling_source(item: Dictionary) -> String:
	var dim: int = item["dim"]
	var sampler_type := "sampler%dD" % dim
	var source := HEADER
	var sampled := "source_combined"
	if item["separate"]:
		source += "layout(set = 0, binding = 1) uniform texture%dD source_texture;\nlayout(set = 0, binding = 2) uniform sampler source_sampler;\n" % dim
		sampled = "%s(source_texture, source_sampler)" % sampler_type
	else:
		source += "layout(set = 0, binding = 1) uniform %s source_combined;\n" % sampler_type
	var coordinates := "params.uvw_lod.xy" if dim == 2 else "params.uvw_lod.xyz"
	var lod := "params.uvw_lod.w" if item["dynamic_lod"] else "0.0"
	if item["loop"]:
		coordinates += " + vec%d(float(i) * 0.001)" % dim
		if item["dynamic_lod"]:
			lod += " + float(i) * 0.5"
		source += "void main() {\n    vec4 value = vec4(0.0);\n    uint count = params.control.x;\n    for (uint i = 0u; i < count; ++i) {\n        value += textureLod(%s, %s, %s);\n    }\n    output_data.value = value;\n}\n" % [sampled, coordinates, lod]
	else:
		source += "void main() { output_data.value = textureLod(%s, %s, %s); }\n" % [sampled, coordinates, lod]
	return source


func _run_case(rd: RenderingDevice, spec: Dictionary) -> Dictionary:
	var begin_usec := Time.get_ticks_usec()
	var result: Dictionary
	if spec.get("replay", false):
		result = _run_replay(rd, spec)
	else:
		result = super._run_case(rd, spec)
		result["shader_name"] = "oheco_gpu_probe_" + String(spec["name"])
		var input_path: String = _output_dir.path_join(result["input_spirv_file"])
		if FileAccess.file_exists(input_path):
			result["input_spirv_sha256"] = FileAccess.get_sha256(input_path)
	result["begin_usec"] = begin_usec
	result["end_usec"] = Time.get_ticks_usec()
	result["final_spirv_verified"] = false
	if result["status"] == "PIPELINE_FAILED":
		_attach_native_failure(result, begin_usec)
	return result


func _run_replay(rd: RenderingDevice, spec: Dictionary) -> Dictionary:
	var case_name: String = spec["name"]
	var shader_name := "oheco_replay_voxelgi0_" + String(_report["run_id"])
	var result: Dictionary = {
		"name": case_name, "shader_name": shader_name, "status": "PENDING", "replay": true,
		"input_spirv_file": case_name + ".input.spv"
	}
	_report["active_case"] = result
	_note("CASE_BEGIN name=%s source=original_failed_VoxelGiShaderRD:0" % case_name)
	_checkpoint("read_replay_metadata")
	var metadata := _read_json(REPLAY_DIR.path_join("voxelgi0.json"))
	var constants: Variant = metadata.get("specialization_constants")
	if metadata.get("shader_name") != "VoxelGiShaderRD:0" or int(metadata.get("stage", -1)) != 32 or metadata.get("entry_point") != "main" or not constants is Array:
		return _case_done(result, "REPLAY_METADATA_INVALID")
	if not constants.is_empty():
		return _case_done(result, "REPLAY_REQUIRES_SPECIALIZATION")
	var original_file: String = str(metadata.get("spirv_file", ""))
	if original_file.is_empty() or original_file.get_file() != original_file or not original_file.ends_with(".spv"):
		return _case_done(result, "REPLAY_METADATA_INVALID")
	var file := FileAccess.open(REPLAY_DIR.path_join(original_file), FileAccess.READ)
	if file == null:
		return _case_done(result, "REPLAY_INPUT_MISSING")
	var size := file.get_length()
	if size < 20 or size > 1048576 or size % 4 != 0 or size != int(metadata.get("spirv_bytes", -1)):
		file.close()
		return _case_done(result, "REPLAY_INPUT_INVALID")
	var bytes := file.get_buffer(size)
	file.close()
	if bytes.size() != size or bytes.decode_u32(0) != 0x07230203:
		return _case_done(result, "REPLAY_INPUT_INVALID")
	if not _write_bytes(result["input_spirv_file"], bytes):
		return _case_done(result, "IO_FAILED")
	var digest := FileAccess.get_sha256(_output_dir.path_join(result["input_spirv_file"]))
	result["input_spirv_sha256"] = digest
	result["input_spirv_bytes"] = bytes.size()
	result["original_shader_name"] = metadata["shader_name"]
	result["original_pid"] = metadata["pid"]
	result["original_failure_id"] = metadata["failure_id"]
	result["original_cache_used"] = metadata["pipeline_cache_used"]
	if digest != metadata.get("spirv_sha256", ""):
		return _case_done(result, "REPLAY_SHA256_MISMATCH")
	_write_bytes(case_name + ".original.json", JSON.stringify(metadata, "\t").to_utf8_buffer())
	_note("REPLAY_INPUT_OK name=%s bytes=%d sha256=%s" % [case_name, bytes.size(), digest])
	var spirv := RDShaderSPIRV.new()
	spirv.bytecode_compute = bytes
	_checkpoint("replay_create_shader")
	var shader := rd.shader_create_from_spirv(spirv, shader_name)
	result["shader_valid"] = shader.is_valid()
	if not shader.is_valid():
		return _case_done(result, "SHADER_FAILED")
	_note("PIPELINE_BEGIN name=%s shader_name=%s" % [case_name, shader_name])
	_checkpoint("replay_create_pipeline")
	var started := Time.get_ticks_usec()
	var pipeline := rd.compute_pipeline_create(shader)
	result["pipeline_ms"] = float(Time.get_ticks_usec() - started) / 1000.0
	result["pipeline_valid"] = pipeline.is_valid() and rd.compute_pipeline_is_valid(pipeline)
	if pipeline.is_valid():
		rd.free_rid(pipeline)
	rd.free_rid(shader)
	return _case_done(result, "PIPELINE_CREATED" if result["pipeline_valid"] else "PIPELINE_FAILED")


func _read_json(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var size := file.get_length()
	if size <= 0 or size > 65536:
		file.close()
		return {}
	var text := file.get_buffer(size).get_string_from_utf8()
	file.close()
	var value: Variant = JSON.parse_string(text)
	return value if value is Dictionary else {}


func _attach_native_failure(result: Dictionary, begin_usec: int) -> void:
	var directory := OS.get_environment("GODOT_VULKAN_PIPELINE_DIAGNOSTICS_DIR")
	if directory.is_empty():
		result["native_diagnostic"] = "not enabled"
		return
	for filename in DirAccess.get_files_at(directory):
		if not filename.ends_with(".json") or _seen_dumps.has(filename):
			continue
		var path := directory.path_join(filename)
		var metadata := _read_json(path)
		if metadata.is_empty():
			continue
		_seen_dumps[filename] = true
		if metadata.get("shader_name", "") != result.get("shader_name", "") or int(metadata.get("pid", -1)) != OS.get_process_id() or int(metadata.get("ticks_usec", -1)) < begin_usec:
			continue
		result["native_diagnostic_json"] = path
		result["native_cache_used"] = metadata.get("pipeline_cache_used")
		result["native_final_spirv_bytes"] = metadata.get("spirv_bytes")
		var spirv_name: String = str(metadata.get("spirv_file", ""))
		if not metadata.get("spirv_write_ok", false) or spirv_name.is_empty() or spirv_name.get_file() != spirv_name:
			continue
		var digest := FileAccess.get_sha256(directory.path_join(spirv_name))
		if digest.is_empty() or digest != metadata.get("spirv_sha256", ""):
			continue
		result["native_final_spirv_sha256"] = digest
		result["final_spirv_verified"] = true
		result["final_spirv_matches_input"] = digest == result.get("input_spirv_sha256", "")
		_note("NATIVE_CAPTURE name=%s cache=%s exact_input_match=%s file=%s" % [result["name"], result["native_cache_used"], result["final_spirv_matches_input"], path])
		return
	result["native_diagnostic"] = "no matching complete failure dump found"
