@tool
extends EditorPlugin

const NODE := "/storage/Users/currentUser/.oheco/packages/nodejs/24.21.0-ohos.1/bin/node"
const BROKER_CLIENT := "res://addons/hdc_probe/broker_hdc_probe.cjs"
const RESULT_PATH := "/data/storage/el2/base/haps/entry/temp/hdc-device-probe.json"
const EXPECTED_TARGET := "192.168.1.160:34835"

var remote: MenuButton
var export_node: Node

func _enter_tree() -> void:
	call_deferred("probe")

func scan(node: Node) -> void:
	if node.get_class() == "EditorExport":
		export_node = node
	if node is MenuButton and node.tooltip_text == "Remote Deploy":
		remote = node
	for child in node.get_children():
		scan(child)

func probe() -> void:
	await get_tree().create_timer(8.0).timeout
	var settings := EditorInterface.get_editor_settings()
	var result: Dictionary = {
		"pid": OS.get_process_id(),
		"hdc_setting": settings.get("export/openharmony/hdc_path"),
		"settings": {
			"use_broker_has_setting": settings.has_setting("export/openharmony/use_broker"),
			"use_broker": settings.get("export/openharmony/use_broker") if settings.has_setting("export/openharmony/use_broker") else null,
		},
		"PATH": OS.get_environment("PATH"),
		"HOME": OS.get_environment("HOME"),
		"OHECO_ROOT": OS.get_environment("OHECO_ROOT"),
		"engine_log": "/data/storage/el2/base/haps/entry/files/godot-%d-engine.log" % OS.get_process_id(),
		"broker": {},
		"presets": [],
	}
	var client_path := ProjectSettings.globalize_path(BROKER_CLIENT)
	var output: Array[String] = []
	var ec: int = OS.execute(NODE, [client_path], output, true)
	var broker: Dictionary = {
		"node": NODE,
		"client": client_path,
		"client_exists": FileAccess.file_exists(client_path),
		"cli_exit": ec,
		"cli_output": output,
		"targets_result": null,
		"expected_target": EXPECTED_TARGET,
		"target_seen": false,
	}
	var parser := JSON.new()
	if parser.parse("".join(output)) == OK and parser.data is Dictionary:
		broker.targets_result = parser.data
		var targets_result: Dictionary = parser.data
		broker.target_seen = str(targets_result.get("stdout", "")).split("\n", false).has(EXPECTED_TARGET)
	else:
		broker.parse_error = parser.get_error_message()
	result.broker = broker
	scan(get_tree().root)
	if export_node:
		for connection in export_node.get_signal_connection_list("export_presets_runnable_updated"):
			var object: Object = connection.callable.get_object()
			if object.get_class() == "EditorExportPlatformOpenHarmony":
				for preset in object.get_current_presets():
					result.presets.append({"name": preset.get_preset_name(), "runnable": preset.is_runnable()})
	result.remote_found = remote != null
	if remote:
		result.remote_visible = remote.is_visible()
		result.remote_visible_in_tree = remote.is_visible_in_tree()
		result.remote_disabled = remote.disabled
		result.items = []
		result.items_details = []
		for item in range(remote.get_popup().item_count):
			result.items.append(remote.get_popup().get_item_text(item))
			result.items_details.append({
				"text": remote.get_popup().get_item_text(item),
				"id": remote.get_popup().get_item_id(item),
				"disabled": remote.get_popup().is_item_disabled(item),
			})
	var file := FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if file:
		file.store_string(JSON.stringify(result, "  "))
		file.close()
	print("HDC_APP_PROBE_COMPLETE ", JSON.stringify(result))
	await get_tree().create_timer(3.0).timeout
	DirAccess.remove_absolute(RESULT_PATH)
	get_tree().quit()
