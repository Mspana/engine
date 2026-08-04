// modules/ai/tests/test_responses_translator.h

#pragma once

#include "../harness/responses_translator.h"

#include "tests/test_macros.h"

namespace TestResponsesTranslator {

// --- Responses-API input item builders (the shapes codex replays). ---

static Dictionary _msg_item(const String &p_role, const String &p_text) {
	Dictionary item;
	item["type"] = "message";
	item["role"] = p_role;
	Array content;
	Dictionary part;
	part["type"] = p_role == "assistant" ? "output_text" : "input_text";
	part["text"] = p_text;
	content.push_back(part);
	item["content"] = content;
	return item;
}

static Dictionary _fc_item(const String &p_call_id, const String &p_name, const String &p_args) {
	Dictionary item;
	item["type"] = "function_call";
	item["call_id"] = p_call_id;
	item["name"] = p_name;
	item["arguments"] = p_args;
	return item;
}

static Dictionary _fco_item(const String &p_call_id, const Variant &p_output) {
	Dictionary item;
	item["type"] = "function_call_output";
	item["call_id"] = p_call_id;
	item["output"] = p_output;
	return item;
}

static Dictionary _reasoning_item(const String &p_content_text, const String &p_summary_text) {
	Dictionary item;
	item["type"] = "reasoning";
	Array content;
	if (!p_content_text.is_empty()) {
		Dictionary part;
		part["type"] = "reasoning_text";
		part["text"] = p_content_text;
		content.push_back(part);
	}
	item["content"] = content;
	Array summary;
	if (!p_summary_text.is_empty()) {
		Dictionary part;
		part["type"] = "summary_text";
		part["text"] = p_summary_text;
		summary.push_back(part);
	}
	item["summary"] = summary;
	return item;
}

static Array _translate(const Array &p_input) {
	Dictionary req;
	req["model"] = "kimi-k3";
	req["input"] = p_input;
	String err;
	Dictionary chat = AIResponsesTranslator::translate_request(req, err);
	CHECK(err.is_empty());
	return chat.get("messages", Array());
}

TEST_CASE("[AIResponsesTranslator] Parallel calls and text merge into one assistant message") {
	Array input;
	input.push_back(_msg_item("user", "do the thing"));
	input.push_back(_fc_item("call_1", "list_files", "{\"directory\":\"res://\"}"));
	input.push_back(_fc_item("call_2", "list_open_scenes", "{}"));
	input.push_back(_msg_item("assistant", "Looking at the project."));
	input.push_back(_fco_item("call_1", "files"));
	input.push_back(_fco_item("call_2", "scenes"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 4);
	Dictionary assistant = messages[1];
	CHECK(String(assistant["role"]) == "assistant");
	CHECK(String(assistant["content"]) == "Looking at the project.");
	Array calls = assistant["tool_calls"];
	REQUIRE(calls.size() == 2);
	CHECK(String(Dictionary(calls[0])["id"]) == "call_1");
	CHECK(String(Dictionary(calls[1])["id"]) == "call_2");
	// Adjacency repair pins both results directly after the merged message.
	CHECK(String(Dictionary(messages[2])["tool_call_id"]) == "call_1");
	CHECK(String(Dictionary(messages[3])["tool_call_id"]) == "call_2");
}

TEST_CASE("[AIResponsesTranslator] Merge is item-order agnostic") {
	Array text_first;
	text_first.push_back(_msg_item("user", "go"));
	text_first.push_back(_msg_item("assistant", "On it."));
	text_first.push_back(_fc_item("call_1", "read_script", "{}"));
	text_first.push_back(_fco_item("call_1", "ok"));

	Array calls_first;
	calls_first.push_back(_msg_item("user", "go"));
	calls_first.push_back(_fc_item("call_1", "read_script", "{}"));
	calls_first.push_back(_msg_item("assistant", "On it."));
	calls_first.push_back(_fco_item("call_1", "ok"));

	Array a = _translate(text_first);
	Array b = _translate(calls_first);
	REQUIRE(a.size() == 3);
	REQUIRE(b.size() == 3);
	Dictionary asst_a = a[1];
	Dictionary asst_b = b[1];
	CHECK(String(asst_a["content"]) == String(asst_b["content"]));
	CHECK(Array(asst_a["tool_calls"]).size() == 1);
	CHECK(Array(asst_b["tool_calls"]).size() == 1);
}

TEST_CASE("[AIResponsesTranslator] function_call_output is a flush boundary") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_fc_item("call_1", "read_script", "{}"));
	input.push_back(_msg_item("assistant", "first round"));
	input.push_back(_fco_item("call_1", "ok"));
	input.push_back(_msg_item("assistant", "second round"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 4);
	Dictionary first = messages[1];
	Dictionary second = messages[3];
	CHECK(String(first["content"]) == "first round");
	CHECK(first.has("tool_calls"));
	CHECK(String(second["content"]) == "second round");
	CHECK_FALSE(second.has("tool_calls"));
}

TEST_CASE("[AIResponsesTranslator] Steering user message flushes and moves the reasoning boundary") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_reasoning_item("old thoughts", ""));
	input.push_back(_fc_item("call_1", "read_script", "{}"));
	input.push_back(_msg_item("assistant", "round one"));
	input.push_back(_fco_item("call_1", "ok"));
	input.push_back(_msg_item("user", "actually, do this instead"));
	input.push_back(_reasoning_item("new thoughts", ""));
	input.push_back(_msg_item("assistant", "round two"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 5);
	Dictionary first = messages[1];
	Dictionary steer = messages[3];
	Dictionary second = messages[4];
	// Reasoning before the last user message is dropped; after it, folded.
	CHECK_FALSE(first.has("reasoning_content"));
	CHECK(String(steer["role"]) == "user");
	CHECK(String(second["reasoning_content"]) == "new thoughts");
}

