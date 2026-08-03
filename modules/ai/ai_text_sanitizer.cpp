// modules/ai/ai_text_sanitizer.cpp

#include "ai_text_sanitizer.h"

#include "core/variant/array.h"
#include "core/variant/dictionary.h"

static bool _is_disallowed_control(char32_t p_c) {
	return is_control(p_c) && p_c != '\n' && p_c != '\r' && p_c != '\t';
}

String ai_sanitize_model_text(const String &p_text) {
	const int len = p_text.length();
	int first_bad = -1;
	for (int i = 0; i < len; i++) {
		if (_is_disallowed_control(p_text[i])) {
			first_bad = i;
			break;
		}
	}
	if (first_bad < 0) {
		// Clean input: hand back the same COW string, no allocation. Payloads
		// routinely carry multi-MB base64 strings.
		return p_text;
	}
	String out = p_text.substr(0, first_bad);
	for (int i = first_bad; i < len; i++) {
		const char32_t c = p_text[i];
		if (!_is_disallowed_control(c)) {
			out += c;
		} else if (c != 0) {
			// NUL carries no information and is dropped; other controls leave
			// positional evidence, matching how invalid UTF-8 bytes render.
			out += char32_t(0xFFFD);
		}
	}
	return out;
}

Variant ai_sanitize_model_variant(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::STRING:
			return ai_sanitize_model_text(p_value);
		case Variant::DICTIONARY: {
			const Dictionary in = p_value;
			Dictionary out;
			for (const Variant &key : in.get_key_list()) {
				out[key] = ai_sanitize_model_variant(in[key]);
			}
			return out;
		}
		case Variant::ARRAY: {
			const Array in = p_value;
			Array out;
			out.resize(in.size());
			for (int i = 0; i < in.size(); i++) {
				out[i] = ai_sanitize_model_variant(in[i]);
			}
			return out;
		}
		default:
			return p_value;
	}
}
