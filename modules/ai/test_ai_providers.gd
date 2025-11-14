extends Node

# Test script for the AI module and providers
# Run this in the Godot editor to test functionality

var test_results = []

func _ready():
	print("\n" + "=".repeat(60))
	print("AI Module Provider Tests")
	print("=".repeat(60) + "\n")
	
	# Run tests
	test_simulation_mode()
	test_provider_creation()
	test_provider_configuration()
	test_validation()
	test_provider_switching()
	
	# Print summary
	print_summary()

func test_simulation_mode():
	print("TEST: Dummy Provider Mode")
	print("-" * 40)
	
	var dummy = DummyProvider.new()
	AI.provider = dummy
	var actions = AI.request_actions("Create a test node")
	
	if actions.size() > 0:
		print("  ✓ DummyProvider returned actions")
		print("  ✓ Action count: %d" % actions.size())
		test_results.append({"name": "Dummy Provider Mode", "passed": true})
	else:
		print("  ✗ DummyProvider failed to return actions")
		test_results.append({"name": "Dummy Provider Mode", "passed": false})
	
	print()

func test_provider_creation():
	print("TEST: Provider Creation")
	print("-" * 40)
	
	var all_passed = true
	
	# Test Dummy provider
	var dummy = DummyProvider.new()
	if dummy != null:
		print("  ✓ DummyProvider created successfully")
	else:
		print("  ✗ Failed to create DummyProvider")
		all_passed = false
	
	# Test OpenAI provider
	var openai = OpenAIProvider.new()
	if openai != null:
		print("  ✓ OpenAIProvider created successfully")
	else:
		print("  ✗ Failed to create OpenAIProvider")
		all_passed = false
	
	# Test Gemini provider
	var gemini = GeminiProvider.new()
	if gemini != null:
		print("  ✓ GeminiProvider created successfully")
	else:
		print("  ✗ Failed to create GeminiProvider")
		all_passed = false
	
	# Test x.ai provider
	var xai = XAIProvider.new()
	if xai != null:
		print("  ✓ XAIProvider created successfully")
	else:
		print("  ✗ Failed to create XAIProvider")
		all_passed = false
	
	test_results.append({"name": "Provider Creation", "passed": all_passed})
	print()

func test_provider_configuration():
	print("TEST: Provider Configuration")
	print("-" * 40)
	
	var all_passed = true
	
	var provider = OpenAIProvider.new()
	
	# Test API key
	provider.api_key = "test-key-123"
	if provider.api_key == "test-key-123":
		print("  ✓ API key set/get works")
	else:
		print("  ✗ API key set/get failed")
		all_passed = false
	
	# Test model
	provider.model = "gpt-4"
	if provider.model == "gpt-4":
		print("  ✓ Model set/get works")
	else:
		print("  ✗ Model set/get failed")
		all_passed = false
	
	# Test temperature
	provider.temperature = 0.5
	if abs(provider.temperature - 0.5) < 0.001:
		print("  ✓ Temperature set/get works")
	else:
		print("  ✗ Temperature set/get failed")
		all_passed = false
	
	# Test max_tokens
	provider.max_tokens = 1000
	if provider.max_tokens == 1000:
		print("  ✓ Max tokens set/get works")
	else:
		print("  ✗ Max tokens set/get failed")
		all_passed = false
	
	# Test base_url
	provider.base_url = "custom.api.com"
	if provider.base_url == "custom.api.com":
		print("  ✓ Base URL set/get works")
	else:
		print("  ✗ Base URL set/get failed")
		all_passed = false
	
	test_results.append({"name": "Provider Configuration", "passed": all_passed})
	print()

func test_validation():
	print("TEST: Command Validation")
	print("-" * 40)
	
	var all_passed = true
	
	# Test valid command
	var valid_json = '{"action": "create_node", "args": {"node_name": "Test", "node_type": "Node2D"}}'
	var result = AI.validate_command_json(valid_json)
	if result["valid"] == true:
		print("  ✓ Valid command passed validation")
	else:
		print("  ✗ Valid command failed validation: " + result["error"])
		all_passed = false
	
	# Test invalid JSON
	var invalid_json = '{"action": "create_node", "args": {'
	result = AI.validate_command_json(invalid_json)
	if result["valid"] == false:
		print("  ✓ Invalid JSON correctly rejected")
	else:
		print("  ✗ Invalid JSON incorrectly accepted")
		all_passed = false
	
	# Test missing action
	var missing_action = '{"args": {"node_name": "Test"}}'
	result = AI.validate_command_json(missing_action)
	if result["valid"] == false:
		print("  ✓ Missing action correctly rejected")
	else:
		print("  ✗ Missing action incorrectly accepted")
		all_passed = false
	
	# Test unknown action
	var unknown_action = '{"action": "unknown_action", "args": {}}'
	result = AI.validate_command_json(unknown_action)
	if result["valid"] == false:
		print("  ✓ Unknown action correctly rejected")
	else:
		print("  ✗ Unknown action incorrectly accepted")
		all_passed = false
	
	# Test missing required args
	var missing_args = '{"action": "create_node", "args": {"node_name": "Test"}}'
	result = AI.validate_command_json(missing_args)
	if result["valid"] == false:
		print("  ✓ Missing required args correctly rejected")
	else:
		print("  ✗ Missing required args incorrectly accepted")
		all_passed = false
	
	test_results.append({"name": "Command Validation", "passed": all_passed})
	print()

