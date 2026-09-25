# Godot Engine contributors. SPDX-License-Identifier: MIT
@tool
extends EditorScript
## Run from the Script editor's File > Run menu, NOT Run Project/F6.
## Creates only local-device shaders/pipelines, never dispatches GPU work, and
## does not modify project.godot, scenes, or the active renderer.
## Remove the temporary project copy after testing; it is not a game script.

const OUTPUT_BASE := "res://.godot/gpu-compute-probe"
const PREFIX := "[GPU_COMPUTE_PROBE]"

var _output_dir: String = ""
var _text_log: FileAccess
var _report: Dictionary = {}


func _run() -> void:
	var stamp := Time.get_datetime_string_from_system(true).replace(":", "-")
	var run_id := "%s-pid%d-%d" % [stamp, OS.get_process_id(), Time.get_ticks_msec()]
	_output_dir = ProjectSettings.globalize_path(_output_base().path_join(run_id))
	var directory_error := DirAccess.make_dir_recursive_absolute(_output_dir)
	if directory_error != OK:
		push_error("%s Cannot create output directory: %s (error %d)" % [_log_prefix(), _output_dir, directory_error])
		return
	_text_log = FileAccess.open(_output_dir.path_join("probe.log"), FileAccess.WRITE)
	if _text_log == null:
		push_error("%s Cannot open probe.log (error %d)" % [_log_prefix(), FileAccess.get_open_error()])
		return
	_report = {
		"probe_version": 1,
		"run_id": run_id,
		"pid": OS.get_process_id(),
		"os": OS.get_name(),
		"engine": Engine.get_version_info(),
		"rendering_driver": RenderingServer.get_current_rendering_driver_name(),
		"rendering_method": RenderingServer.get_current_rendering_method(),
		"adapter_api_version": RenderingServer.get_video_adapter_api_version(),
		"output_dir": _output_dir,
		"state": "running",
		"active_case": {},
		"cases": [],
		"notes": [
			"Sequential pipeline creation only. No textures/buffers are bound and no GPU dispatch is performed.",
			"Uses a separate local RenderingDevice in the SAME application process, not the screen RenderingDevice.",
			"GLSL allow_cache=false; the local RD does not create Godot's main-device pipeline cache. Driver-internal caches are not controlled.",
			"Saved .input.spv files are GLSL compiler output. The Vulkan backend may transform them before vkCreateShaderModule.",
			"Format STORAGE support alone does not prove every required shader capability is enabled.",
			"SPV01 rgba1010108 is a driver diagnostic; this probe does not assume it names a particular standard format."
		]
	}
	_note("BEGIN run=%s output=%s" % [run_id, _output_dir])
	_checkpoint("create_local_device")
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		_finish("LOCAL_DEVICE_UNAVAILABLE")
		return
	_report["device_name"] = rd.get_device_name()
	_report["device_vendor"] = rd.get_device_vendor_name()
	_note("DEVICE name=%s vendor=%s api=%s" % [_report["device_name"], _report["device_vendor"], _report["adapter_api_version"]])
	var cases: Array[Dictionary] = _make_cases()
	for spec in cases:
		var result: Dictionary = _run_case(rd, spec)
		_report["cases"].append(result)
		_checkpoint("between_cases")
	rd.free()
	var counts: Dictionary = {}
	for result: Dictionary in _report["cases"]:
		var status: String = result["status"]
		counts[status] = int(counts.get(status, 0)) + 1
	_report["summary"] = counts
	_report["active_case"] = {}
	_note("SUMMARY %s" % JSON.stringify(counts))
	_finish("COMPLETE")


func _output_base() -> String:
	return OUTPUT_BASE


func _log_prefix() -> String:
	return PREFIX


func _make_cases() -> Array[Dictionary]:
	var header := "#version 450\nlayout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
	var cases: Array[Dictionary] = [
		{
			"name": "00_noop", "format": -1, "format_name": "none", "qualifier": "none",
			"source": header + "void main() {}\n"
		},
		{
			"name": "01_storage_buffer", "format": -1, "format_name": "none", "qualifier": "none",
			"source": header + "layout(set = 0, binding = 0, std430) buffer Output { uint value; } output_data;\nvoid main() { output_data.value = 0x12345678u; }\n"
		}
	]
	var formats: Array[Dictionary] = [
		{"qualifier": "r32f", "format": RenderingDevice.DATA_FORMAT_R32_SFLOAT, "format_name": "R32_SFLOAT"},
		{"qualifier": "rgba8", "format": RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, "format_name": "R8G8B8A8_UNORM"},
		{"qualifier": "rgba16f", "format": RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT, "format_name": "R16G16B16A16_SFLOAT"},
		{"qualifier": "rgba32f", "format": RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT, "format_name": "R32G32B32A32_SFLOAT"},
		{"qualifier": "rgb10_a2", "format": RenderingDevice.DATA_FORMAT_A2B10G10R10_UNORM_PACK32, "format_name": "A2B10G10R10_UNORM_PACK32"},
		{"qualifier": "r11f_g11f_b10f", "format": RenderingDevice.DATA_FORMAT_B10G11R11_UFLOAT_PACK32, "format_name": "B10G11R11_UFLOAT_PACK32"}
	]
	for index in formats.size():
		var spec: Dictionary = formats[index].duplicate()
		var qualifier: String = spec["qualifier"]
		spec["name"] = "%02d_%s" % [index + 2, qualifier]
		spec["source"] = header + "layout(%s, set = 0, binding = 0) uniform restrict writeonly image2D output_image;\nvoid main() { imageStore(output_image, ivec2(0, 0), vec4(1.0)); }\n" % qualifier
		cases.append(spec)
	return cases


