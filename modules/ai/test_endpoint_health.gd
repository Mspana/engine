extends SceneTree

# Endpoint Health Check for AI Providers
# Tests if API endpoints are online and reachable
# Also validates API keys if available
# Run: godot --headless --script modules/ai/test_endpoint_health.gd

var test_results = {}
var tests_completed = 0
var total_tests = 3
var use_api_keys = true  # Set to false to skip API key validation
var root_node = null

# ANSI color codes for terminal output
const COLOR_RESET = "\u001b[0m"
const COLOR_BOLD = "\u001b[1m"
const COLOR_GREEN = "\u001b[32m"
const COLOR_RED = "\u001b[31m"
const COLOR_YELLOW = "\u001b[33m"
const COLOR_CYAN = "\u001b[36m"
const COLOR_BLUE = "\u001b[34m"
const COLOR_GRAY = "\u001b[90m"

func _init():
	print("\n" + COLOR_BOLD + COLOR_CYAN + "=".repeat(60))
	print("AI Provider Endpoint Health Check")
	print("=".repeat(60) + COLOR_RESET + "\n")
	
	if use_api_keys:
		print(COLOR_BOLD + "Mode: " + COLOR_RESET + "Testing connectivity AND API key validation\n")
	else:
		print(COLOR_BOLD + "Mode: " + COLOR_RESET + "Testing connectivity only\n")
	
	# Create a root node for HTTPRequest nodes
	root_node = Node.new()
	root.add_child(root_node)
	
	# Wait for next frame before making requests
	await create_timer(0.01).timeout
	
	# Test each provider sequentially for cleaner output
	await test_openai_endpoint()
	print()  # Blank line between tests
	
	await test_gemini_endpoint()
	print()  # Blank line between tests
	
	await test_xai_endpoint()
	print()  # Blank line before summary
	
	# All tests complete
	print_summary()
	quit(0 if all_online() else 1)

func test_openai_endpoint():
	print(COLOR_BOLD + "Testing OpenAI" + COLOR_RESET + COLOR_GRAY + " (https://api.openai.com/v1/models)" + COLOR_RESET)
	
	# Try to load API key
	var api_key = load_api_key("OPENAI_API_KEY")
	
	var http = HTTPRequest.new()
	root_node.add_child(http)
	
	var headers = PackedStringArray()
	var url = "https://api.openai.com/v1/models"
	
	# If we have a key, test authentication with GET request
	if use_api_keys and not api_key.is_empty():
		headers.append("Authorization: Bearer " + api_key)
		print("  " + COLOR_CYAN + "→" + COLOR_RESET + " API key found, testing authentication (GET /v1/models)...")
	else:
		if use_api_keys:
			print("  " + COLOR_YELLOW + "→" + COLOR_RESET + " No API key found (set OPENAI_API_KEY), testing connectivity only...")
		else:
			print("  " + COLOR_CYAN + "→" + COLOR_RESET + " Testing connectivity only...")
	
	var err = http.request(url, headers, HTTPClient.METHOD_GET)
	if err != OK:
		print("  " + COLOR_RED + "✗ OpenAI: Failed to send request (error %d)" % err + COLOR_RESET)
		test_results["OpenAI"] = {"online": false, "authenticated": false, "error": "Failed to send request"}
		return
	
	# Wait for the request to complete
	var result_data = await http.request_completed
	process_result("OpenAI", result_data[0], result_data[1], result_data[3])

func test_gemini_endpoint():
	print(COLOR_BOLD + "Testing Gemini" + COLOR_RESET + COLOR_GRAY + " (https://generativelanguage.googleapis.com/v1beta/models)" + COLOR_RESET)
	
	# Try to load API key
	var api_key = load_api_key("GEMINI_API_KEY")
	
	var http = HTTPRequest.new()
	root_node.add_child(http)
	
	var headers = PackedStringArray()
	var url = "https://generativelanguage.googleapis.com/v1beta/models"
	
	# If we have a key, add it to URL for GET request
	if use_api_keys and not api_key.is_empty():
		url += "?key=" + api_key
		print("  " + COLOR_CYAN + "→" + COLOR_RESET + " API key found, testing authentication (GET /v1beta/models)...")
	else:
		if use_api_keys:
			print("  " + COLOR_YELLOW + "→" + COLOR_RESET + " No API key found (set GEMINI_API_KEY), testing connectivity only...")
		else:
			print("  " + COLOR_CYAN + "→" + COLOR_RESET + " Testing connectivity only...")
	
	var err = http.request(url, headers, HTTPClient.METHOD_GET)
	if err != OK:
		print("  " + COLOR_RED + "✗ Gemini: Failed to send request (error %d)" % err + COLOR_RESET)
		test_results["Gemini"] = {"online": false, "authenticated": false, "error": "Failed to send request"}
		return
	
	# Wait for the request to complete
	var result_data = await http.request_completed
	process_result("Gemini", result_data[0], result_data[1], result_data[3])