func test_provider_switching():
	print("TEST: Provider Switching")
	print("-" * 40)
	
	var all_passed = true
	
	# Create different providers
	var openai = OpenAIProvider.new()
	openai.api_key = "openai-key"
	openai.model = "gpt-4"
	
	var gemini = GeminiProvider.new()
	gemini.api_key = "gemini-key"
	gemini.model = "gemini-pro"
	
	# Switch to OpenAI
	AI.provider = openai
	if AI.provider != null and AI.provider.api_key == "openai-key":
		print("  ✓ Switched to OpenAI provider")
	else:
		print("  ✗ Failed to switch to OpenAI provider")
		all_passed = false
	
	# Switch to Gemini
	AI.provider = gemini
	if AI.provider != null and AI.provider.api_key == "gemini-key":
		print("  ✓ Switched to Gemini provider")
	else:
		print("  ✗ Failed to switch to Gemini provider")
		all_passed = false
	
	# Test switching to dummy provider
	var dummy = DummyProvider.new()
	AI.provider = dummy
	if AI.provider != null and AI.provider is DummyProvider:
		print("  ✓ Switched to DummyProvider")
	else:
		print("  ✗ Failed to switch to DummyProvider")
		all_passed = false
	
	test_results.append({"name": "Provider Switching", "passed": all_passed})
	print()

func print_summary():
	print("\n" + "=".repeat(60))
	print("Test Summary")
	print("=".repeat(60) + "\n")
	
	var passed_count = 0
	var total_count = test_results.size()
	
	for result in test_results:
		var status = "✓ PASSED" if result["passed"] else "✗ FAILED"
		print("  %s: %s" % [result["name"], status])
		if result["passed"]:
			passed_count += 1
	
	print("\n" + "-".repeat(60))
	print("Total: %d/%d tests passed" % [passed_count, total_count])
	
	if passed_count == total_count:
		print("✓ All tests passed!")
	else:
		print("✗ Some tests failed. Review output above.")
	
	print("=".repeat(60) + "\n")
	
	# Note about API tests
	print("NOTE: These tests do not make actual API calls.")
	print("To test real API connectivity:")
	print("  1. Create a real provider (OpenAI, Gemini, or XAI)")
	print("  2. Set a valid API key on the provider")
	print("  3. Set AI.provider to your configured provider")
	print("  4. Call AI.request_actions() with a prompt")
	print()

# Optional: Test with real API (requires valid key)
func test_real_api_call(provider_name: String, api_key: String):
	print("\n" + "=".repeat(60))
	print("OPTIONAL: Real API Test - %s" % provider_name)
	print("=".repeat(60) + "\n")
	
	var provider
	match provider_name.to_lower():
		"openai":
			provider = OpenAIProvider.new()
			provider.model = "gpt-4o-mini"
		"gemini":
			provider = GeminiProvider.new()
			provider.model = "gemini-pro"
		"xai":
			provider = XAIProvider.new()
			provider.model = "grok-beta"
		_:
			print("Unknown provider: " + provider_name)
			return
	
	provider.api_key = api_key
	AI.provider = provider
	
	print("Making real API call...")
	print("(This may take a few seconds)\n")
	
	var actions = AI.request_actions("Create a simple Sprite2D node named TestSprite")
	
	if actions.size() > 0:
		print("✓ API call successful!")
		print("  Received %d actions:" % actions.size())
		for i in range(actions.size()):
			print("    %d. %s" % [i + 1, JSON.stringify(actions[i])])
	else:
		print("✗ API call failed or returned no actions")
		print("  Check the console for error messages")
	
	print()

# Uncomment and set your API key to test real API calls:
# func _ready():
#     test_real_api_call("openai", "your-api-key-here")

