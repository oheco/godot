@tool
extends EditorPlugin

const HOME_DIR := "/storage/Users/currentUser"
const SHOW_IN_EXPLORER := 15
const OPEN_IN_TERMINAL := 17
var report: Dictionary = {}
var last_action := ""
var initialized := false
var input_dialog: AcceptDialog
var input_line: LineEdit
var input_text: TextEdit
var hover_button: MenuButton
var geometry_report_at := 0

func _enter_tree() -> void:
	_check_defaults.call_deferred()

func _write_report() -> void:
	var output := FileAccess.open("res://desktop_report.json", FileAccess.WRITE)
	if output:
		output.store_string(JSON.stringify(report, "\t"))
	else:
		push_error("Cannot write desktop acceptance report")

func _check_defaults() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var settings := EditorInterface.get_editor_settings()
	var mode := int(settings.get_setting("interface/editor/appearance/display_scale"))
	var editor_scale := EditorInterface.get_editor_scale()
	var screen_scale := DisplayServer.screen_get_scale()
	var dialog := FileDialog.new()
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	report = {
		"display": DisplayServer.get_name(),
		"editor_scale": editor_scale,
		"screen_scale": screen_scale,
		"scale_mode": mode,
		"scale_ok": mode != 0 or is_equal_approx(editor_scale, screen_scale),
		"file_dialog_default": dialog.current_dir,
		"system_desktop": OS.get_system_dir(OS.SYSTEM_DIR_DESKTOP),
		"default_project_path": settings.get_setting("filesystem/directories/default_project_path"),
		"home_ok": dialog.current_dir == HOME_DIR,
		"actions": {},
	}
	dialog.free()
	_write_report()
	await get_tree().process_frame
	if DisplayServer.get_name() != "headless":
		var image := get_viewport().get_texture().get_image()
		if image:
			report["frame_save_error"] = image.save_png("res://editor_frame.png")
	_write_report()
	initialized = true
	print("OHOS_DESKTOP_ACCEPTANCE ", JSON.stringify(report))

func _process(_delta: float) -> void:
	if is_instance_valid(input_dialog) and input_dialog.visible and Time.get_ticks_msec() >= geometry_report_at:
		geometry_report_at = Time.get_ticks_msec() + 500
		report["input_geometry"] = {
			"window_position": str(DisplayServer.window_get_position()),
			"root_position": str(get_tree().root.position),
			"screen_mouse": str(DisplayServer.mouse_get_position()),
			"viewport_mouse": str(get_viewport().get_mouse_position()),
			"line_rect": str(input_line.get_global_rect()),
			"text_rect": str(input_text.get_global_rect()),
			"text_caret": str(input_text.get_caret_draw_pos()),
			"popup_visible": hover_button.get_popup().visible,
			"popup_focused_item": hover_button.get_popup().get_focused_item(),
		}
		_write_report()
	if not initialized or not FileAccess.file_exists("res://action.txt"):
		return
	var action := FileAccess.get_file_as_string("res://action.txt").strip_edges()
	if action == last_action or action.is_empty():
		return
	last_action = action
	if action == "terminal":
		_dispatch.call_deferred(OPEN_IN_TERMINAL, action)
	elif action == "files":
		_dispatch.call_deferred(SHOW_IN_EXPLORER, action)
	elif action.begins_with("input-probe"):
		_show_input_probe.call_deferred()
	elif action == "home-files":
		var start := Time.get_ticks_msec()
		var error := OS.shell_show_in_file_manager(HOME_DIR, true)
		report["actions"][action] = {"enqueue_error": error, "elapsed_ms": Time.get_ticks_msec() - start}
		_write_report()

func _connected_to(source: Object, signal_name: StringName, target: Object) -> bool:
	for connection in source.get_signal_connection_list(signal_name):
		var callback: Callable = connection["callable"]
		if callback.get_object() == target:
			return true
	return false