TEST_CASE("[AIResponsesTranslator] Current-turn reasoning folds into reasoning_content") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_reasoning_item("I should read the script first.", ""));
	input.push_back(_fc_item("call_1", "read_script", "{}"));
	input.push_back(_msg_item("assistant", "Reading."));
	input.push_back(_fco_item("call_1", "ok"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 3);
	Dictionary assistant = messages[1];
	CHECK(String(assistant["reasoning_content"]) == "I should read the script first.");
	CHECK(String(assistant["content"]) == "Reading.");
	CHECK(Array(assistant["tool_calls"]).size() == 1);
}

TEST_CASE("[AIResponsesTranslator] Prior-turn reasoning is dropped") {
	Array input;
	input.push_back(_reasoning_item("stale thoughts", ""));
	input.push_back(_msg_item("assistant", "previous turn answer"));
	input.push_back(_msg_item("user", "next question"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 2);
	Dictionary assistant = messages[0];
	CHECK(String(assistant["content"]) == "previous turn answer");
	CHECK_FALSE(assistant.has("reasoning_content"));
}

TEST_CASE("[AIResponsesTranslator] Reasoning extraction prefers content over summary") {
	Array both;
	both.push_back(_msg_item("user", "go"));
	both.push_back(_reasoning_item("full text", "summary text"));
	both.push_back(_msg_item("assistant", "x"));
	Dictionary assistant = _translate(both)[1];
	CHECK(String(assistant["reasoning_content"]) == "full text");

	Array summary_only;
	summary_only.push_back(_msg_item("user", "go"));
	summary_only.push_back(_reasoning_item("", "summary text"));
	summary_only.push_back(_msg_item("assistant", "x"));
	assistant = _translate(summary_only)[1];
	CHECK(String(assistant["reasoning_content"]) == "summary text");

	Array empty;
	empty.push_back(_msg_item("user", "go"));
	empty.push_back(_reasoning_item("", ""));
	empty.push_back(_msg_item("assistant", "x"));
	assistant = _translate(empty)[1];
	CHECK_FALSE(assistant.has("reasoning_content"));
}

TEST_CASE("[AIResponsesTranslator] Reasoning-only round is stripped") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_reasoning_item("thoughts with no output", ""));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 1);
	CHECK(String(Dictionary(messages[0])["role"]) == "user");
}

TEST_CASE("[AIResponsesTranslator] Consecutive assistant text items concatenate with separator") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_msg_item("assistant", "part one"));
	input.push_back(_msg_item("assistant", "part two"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 2);
	CHECK(String(Dictionary(messages[1])["content"]) == "part one\n\npart two");
}

TEST_CASE("[AIResponsesTranslator] Adjacency repair synthesizes placeholders inside merged messages") {
	Array input;
	input.push_back(_msg_item("user", "go"));
	input.push_back(_fc_item("call_1", "read_script", "{}"));
	input.push_back(_fc_item("call_2", "list_files", "{}"));
	input.push_back(_msg_item("assistant", "both"));
	input.push_back(_fco_item("call_2", "only second result"));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 4);
	Dictionary missing = messages[2];
	Dictionary present = messages[3];
	CHECK(String(missing["tool_call_id"]) == "call_1");
	CHECK(String(missing["content"]) == "(no result recorded)");
	CHECK(String(present["tool_call_id"]) == "call_2");
	CHECK(String(present["content"]) == "only second result");
}

TEST_CASE("[AIResponsesTranslator] Call-id sanitization and vision hoist survive the merge") {
	Dictionary image_part;
	image_part["type"] = "input_image";
	image_part["image_url"] = "data:image/png;base64,AAAA";
	Array output_parts;
	output_parts.push_back(image_part);

	Array input;
	input.push_back(_msg_item("user", "screenshot please"));
	input.push_back(_fc_item("functions:shot:1", "run_and_screenshot", "{}"));
	input.push_back(_msg_item("assistant", "Capturing."));
	input.push_back(_fco_item("functions:shot:1", output_parts));

	Array messages = _translate(input);
	REQUIRE(messages.size() == 4);
	Dictionary assistant = messages[1];
	Array calls = assistant["tool_calls"];
	CHECK(String(Dictionary(calls[0])["id"]) == "functions_shot_1");
	Dictionary tool = messages[2];
	CHECK(String(tool["role"]) == "tool");
	CHECK(String(tool["tool_call_id"]) == "functions_shot_1");
	// Hoisted image message follows the tool result as a user message.
	Dictionary hoisted = messages[3];
	CHECK(String(hoisted["role"]) == "user");
	Array parts = hoisted["content"];
	REQUIRE(parts.size() == 2);
	CHECK(String(Dictionary(parts[1])["type"]) == "image_url");
}

TEST_CASE("[AIResponsesTranslator] Reasoning done item matches codex schema") {
	Dictionary item = AIResponsesTranslator::make_reasoning_done_item("rs_x", "the thought");
	CHECK(String(item["type"]) == "reasoning");
	CHECK(String(item["id"]) == "rs_x");
	Array summary = item["summary"];
	REQUIRE(summary.size() == 1);
	CHECK(String(Dictionary(summary[0])["type"]) == "summary_text");
	CHECK(String(Dictionary(summary[0])["text"]) == "the thought");
	Array content = item["content"];
	REQUIRE(content.size() == 1);
	CHECK(String(Dictionary(content[0])["type"]) == "reasoning_text");
	CHECK(String(Dictionary(content[0])["text"]) == "the thought");
	CHECK_FALSE(item.has("status"));
}

} // namespace TestResponsesTranslator
