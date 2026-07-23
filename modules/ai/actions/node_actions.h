// modules/ai/actions/node_actions.h
// Node-related action implementations for the AI module

#ifndef AI_NODE_ACTIONS_H
#define AI_NODE_ACTIONS_H

#include "core/variant/dictionary.h"

namespace AINodeActions {

// Creates a new node in the scene tree.
// Required args: node_name (String), node_type (String)
// Optional args: parent_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_node(const Dictionary &args);

// Sets a property on an existing node.
// Required args: node_path (String), property_name (String), value (Variant)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_set_property(const Dictionary &args);

// Renames an existing node.
// Required args: node_path (String), new_name (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_rename_node(const Dictionary &args);

// Reparents a node to a new parent.
// Required args: node_path (String), new_parent_path (String)
// Optional args: index (int)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_reparent_node(const Dictionary &args);

// Deletes a node from the scene tree.
// Required args: node_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_delete_node(const Dictionary &args);

// Duplicates a node in the scene tree.
// Required args: node_path (String)
// Optional args: new_name (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_duplicate_node(const Dictionary &args);

// Instantiates a Resource subclass and assigns it to a node property.
// Required args: node_path (String), property_name (String), resource_type (String)
// Optional args: properties (Dictionary) — initial property values applied before assignment
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_resource(const Dictionary &args);

// Builds a fully-populated SpriteFrames resource (named animations, per-frame
// textures/durations, fps, loop) and assigns it to a node's sprite_frames
// property and/or saves it as a standalone .tres.
// Required args: animations (Array of {name, fps?, loop?, frames: [{texture, duration?, region?}]})
// Optional args: node_path (String; required unless save_path given), save_path (String), scene_path (String)
// Returns: Dictionary with status="success"|"error", result/error fields
Dictionary exec_create_sprite_frames(const Dictionary &args);

} // namespace AINodeActions

#endif // AI_NODE_ACTIONS_H

