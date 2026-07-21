# Scene Targeting for Mutating Tools

## The Problem

Node-mutating tools (`create_node`, `set_property`, `delete_node`, etc.) always
operate on the **active scene tab**. Read tools, however, accept a `scene_path`
and can inspect any open scene without switching tabs. This asymmetry created a
trap: a model would `read_scene_file` / `list_nodes(scene_path=...)` on scene B
while scene A was focused, then call `create_node` — and the node landed in
scene A. The model had every reason to believe it was working on scene B.

This happened in practice: an agent asked to add a background to `scene6outside.tscn`
created the node in `EyesAnimation.tscn` (the focused tab) and only caught it via
the scene diff.

## The Fix

All mutating tools now accept the same optional `scene_path` the read tools take:

- **Omitted** — behaves as before: the active scene.
- **Names the active scene** — no-op, proceeds.
- **Names another open scene tab** — the editor switches to that tab first, then
  applies the change. The result reports `switched_scene_tab: true`.
- **Names a scene that is not open** — the tool errors and lists the open scenes,
  directing the model to call `open_scene` first (consistent with the read tools).

Mutations deliberately *switch* the tab rather than editing a background scene:
the editor's undo system routes a node's history through the currently edited
scene, so mutating a non-focused tab would push the action into the global undo
history and skip that tab's unsaved-changes marking. Focusing first keeps undo,
dirty-marking, and on-screen state all correct — and the visible tab switch shows
the user which scene the AI is working on.

## Self-Evident Results

Every mutation result now includes `scene_path` — the scene that was actually
modified — so a wrong-scene operation is visible in the very next tool result
instead of surfacing later as a mystery. Node paths returned by `create_node`,
`duplicate_node`, and friends are scene-root-relative (the same form the tools
accept as input), not absolute editor-tree paths.

## Tools Covered

`create_node`, `delete_node`, `duplicate_node`, `set_property`, `create_resource`,
`rename_node`, `reparent_node`, `attach_script`, `detach_script`,
`connect_signal`, `disconnect_signal`.

## Key Files

- `modules/ai/actions/action_common.h` — `ai_focus_scene_for_mutation()` helper
- `modules/ai/actions/node_actions.cpp`, `script_actions.cpp`, `signal_actions.cpp` — call sites
- `modules/ai/tools_array.inc` — shared `scene_path` parameter description
- `modules/ai/system_prompt.inc` — guidance telling the model about the active-tab rule