func test_xai_endpoint():
	print(COLOR_BOLD + "Testing x.ai" + COLOR_RESET + COLOR_GRAY + " (https://api.x.ai/v1/models)" + COLOR_RESET)
	
	# Try to load API key
	var api_key = load_api_key("XAI_API_KEY")
	
	var http = HTTPRequest.new()
	root_node.add_child(http)
	
	var headers = PackedStringArray()
	var url = "https://api.x.ai/v1/models"
	
	# If we have a key, test authentication with GET request
	if use_api_keys and not api_key.is_empty():
		headers.append("Authorization: Bearer " + api_key)
		print("  " + COLOR_CYAN + "→" + COLOR_RESET + " API key found, testing authentication (GET /v1/models)...")
	else:
		if use_api_keys:
			print("  " + COLOR_YELLOW + "→" + COLOR_RESET + " No API key found (set XAI_API_KEY), testing connectivity only...")
		else:
			print("  " + COLOR_CYAN + "→" + COLOR_RESET + " Testing connectivity only...")
	
	var err = http.request(url, headers, HTTPClient.METHOD_GET)
	if err != OK:
		print("  " + COLOR_RED + "✗ x.ai: Failed to send request (error %d)" % err + COLOR_RESET)
		test_results["x.ai"] = {"online": false, "authenticated": false, "error": "Failed to send request"}
		return
	
	# Wait for the request to complete
	var result_data = await http.request_completed
	process_result("x.ai", result_data[0], result_data[1], result_data[3])

# Helper function to load API keys (same logic as AIProvider)
func load_api_key(env_var_name: String) -> String:
	# Try environment variable first
	var env_value = OS.get_environment(env_var_name)
	if not env_value.is_empty():
		return env_value
	
	# Try .env file
	var file = FileAccess.open("res://.env", FileAccess.READ)
	if file:
		while not file.eof_reached():
			var line = file.get_line().strip_edges()
			if line.is_empty() or line.begins_with("#"):
				continue
			var parts = line.split("=", true, 1)
			if parts.size() == 2 and parts[0].strip_edges() == env_var_name:
				var value = parts[1].strip_edges()
				# Remove quotes
				if (value.begins_with('"') and value.ends_with('"')) or \
				   (value.begins_with("'") and value.ends_with("'")):
					value = value.substr(1, value.length() - 2)
				return value
	
	return ""

func process_result(provider_name: String, result: int, response_code: int, body: PackedByteArray):
	if result != HTTPRequest.RESULT_SUCCESS:
		# Connection failed - endpoint is offline or unreachable
		var error_names = {
			HTTPRequest.RESULT_CHUNKED_BODY_SIZE_MISMATCH: "Chunked body size mismatch",
			HTTPRequest.RESULT_CANT_CONNECT: "Can't connect",
			HTTPRequest.RESULT_CANT_RESOLVE: "Can't resolve hostname",
			HTTPRequest.RESULT_CONNECTION_ERROR: "Connection error",
			HTTPRequest.RESULT_TLS_HANDSHAKE_ERROR: "TLS handshake error",
			HTTPRequest.RESULT_NO_RESPONSE: "No response",
			HTTPRequest.RESULT_BODY_SIZE_LIMIT_EXCEEDED: "Body size limit exceeded",
			HTTPRequest.RESULT_REQUEST_FAILED: "Request failed",
			HTTPRequest.RESULT_DOWNLOAD_FILE_CANT_OPEN: "Download file can't open",
			HTTPRequest.RESULT_DOWNLOAD_FILE_WRITE_ERROR: "Download file write error",
			HTTPRequest.RESULT_REDIRECT_LIMIT_REACHED: "Redirect limit reached",
			HTTPRequest.RESULT_TIMEOUT: "Timeout"
		}
		
		var error_msg = error_names.get(result, "Unknown error (%d)" % result)
		print("  " + COLOR_RED + "✗ %s: Offline or unreachable" + COLOR_RESET + " (%s)" % [provider_name, error_msg])
		test_results[provider_name] = {"online": false, "authenticated": false, "error": error_msg}
	else:
		# Got a response - endpoint is online!
		var status_msg = get_status_message(response_code)
		var is_authenticated = response_code == 200
		
		if response_code == 200:
			print("  " + COLOR_GREEN + ("✓ %s: Online AND authenticated" % provider_name) + COLOR_RESET + " (HTTP 200 - %s)" % status_msg)
			if use_api_keys:
				print("    " + COLOR_GREEN + "✓ API key is valid!" + COLOR_RESET)
		elif response_code == 401 or response_code == 403:
			print("  " + COLOR_YELLOW + ("✓ %s: Online but NOT authenticated" % provider_name) + COLOR_RESET + (" (HTTP %d - %s)" % [response_code, status_msg]))
			if use_api_keys:
				# Try to parse error message
				var body_str = body.get_string_from_utf8()
				var error_detail = extract_error_message(body_str)
				if not error_detail.is_empty():
					print("    " + COLOR_RED + "✗ API key issue:" + COLOR_RESET + " %s" % error_detail)
				else:
					print("    " + COLOR_RED + "✗ API key missing or invalid" + COLOR_RESET)
		elif response_code == 400:
			print("  " + COLOR_YELLOW + ("✓ %s: Online" % provider_name) + COLOR_RESET + (" (HTTP %d - %s)" % [response_code, status_msg]))
			if use_api_keys:
				# Could be auth'd but bad request format - check error
				var body_str = body.get_string_from_utf8()
				if "authentication" in body_str.to_lower() or "api_key" in body_str.to_lower():
					print("    " + COLOR_RED + "✗ API key issue" + COLOR_RESET + " (check error)")
				else:
					print("    " + COLOR_YELLOW + "? Authentication status unclear" + COLOR_RESET)
		elif response_code == 404:
			print("  " + COLOR_YELLOW + ("⚠ %s: Online but endpoint/model not found" % provider_name) + COLOR_RESET + (" (HTTP %d - %s)" % [response_code, status_msg]))
			var body_str = body.get_string_from_utf8()
			var error_detail = extract_error_message(body_str)
			if not error_detail.is_empty():
				print("    " + COLOR_BLUE + "ℹ Error:" + COLOR_RESET + " %s" % error_detail)
			else:
				print("    " + COLOR_BLUE + "ℹ Check if model name is correct" + COLOR_RESET + " (currently using: %s)" % get_model_for_provider(provider_name))
		elif response_code >= 500:
			print("  " + COLOR_YELLOW + ("⚠ %s: Server error" % provider_name) + COLOR_RESET + (" (HTTP %d - %s)" % [response_code, status_msg]))
		else:
			print("  " + COLOR_GREEN + ("✓ %s: Online" % provider_name) + COLOR_RESET + (" (HTTP %d - %s)" % [response_code, status_msg]))
		
		test_results[provider_name] = {
			"online": true, 
			"authenticated": is_authenticated,
			"code": response_code, 
			"status": status_msg
		}

