# Godot Engine contributors. SPDX-License-Identifier: MIT
@tool
extends EditorPlugin

var report := ""
var next_check := 0
var last_value := -1

func _enter_tree() -> void:
	report = OS.get_environment("GODOT_OHOS_RELOAD_REPORT")
	set_process(not report.is_empty())

func _process(_delta: float) -> void:
	if report.is_empty():
		return
	if Time.get_ticks_msec() < next_check:
		return
	next_check = Time.get_ticks_msec() + 500
	var script = load("res://ReloadMarker.cs")
	if not script or not script.can_instantiate():
		return
	var marker = script.new()
	var value: int = marker.ReadVersion()
	if value != last_value:
		last_value = value
		var output := FileAccess.open(report, FileAccess.WRITE)
		if not output:
			get_tree().quit(1)
			return
		output.store_string(str(value))
		output.close()
		print("OHOS_CSHARP_RELOAD_VALUE=", value)
	if value == 200:
		print("OHOS_CSHARP_HOT_RELOAD_PASS")
		get_tree().quit()
