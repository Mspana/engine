# modules/ai/config.py

def can_build(env, platform):
    # Can build on all platforms by default
    return True

def configure(env):
    # No specific configuration needed for this simple module
    pass

def get_doc_classes():
    # No documentation classes to register
    return []

def get_doc_path():
    # No documentation path
    return ""

# Optional: Check if the module should be enabled by default
# def is_enabled():
#     # Enabled by default, command line 'module_ai_enabled=no' overrides
#	  return True 