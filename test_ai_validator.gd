extends SceneTree

# Simple script to test the AI command validator.
# Run this from the command line: godot --headless --script test_ai_validator.gd

func _init():
	# Access the singleton directly by its registered name (assuming "AI")
	var ai_singleton = AI
	if ai_singleton == null:
		printerr("AI singleton not found. Ensure it's registered as an autoload named 'AI'.")
		quit(1)

	print("--- Testing AI Command Validator ---")

	# Test cases (JSON strings)
	var tests = {
		"Valid create_node": '{ "action": "create_node", "args": { "node_name": "MySprite", "node_type": "Sprite2D" } }',
		"Valid delete_node": '{ "action": "delete_node", "args": { "node_path": "/root/MyNode" } }',
		"Valid set_property": '{ "action": "set_property", "args": { "node_path": "/root/AnotherNode", "property_name": "position", "value": [100, 50] } }',
		"Invalid JSON": '{ "action": "create_node", "args": { node_name: "Invalid" } }', # Missing quotes
		"Missing action": '{ "args": { "node_name": "NoAction" } }',
		"Unknown action": '{ "action": "fly_to_moon", "args": {} }',
		"Missing args": '{ "action": "create_node" }',
		"create_node: Missing node_name": '{ "action": "create_node", "args": { "node_type": "Node2D" } }',
		"create_node: Wrong type node_name": '{ "action": "create_node", "args": { "node_name": 123, "node_type": "Node2D" } }',
		"delete_node: Missing node_path": '{ "action": "delete_node", "args": {} }',
		"set_property: Missing value": '{ "action": "set_property", "args": { "node_path": "/root/N", "property_name": "visible" } }'
	}

	var passed = 0
	var failed = 0

	for test_name in tests:
		var json_str = tests[test_name]
		print("\nTesting: ", test_name)
		print("Input JSON: ", json_str)
		var result = ai_singleton.validate_command_json(json_str)
		print("Result: ", result)

		# Simple validation of the test outcome (you might want more specific checks)
		if test_name.begins_with("Valid") and result.valid:
			print("Status: PASSED")
			passed += 1
		elif test_name.begins_with("Invalid") and not result.valid and result.error != "":
			print("Status: PASSED (Expected Failure)")
			passed += 1
		elif (test_name.begins_with("Missing") or test_name.begins_with("Unknown") or test_name.contains(":")) and not result.valid and result.error != "":
			print("Status: PASSED (Expected Failure)")
			passed += 1
		else:
			print("Status: FAILED")
			failed += 1

	print("\n--- Test Summary ---")
	print("Passed: %d" % passed)
	print("Failed: %d" % failed)

	# Quit the engine (necessary for script mode)
	quit(0 if failed == 0 else 1) 