func extract_error_message(body_str: String) -> String:
	# Try to parse JSON error messages
	var json = JSON.new()
	var err = json.parse(body_str)
	if err == OK:
		var data = json.data
		if typeof(data) == TYPE_DICTIONARY:
			if data.has("error"):
				var error = data["error"]
				if typeof(error) == TYPE_DICTIONARY:
					if error.has("message"):
						return error["message"]
					elif error.has("type"):
						return error["type"]
				elif typeof(error) == TYPE_STRING:
					return error
	return ""

func get_model_for_provider(provider_name: String) -> String:
	match provider_name:
		"OpenAI":
			return "gpt-3.5-turbo"
		"Gemini":
			return "gemini-pro"
		"x.ai":
			return "grok-4-fast"
		_:
			return "unknown"

func get_status_message(code: int) -> String:
	var messages = {
		200: "OK",
		400: "Bad Request",
		401: "Unauthorized",
		403: "Forbidden",
		404: "Not Found",
		429: "Too Many Requests",
		500: "Internal Server Error",
		502: "Bad Gateway",
		503: "Service Unavailable",
		504: "Gateway Timeout"
	}
	return messages.get(code, "Status %d" % code)

func all_online() -> bool:
	for provider in test_results:
		if not test_results[provider].get("online", false):
			return false
	return true

func print_summary():
	print("\n" + COLOR_BOLD + COLOR_CYAN + "=".repeat(60))
	print("Summary")
	print("=".repeat(60) + COLOR_RESET + "\n")
	
	var online_count = 0
	var authenticated_count = 0
	
	for provider in test_results:
		if test_results[provider].get("online", false):
			online_count += 1
		if test_results[provider].get("authenticated", false):
			authenticated_count += 1
	
	print(COLOR_BOLD + "Endpoints online:" + COLOR_RESET + " %d/%d" % [online_count, total_tests])
	
	if use_api_keys:
		print(COLOR_BOLD + "Authenticated:" + COLOR_RESET + " %d/%d" % [authenticated_count, total_tests])
	
	if online_count == total_tests:
		print(COLOR_GREEN + "✓ All provider endpoints are reachable!" + COLOR_RESET)
		if use_api_keys:
			if authenticated_count == total_tests:
				print(COLOR_GREEN + "✓ All API keys are valid and working!" + COLOR_RESET)
			elif authenticated_count > 0:
				print(COLOR_YELLOW + "⚠ Some API keys are missing or invalid" + COLOR_RESET)
			else:
				print(COLOR_RED + "✗ No valid API keys found" + COLOR_RESET)
	elif online_count > 0:
		print(COLOR_YELLOW + "⚠ Some endpoints are offline or unreachable" + COLOR_RESET)
	else:
		print(COLOR_RED + "✗ No endpoints are reachable - check internet connection" + COLOR_RESET)
	
	print("\n" + COLOR_BOLD + COLOR_CYAN + "=".repeat(60) + COLOR_RESET)
	
	if use_api_keys:
		print("\nNote: This test checks both connectivity and API key validity.")
		print("Uses GET /v1/models endpoints - no credits consumed!")
		print("To add API keys:")
		print("  1. Set environment variables (OPENAI_API_KEY, GEMINI_API_KEY, XAI_API_KEY)")
		print("  2. Or create a .env file in your project root")
		print("  3. See modules/ai/API_KEYS.md for details")
	else:
		print("\nNote: This test only checks endpoint reachability.")
		print("Set use_api_keys = true to also validate API keys.")
	print()

