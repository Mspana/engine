// modules/ai/ai_text_sanitizer.h
// Control-character scrubbing for model-bound text.

#ifndef AI_TEXT_SANITIZER_H
#define AI_TEXT_SANITIZER_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

// Scrubs control characters that poison model-bound payloads (strict provider
// JSON decoders, codex rollout files, JSONL transcript lines). Keeps \n, \r
// and \t; NUL is dropped; every other is_control() char (C0, DEL, C1) becomes
// U+FFFD. Returns the input string unchanged (no allocation) when clean.
String ai_sanitize_model_text(const String &p_text);

// Deep copy-walk over Dictionary/Array values; sanitizes every String value.
// Keys are left alone — they are engine/codex schema literals. Non-container,
// non-string variants pass through untouched.
Variant ai_sanitize_model_variant(const Variant &p_value);

#endif // AI_TEXT_SANITIZER_H