func _dispatch(menu_id: int, action: String) -> void:
	var dock := EditorInterface.get_file_system_dock()
	dock.navigate_to_path("res://")
	await get_tree().process_frame
	var filesystem_tree: Tree = null
	for node in dock.find_children("*", "Tree", true, false):
		if _connected_to(node, &"item_mouse_selected", dock):
			filesystem_tree = node as Tree
			break
	if filesystem_tree == null or filesystem_tree.get_root() == null:
		_fail(action, "FileSystem Tree is not ready")
		return
	var resource_root := filesystem_tree.get_root().get_first_child()
	while resource_root != null and resource_root.get_metadata(0) != "res://":
		resource_root = resource_root.get_next()
	if resource_root == null:
		_fail(action, "Actual resource root is missing")
		return
	filesystem_tree.deselect_all()
	filesystem_tree.set_selected(resource_root, 0)
	filesystem_tree.ensure_cursor_is_visible()
	var position := filesystem_tree.get_item_area_rect(resource_root, 0).get_center()
	filesystem_tree.emit_signal(&"item_mouse_selected", position, MOUSE_BUTTON_RIGHT)
	var menu: PopupMenu = null
	for child in dock.get_children():
		var popup := child as PopupMenu
		if popup == null or not popup.visible:
			continue
		if not _connected_to(popup, &"id_pressed", dock):
			continue
		if popup.get_item_index(SHOW_IN_EXPLORER) < 0 or popup.get_item_index(OPEN_IN_TERMINAL) < 0:
			continue
		menu = popup
		break
	if menu == null:
		_fail(action, "Actual FileSystem context menu did not open")
		return
	var index := menu.get_item_index(menu_id)
	if index < 0 or menu.is_item_disabled(index):
		menu.hide()
		_fail(action, "Context menu action is disabled")
		return
	var label := menu.get_item_text(index)
	menu.hide()
	var start := Time.get_ticks_msec()
	var error: int = menu.emit_signal(&"id_pressed", menu_id)
	var elapsed := Time.get_ticks_msec() - start
	await get_tree().process_frame
	report["actions"][action] = {
		"menu_id": menu_id, "label": label, "dispatch_error": error,
		"elapsed_ms": elapsed, "next_frame_reached": true,
		"target": ProjectSettings.globalize_path("res://"),
	}
	_write_report()
	print("OHOS_DESKTOP_ACCEPTANCE_ACTION ", action, " ", JSON.stringify(report["actions"][action]))

func _show_input_probe() -> void:
	if not is_instance_valid(input_dialog):
		var scale := EditorInterface.get_editor_scale()
		input_dialog = AcceptDialog.new()
		input_dialog.title = "OpenHarmony input acceptance"
		EditorInterface.get_base_control().add_child(input_dialog)
		var column := VBoxContainer.new()
		column.custom_minimum_size = Vector2(620, 240) * scale
		input_dialog.add_child(column)
		var instructions := Label.new()
		instructions.text = "中文输入：检查候选面板位于输入框下方；移动窗口后再检查。\n菜单：移到第二、第三项，检查悬停高亮。"
		column.add_child(instructions)
		input_line = LineEdit.new()
		input_line.placeholder_text = "LineEdit：在这里输入拼音"
		column.add_child(input_line)
		input_text = TextEdit.new()
		input_text.custom_minimum_size.y = 120 * scale
		input_text.placeholder_text = "TextEdit：在这里输入拼音"
		column.add_child(input_text)
		hover_button = MenuButton.new()
		hover_button.text = "Hover test menu"
		column.add_child(hover_button)
		for label in ["First item", "Second item", "Third item"]:
			hover_button.get_popup().add_item(label)
	input_dialog.popup_centered()
	input_line.grab_focus()

func _exit_tree() -> void:
	if is_instance_valid(input_dialog):
		input_dialog.free()

func _fail(action: String, message: String) -> void:
	report["actions"][action] = {"error": message}
	_write_report()
	push_error(message)
