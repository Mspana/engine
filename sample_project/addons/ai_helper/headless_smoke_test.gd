extends SceneTree

# Headless smoke test for Godot 4.4 AI Helper module
# Run with: bin/godot.windows.editor.x86_64.exe --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd

var frame_count: int = 0
var player_node: Node = null
var initial_position: Vector2 = Vector2.ZERO
var test_started: bool = false

const INPUT_START_FRAME = 10
const INPUT_END_FRAME = 40
const TOTAL_FRAMES = 60

func _initialize():
	print("\n" + "=".repeat(60))
	print("SMOKE: Starting headless smoke test")
	print("=".repeat(60))
	
	# Load the main scene from project settings
	var main_scene_path = ProjectSettings.get_setting("application/run/main_scene")
	if main_scene_path == null or main_scene_path == "":
		print("SMOKE: ERROR - No main scene configured in project settings")
		quit(1)
		return
	
	print("SMOKE: Loading main scene: ", main_scene_path)
	
	# Load and instance the main scene
	var scene = load(main_scene_path)
	if scene == null:
		print("SMOKE: ERROR - Failed to load main scene: ", main_scene_path)
		quit(1)
		return
	
	var scene_instance = scene.instantiate()
	if scene_instance == null:
		print("SMOKE: ERROR - Failed to instantiate main scene")
		quit(1)
		return
	
	# Add the scene to the root
	get_root().add_child(scene_instance)
	print("SMOKE: Main scene loaded successfully")
	
	# Find the Player node
	player_node = get_root().find_child("Player", true, false)
	
	if player_node == null:
		print("SMOKE: WARNING - Player node not found")
		print("SMOKE: Test will continue but no input simulation will occur")
	else:
		print("SMOKE: Player found")
		
		# Get initial position
		if player_node.has_method("get_position"):
			initial_position = player_node.position
		else:
			print("SMOKE: WARNING - Player node has no position property")
		
		print("SMOKE: Found Player at ", initial_position)
		print("SMOKE: Simulating ui_right input for ", INPUT_END_FRAME - INPUT_START_FRAME, " frames")
	
	test_started = true

func _process(delta: float) -> bool:
	if not test_started:
		return false  # Return false to continue
	
	frame_count += 1
	
	# Simulate input between INPUT_START_FRAME and INPUT_END_FRAME
	if player_node != null:
		if frame_count == INPUT_START_FRAME:
			# Start pressing ui_right
			Input.action_press("ui_right")
		elif frame_count == INPUT_END_FRAME:
			# Release ui_right
			Input.action_release("ui_right")
	
	# End test after TOTAL_FRAMES
	if frame_count >= TOTAL_FRAMES:
		_finalize_test()
		return true  # Return true to exit the main loop
	
	return false  # Return false to continue processing

func _finalize_test():
	print("\n" + "-".repeat(60))
	
	if player_node != null:
		var final_position = player_node.position
		print("SMOKE: After input, Player at ", final_position)
		
		var delta = final_position - initial_position
		var distance_moved = delta.length()
		print("SMOKE: Delta: ", delta, " [distance: ", distance_moved, " units]")
		
		if distance_moved > 0.1:
			print("SMOKE: Test completed successfully ✓")
		else:
			print("SMOKE: WARNING - Player did not move (distance: ", distance_moved, ")")
			print("SMOKE: Test completed with warnings")
	else:
		print("SMOKE: Test completed (Player node not found)")
	
	print("SMOKE: Done.")
	print("=".repeat(60) + "\n")
	
	# Clean up input state
	Input.action_release("ui_right")
	
	# Exit cleanly
	quit(0)

