## AI Module - Multi-Provider Support

The AI module now supports multiple AI providers including OpenAI, Google Gemini, and x.ai (Grok). The default provider has been set to **XAIProvider** using the `grok-4-fast` model. API keys are automatically loaded from system environment variables or a `.env` file located in the engine root or bin directory. Each provider implements asynchronous HTTP requests using Godot's `HTTPRequest` node, with proper error handling and signal-based callbacks. The system has been successfully tested with live API calls to Grok, demonstrating the ability to generate and execute actions (like node creation) based on natural language prompts. Provider classes are now registered with Godot's ClassDB, making them accessible from GDScript for easy runtime configuration.


