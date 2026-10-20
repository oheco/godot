# Godot Engine contributors. SPDX-License-Identifier: MIT
# Native-window graphics acceptance; display scanout synchronization is not measured.
extends Node3D

func _ready() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.25)
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 4.0
	camera.position = Vector3(0.0, 0.0, 4.0)
	add_child(camera)
	camera.current = true
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color(1, 0, 0)
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(2, 2)
	quad.mesh = mesh
	quad.material_override = red
	add_child(quad)
	var green := StandardMaterial3D.new()
	green.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	green.albedo_color = Color(0, 1, 0)
	var cube := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(0.6, 0.6, 0.6)
	cube.mesh = box
	cube.material_override = green
	cube.position = Vector3(0.7, 0, 0.5)
	add_child(cube)
	var overlay := CanvasLayer.new()
	add_child(overlay)
	var yellow := ColorRect.new()
	yellow.position = Vector2(12, 12)
	yellow.size = Vector2(48, 48)
	yellow.color = Color(1, 1, 0)
	overlay.add_child(yellow)
	var cyan := ColorRect.new()
	cyan.position = Vector2(260, 12)
	cyan.size = Vector2(48, 48)
	cyan.color = Color(0, 1, 1)
	overlay.add_child(cyan)

	var vsync_requests: Array = []
	var vsync_getters_match := true
	for frame in range(20):
		if frame == 0 or frame == 6 or frame == 12:
			var mode := DisplayServer.VSYNC_ENABLED if frame == 6 else DisplayServer.VSYNC_DISABLED
			DisplayServer.window_set_vsync_mode(mode)
			var actual := DisplayServer.window_get_vsync_mode()
			vsync_requests.append({"frame": frame, "requested": mode, "getter": actual})
			vsync_getters_match = vsync_getters_match and actual == mode
		await RenderingServer.frame_post_draw

	var resize_requested := OS.get_environment("GODOT_RENDERING_WINDOW_RESIZE") == "1"
	var expected_size := Vector2i(640, 480) if resize_requested else Vector2i(320, 240)
	var image := get_viewport().get_texture().get_image()
	var valid_image := image != null and not image.is_empty()
	var checks := {
		"image_valid": valid_image,
		"method_exact": RenderingServer.get_current_rendering_method() == OS.get_environment("GODOT_RENDERING_WINDOW_METHOD"),
		"driver_exact": RenderingServer.get_current_rendering_driver_name() == OS.get_environment("GODOT_RENDERING_WINDOW_DRIVER"),
		"vsync_requests_and_getters": vsync_getters_match and vsync_requests.size() == 3,
		"viewport_size": valid_image and image.get_size() == expected_size,
	}
	var sample_colors := {}
	var hash := ""
	var png_status := ERR_UNCONFIGURED
	var output := OS.get_environment("GODOT_RENDERING_WINDOW_OUTPUT")
	if valid_image and image.get_size() == expected_size:
		# The optional native resize preserves 4:3. World-space 3D samples scale;
		# the CanvasLayer rectangles retain their ordinary fixed pixel positions.
		var scale_x := float(image.get_width()) / 320.0
		var scale_y := float(image.get_height()) / 240.0
		var red_pixel := image.get_pixel(int(150 * scale_x), int(120 * scale_y))
		var green_pixel := image.get_pixel(int(202 * scale_x), int(120 * scale_y))
		var yellow_pixel := image.get_pixel(24, 24)
		var cyan_pixel := image.get_pixel(280, 24)
		var background := image.get_pixel(int(12 * scale_x), int(220 * scale_y))
		checks["red_3d"] = red_pixel.is_equal_approx(Color(1, 0, 0))
		# The nearer green cube must occlude the red quad at this sample.
		checks["green_3d_depth"] = green_pixel.is_equal_approx(Color(0, 1, 0))
		checks["yellow_2d"] = yellow_pixel.is_equal_approx(Color(1, 1, 0))
		checks["cyan_2d"] = cyan_pixel.is_equal_approx(Color(0, 1, 1))
		checks["background"] = background.b > 0.2 and background.r < 0.02 and background.g < 0.02
		sample_colors = {"red_3d": str(red_pixel), "green_3d_depth": str(green_pixel), "yellow_2d": str(yellow_pixel), "cyan_2d": str(cyan_pixel), "background": str(background)}
		var hash_context := HashingContext.new()
		hash_context.start(HashingContext.HASH_SHA256)
		hash_context.update(image.get_data())
		hash = hash_context.finish().hex_encode()
		png_status = image.save_png(output.path_join("viewport.png"))
	checks["png_saved"] = png_status == OK
	var passed := true
	for value in checks.values():
		passed = passed and value
	var receipt := {
		"pass": passed,
		"method": RenderingServer.get_current_rendering_method(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"api": RenderingServer.get_video_adapter_api_version(),
		"viewport_size": [image.get_width(), image.get_height()] if valid_image else [],
		"expected_viewport_size": [expected_size.x, expected_size.y],
		"resize_requested": resize_requested,
		"pixel_sha256": hash,
		"checks": checks,
		"sample_colors": sample_colors,
		"png_status": png_status,
		"headless": false,
		"frame_post_draw_waits": 20,
		"vsync_requests": vsync_requests,
		"display_scanout_sync_measured": false,
	}
	var file := FileAccess.open(output.path_join("viewport-receipt.json"), FileAccess.WRITE)
	if file:
		file.store_string(JSON.stringify(receipt, "  "))
		file.close()
	else:
		passed = false
	print("GRAPHICS_RECEIPT ", JSON.stringify(receipt))
	print("NATIVE_WINDOW_GRAPHICS_", "PASS" if passed else "FAIL")
	get_tree().quit(0 if passed else 31)
