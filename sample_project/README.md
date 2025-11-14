# AI Helper Sample Project

This is a minimal Godot 4.4 sample project for testing the AI Helper module functionality.

## Features

- **AI Helper Plugin**: Provides an editor dock to interact with the AI module
  - Located in the top-right dock area
  - Enter natural language prompts
  - AI can modify scripts, create nodes, and more
- **Platformer Player**: Simple CharacterBody2D with movement and jumping
- **Headless Testing**: Automated smoke tests for CI/CD

## Project Structure

```
sample_project/
├── project.godot              # Main project configuration
├── scenes/
│   └── main.tscn             # Main scene with Player node
├── scripts/
│   └── PlayerController.gd   # Player movement script (platformer style)
├── sprites/
│   └── player.png            # Player sprite (32x32 placeholder)
└── addons/
    └── ai_helper/
        ├── plugin.cfg        # AI Helper plugin config
        ├── plugin.gd         # AI Helper editor UI
        └── headless_smoke_test.gd  # Headless smoke test script
```

## Headless Smoke Test

The project includes a headless smoke test that validates the project can:
- Load the main scene
- Find the Player node
- Simulate input actions
- Advance the game loop
- Log position changes

### Running the Smoke Test

From the engine root directory, run:

```bash
bin/godot.windows.editor.x86_64.exe --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
```

Or on Linux/macOS:

```bash
bin/godot.linuxbsd.editor.x86_64 --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
# or
bin/godot.macos.editor.universal --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
```

### Expected Output

The test should produce output similar to:

```
============================================================
SMOKE: Starting headless smoke test
============================================================
SMOKE: Loading main scene: res://scenes/main.tscn
SMOKE: Main scene loaded successfully
SMOKE: Player found
SMOKE: Before input, Player position: (0, 0)
SMOKE: Simulating ui_right input for 30 frames

------------------------------------------------------------
SMOKE: After input, Player position: (300, 0)
SMOKE: Player moved 300 units
SMOKE: Test completed successfully ✓
SMOKE: Done.
============================================================
```

### Test Details

The smoke test:
1. Loads the main scene from project settings
2. Finds the Player node (CharacterBody2D)
3. Records the initial position
4. Simulates pressing `ui_right` for 30 frames (frames 10-40)
5. Continues running for a total of 60 frames (~1 second at 60fps)
6. Logs the final position
7. Exits with code 0 on success

The Player script responds to input by moving at 300 pixels/second, so after ~0.5 seconds of input, the player should have moved approximately 150-300 pixels to the right.

### Exit Codes

- `0` - Test completed successfully
- `1` - Test failed (scene load error, etc.)

## Playing the Project

You can also open this project in the Godot editor:

```bash
bin/godot.windows.editor.x86_64.exe --editor --path sample_project
```

Use arrow keys to move the Player node around.

## License

This sample project is provided as-is for testing purposes.

