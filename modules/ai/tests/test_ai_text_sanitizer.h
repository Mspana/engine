// modules/ai/tests/test_ai_text_sanitizer.h

#pragma once

#include "../ai_text_sanitizer.h"

#include "tests/test_macros.h"

namespace TestAITextSanitizer {

TEST_CASE("[AITextSanitizer] Clean text passes through without allocation") {
	String clean = String(U"plain text\nwith\tlines\r\nand unicode Tēšt 🙂");
	String result = ai_sanitize_model_text(clean);
	CHECK(result == clean);
	// Fast path must return the same COW buffer, not a copy.
	CHECK(result.ptr() == clean.ptr());
}

TEST_CASE("[AITextSanitizer] NUL is dropped, other controls become U+FFFD") {
	String s = "a";
	s += char32_t(0); // NUL — droppable noise.
	s += "b";
	s += char32_t(0x01); // C0 control.
	s += "c";
	s += char32_t(0x7F); // DEL.
	s += char32_t(0x9B); // C1 control.
	s += "d";

	String expected = "ab";
	expected += char32_t(0xFFFD);
	expected += "c";
	expected += char32_t(0xFFFD);
	expected += char32_t(0xFFFD);
	expected += "d";

	CHECK(ai_sanitize_model_text(s) == expected);
}

TEST_CASE("[AITextSanitizer] Whitespace controls are preserved") {
	String s = "line1\nline2\r\n\tindented";
	CHECK(ai_sanitize_model_text(s) == s);
	// But vertical tab and form feed are not on the allowlist.
	String vt = "a\vb\fc";
	String expected = "a";
	expected += char32_t(0xFFFD);
	expected += "b";
	expected += char32_t(0xFFFD);
	expected += "c";
	CHECK(ai_sanitize_model_text(vt) == expected);
}

TEST_CASE("[AITextSanitizer] Variant walker sanitizes nested containers") {
	String dirty = "x";
	dirty += char32_t(0);
	dirty += "y";

	Array inner;
	inner.push_back(dirty);
	inner.push_back(42);
	Dictionary nested;
	nested["text"] = dirty;
	nested["arr"] = inner;
	Dictionary root;
	root["nested"] = nested;
	root["num"] = 3.5;
	root["flag"] = true;

	Dictionary out = ai_sanitize_model_variant(root);
	Dictionary out_nested = out["nested"];
	CHECK(String(out_nested["text"]) == "xy");
	Array out_arr = out_nested["arr"];
	CHECK(String(out_arr[0]) == "xy");
	CHECK(int(out_arr[1]) == 42);
	CHECK(double(out["num"]) == 3.5);
	CHECK(bool(out["flag"]));
	// Input containers must not be mutated.
	CHECK(String(nested["text"]) == dirty);
}

TEST_CASE("[AITextSanitizer] Non-string variants pass through") {
	CHECK(ai_sanitize_model_variant(Variant()) == Variant());
	CHECK(int(ai_sanitize_model_variant(7)) == 7);
	PackedByteArray bytes;
	bytes.push_back(0);
	bytes.push_back(255);
	PackedByteArray out = ai_sanitize_model_variant(bytes);
	CHECK(out == bytes);
}

} // namespace TestAITextSanitizer
