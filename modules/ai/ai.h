#ifndef AI_H
#define AI_H

#include "core/object/object.h"         // Base class
#include "core/variant/array.h"         // For Array return type
#include "core/string/ustring.h"        // For String parameter type

class AI : public Object {
	GDCLASS(AI, Object); // Godot class macro

	static AI *singleton; // Pointer to the singleton instance

protected:
	// Binds methods to be used in scripting.
	static void _bind_methods();

public:
	// The core method to interact with the AI backend.
	Array request_actions(const String &prompt);

	// Static methods for singleton management (called from register_types.cpp)
	static void initialize_singleton();
	static void finalize_singleton();
	// Static getter for easy C++ access to the singleton.
	static AI *get_singleton();

	// Constructor and Destructor
	AI();
	~AI();
};

#endif // AI_H 