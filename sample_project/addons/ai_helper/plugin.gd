# addons/ai_helper/plugin.gd
@tool
extends EditorPlugin

var dock # Keep a reference to the dock control

# Called when the plugin is enabled in Project Settings -> Plugins
func _enter_tree():
	# Create the main container for the dock UI
	dock = VBoxContainer.new()
	dock.name = "AI" # Consistent naming is good practice

	# Create the text input field
	var prompt_edit = TextEdit.new()
	prompt_edit.name = "PromptEdit" # Use a name to easily find it later
	prompt_edit.placeholder_text = "Enter prompt for AI..."
	prompt_edit.size_flags_vertical = Control.SIZE_EXPAND_FILL # Make it fill available vertical space
	dock.add_child(prompt_edit)

	# Create the button to trigger the action
	var run_button = Button.new()
	run_button.name = "RunButton" # Name for access
	run_button.text = "Request Actions"
	# Connect the button's 'pressed' signal to our custom function
	run_button.pressed.connect(_on_run_button_pressed)
	dock.add_child(run_button)

	# Add the container (dock) to the editor UI.
	# Choose a suitable dock slot (e.g., DOCK_SLOT_LEFT_UL, DOCK_SLOT_BOTTOM_LR)
	add_control_to_dock(DOCK_SLOT_RIGHT_UL, dock) # Example: Top-right dock

# Called when the plugin is disabled
func _exit_tree():
	# Clean up: Remove the dock and free its memory
	if dock:
		remove_control_from_docks(dock)
		dock.queue_free()
		dock = null # Clear the reference

# Custom function executed when the 'RunButton' is pressed
func _on_run_button_pressed():
	# Find the TextEdit node using its name
	var prompt_edit = dock.get_node("PromptEdit") as TextEdit
	if not prompt_edit:
		printerr("AI Plugin: Could not find PromptEdit node!")
		return

	var prompt_text: String = prompt_edit.text

	# Check if the C++ singleton exists before trying to call it
	if Engine.has_singleton("AI"):
		var ai = Engine.get_singleton("AI")
		print("AI Plugin: Sending prompt: '", prompt_text, "'")
		# Call the C++ method
		var result: Array = ai.request_actions(prompt_text)
		# Output the result to the Godot console
		print("AI Plugin: Received result: ", result)
	else:
		printerr("AI Plugin: AI singleton not found. Module might be disabled or not compiled.")