func _run_case(rd: RenderingDevice, spec: Dictionary) -> Dictionary:
	var case_name: String = spec["name"]
	var result: Dictionary = {
		"name": case_name,
		"format": spec["format"],
		"format_name": spec["format_name"],
		"qualifier": spec["qualifier"],
		"status": "PENDING",
		"glsl_file": case_name + ".comp.glsl",
		"input_spirv_file": case_name + ".input.spv"
	}
	_report["active_case"] = result
	_note("CASE_BEGIN name=%s qualifier=%s" % [case_name, spec["qualifier"]])
	_checkpoint("save_glsl")
	var source_text: String = spec["source"]
	if not _write_bytes(result["glsl_file"], source_text.to_utf8_buffer()):
		return _case_done(result, "IO_FAILED")
	var storage_supported := true
	if int(spec["format"]) >= 0:
		_checkpoint("query_format_support")
		storage_supported = rd.texture_is_format_supported_for_usage(spec["format"], RenderingDevice.TEXTURE_USAGE_STORAGE_BIT)
		result["storage_supported"] = storage_supported
		result["color_attachment_supported"] = rd.texture_is_format_supported_for_usage(spec["format"], RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT)
		_note("FORMAT name=%s storage=%s color_attachment=%s" % [case_name, storage_supported, result["color_attachment_supported"]])

	var shader_source := RDShaderSource.new()
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	shader_source.source_compute = source_text
	_checkpoint("glsl_to_spirv")
	var spirv := rd.shader_compile_spirv_from_source(shader_source, false)
	if spirv == null:
		return _case_done(result, "SPIRV_RESOURCE_MISSING")
	result["glsl_error"] = spirv.compile_error_compute
	if not spirv.compile_error_compute.is_empty():
		_note("GLSL_ERROR name=%s error=%s" % [case_name, spirv.compile_error_compute])
		return _case_done(result, "GLSL_FAILED")
	var bytecode: PackedByteArray = spirv.bytecode_compute
	result["input_spirv_bytes"] = bytecode.size()
	if bytecode.is_empty():
		return _case_done(result, "SPIRV_EMPTY")
	if not _write_bytes(result["input_spirv_file"], bytecode):
		return _case_done(result, "IO_FAILED")
	_note("SPIRV_OK name=%s bytes=%d" % [case_name, bytecode.size()])
	# Do not deliberately submit a storage format the device says it cannot use.
	# Still keep its GLSL and SPIR-V for comparison with the supported cases.
	if not storage_supported:
		return _case_done(result, "SKIP_STORAGE_UNSUPPORTED")

	_checkpoint("create_shader")
	var shader := rd.shader_create_from_spirv(spirv, "oheco_gpu_probe_" + case_name)
	result["shader_valid"] = shader.is_valid()
	if not shader.is_valid():
		return _case_done(result, "SHADER_FAILED")
	_note("PIPELINE_BEGIN name=%s ticks_us=%d" % [case_name, Time.get_ticks_usec()])
	_checkpoint("create_compute_pipeline")
	var started := Time.get_ticks_usec()
	var pipeline := rd.compute_pipeline_create(shader)
	result["pipeline_ms"] = float(Time.get_ticks_usec() - started) / 1000.0
	result["pipeline_valid"] = pipeline.is_valid() and rd.compute_pipeline_is_valid(pipeline)
	if pipeline.is_valid():
		rd.free_rid(pipeline)
	rd.free_rid(shader)
	return _case_done(result, "PIPELINE_CREATED" if result["pipeline_valid"] else "PIPELINE_FAILED")


func _case_done(result: Dictionary, status: String) -> Dictionary:
	result["status"] = status
	_note("CASE_END name=%s status=%s" % [result["name"], status])
	_checkpoint("case_complete")
	return result


func _write_bytes(relative_path: String, bytes: PackedByteArray) -> bool:
	var file := FileAccess.open(_output_dir.path_join(relative_path), FileAccess.WRITE)
	if file == null:
		push_error("%s Cannot write %s (error %d)" % [_log_prefix(), relative_path, FileAccess.get_open_error()])
		return false
	file.store_buffer(bytes)
	file.flush()
	var error := file.get_error()
	file.close()
	return error == OK


func _checkpoint(stage: String) -> void:
	_report["active_stage"] = stage
	_report["updated_utc"] = Time.get_datetime_string_from_system(true)
	# Keep the last stage on disk if a driver call crashes or hangs.
	_write_bytes("report.json", JSON.stringify(_report, "\t").to_utf8_buffer())


func _note(message: String) -> void:
	var line := "%s %s %s" % [Time.get_datetime_string_from_system(true), _log_prefix(), message]
	print(line)
	if _text_log != null:
		_text_log.store_line(line)
		_text_log.flush()


func _finish(state: String) -> void:
	_report["state"] = state
	_checkpoint("finished")
	_note("END state=%s report=%s" % [state, _output_dir.path_join("report.json")])
	_text_log.close()
	_text_log = null
