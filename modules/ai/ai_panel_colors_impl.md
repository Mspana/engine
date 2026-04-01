# AI Panel Colors & Debug I/O — Implementation Notes

## What was built

Two enhancements to the AI chat panel:

1. **Tool result color coding** — `ToolCollapsibleEntry` border is tinted green on success and red on failure, giving immediate visual feedback without opening the collapsible.
2. **Debug I/O popup** — "I/O" button in the chat toolbar (and Ctrl+Shift+D from the input box) opens a read-only `TextEdit` showing the full chat transcript as pretty-printed JSON.

---

## Tool result color coding

### Where

`ToolCollapsibleEntry::update_from_tool_result()` in `ai_status_indicator.cpp`.

### How

Previously, `panel_style` was a local variable in the constructor discarded after setup. It's now a `Ref<StyleBoxFlat>` member so `update_from_tool_result()` can reach it later.

When the result arrives:
- Success (`status != "error"`): border alpha-tinted green — `Color(SUCCESS.r, SUCCESS.g, SUCCESS.b, 0.5f)`
- Error (`status == "error"`): border alpha-tinted red — `Color(ERROR.r, ERROR.g, ERROR.b, 0.6f)`
- The header status label is also set to the same color family for redundancy.

### Gaps

- No "warning" tier. Any non-error/non-success status (e.g. "partial") is treated as success.
- Color is set once when the result arrives and never cleared. If the same entry is reused (unlikely given current architecture), the old color persists.

---

## Debug I/O popup

### Where

`AIStatusPanel::_show_debug_io_popup()` in `ai_status_indicator.cpp`.

### How

1. `chat_store->get_messages()` returns the current in-memory transcript as `Vector<ChatMessage>`.
2. Each message is serialized to a `Dictionary` (`id`, `role`, `content`, `created_at`, optional `images`).
3. Base64 image blobs are truncated to 80 chars + `[N chars]` annotation so the popup stays readable.
4. `JSON::stringify(arr, "\t")` produces indented JSON.
5. The result is set into a non-editable `TextEdit` inside a `PopupPanel`.
6. The popup is sized to fill most of the AI panel area (`panel width - 20`, `panel height - 60`).

### Trigger

- "I/O" button in chat toolbar (flat button, right of "New Chat" and "History").
- `Ctrl+Shift+D` while the prompt `TextEdit` is focused (intercepted in `_on_prompt_gui_input`).

### Gaps

1. **Read-only, no search**: The TextEdit is non-editable. There's no Ctrl+F search. Copying text still works via Ctrl+C after selecting.
2. **Snapshot on open**: The JSON reflects the transcript at the moment the popup is opened, not live-updated.
3. **Images truncated**: Only the first 80 characters of each base64 blob are shown. Full image data is not displayed. This is intentional to keep the popup usable.
4. **No syntax highlighting**: Plain text. A future improvement could use a `CodeEdit` with JSON highlighting.
5. **Raw transcript only**: Shows the `chat_store` messages, not the model-API payload (which goes through `_build_model_messages()` with truncation and role remapping). To see the actual API payload, the orchestrator would need to emit it as a signal.
