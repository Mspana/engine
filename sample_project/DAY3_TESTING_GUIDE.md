# Day 3 AI Testing Guide

This guide explains how to test the AI's ability to understand and modify the PlayerController script.

## Project Overview

This minimal Godot 4 project contains:
- **Main Scene**: `res://scenes/main.tscn` with a Player CharacterBody2D
- **Player Script**: `res://scripts/PlayerController.gd` with platformer movement
- **Placeholder Sprite**: `res://sprites/player.png` (32x32 magenta square)
- **AI Helper Plugin**: `res://addons/ai_helper/plugin.gd` provides the AI tab in the editor

## Manual Testing

### 1. Open the Project in Godot

```bash
# From the engine root directory:
bin\godot.windows.editor.x86_64.exe --editor --path sample_project
```

You should see the **AI Helper** tab in the top-right dock area with:
- Text input field: "Enter prompt for AI..."
- "Request Actions" button

### 2. Play the Scene

- Press **F5** to run the project (or click the Play button)
- Use **Arrow Keys (Left/Right)** to move horizontally
- Press **Space** to jump
- The player should move at 200 pixels/second

### 3. Verify the Script

Open `scripts/PlayerController.gd` and verify:
```gdscript
const SPEED = 200
const JUMP_VELOCITY = -350
```

## AI Testing Protocol

### Using the AI Helper Tab

1. Open the project in the Godot editor
2. Look for the **AI** tab in the top-right dock area
3. Type your prompt in the text field
4. Click **"Request Actions"**
5. Watch the Output console for AI responses and actions

### Test 1: Basic Speed Modification

**In the AI Helper tab, enter:**
> "Make the player run twice as fast."

**Expected AI Actions:**
1. Locate `scripts/PlayerController.gd`
2. Find `const SPEED = 200` on line 3
3. Change it to `const SPEED = 400`

**Verification:**
- Open `scripts/PlayerController.gd`
- Confirm `const SPEED = 400`
- Run the project and test - player should move faster

### Test 2: Jump Height Modification

**Prompt the AI with:**
> "Make the player jump higher."

**Expected AI Actions:**
1. Locate `scripts/PlayerController.gd`
2. Find `const JUMP_VELOCITY = -350` on line 4
3. Change it to a larger negative value (e.g., `-500` or `-450`)

**Verification:**
- Confirm the JUMP_VELOCITY constant was modified
- Run the project - player should jump higher

### Test 3: Combined Modification

**Prompt the AI with:**
> "Make the player move at 150 speed and jump with velocity -400."

**Expected AI Actions:**
1. Change `SPEED` to `150`
2. Change `JUMP_VELOCITY` to `-400`

### Test 4: Code Understanding

**Prompt the AI with:**
> "What does this player script do?"

**Expected AI Response:**
- Identifies it as a platformer controller
- Mentions horizontal movement (left/right)
- Mentions jumping with ground detection
- Mentions gravity application
- Notes the speed constants

## Success Criteria

✅ **PASS** if the AI:
- Correctly identifies the file location
- Finds and modifies the right constants
- Uses proper GDScript syntax
- Explains changes clearly

❌ **FAIL** if the AI:
- Cannot locate the script
- Modifies wrong values
- Introduces syntax errors
- Creates new files instead of editing existing ones

## Running Headless Tests

The project also includes a headless smoke test:

```bash
# From engine root:
bin\godot.windows.editor.x86_64.exe --headless --path sample_project --script res://addons/ai_helper/headless_smoke_test.gd
```

**Note:** The smoke test expects the old movement behavior (4-directional). You may need to update it for platformer physics.

## Project Structure

```
sample_project/
├── project.godot                      # Project configuration (plugin enabled)
├── scenes/
│   └── main.tscn                     # Main → Player (CharacterBody2D)
├── scripts/
│   └── PlayerController.gd           # ⭐ TARGET FOR AI TESTING
├── sprites/
│   └── player.png                    # Placeholder sprite
└── addons/
    └── ai_helper/
        ├── plugin.cfg                # AI Helper plugin config
        ├── plugin.gd                 # AI Helper editor UI
        └── headless_smoke_test.gd    # Automated test script
```

## Tips for AI Testing

1. **Clear Instructions**: Give specific, natural language instructions
2. **One Change at a Time**: Test individual modifications first
3. **Verify After Each Change**: Check the file contents after AI edits
4. **Test in Godot**: Run the project to verify behavior matches expectations
5. **Document Results**: Note which prompts work well and which don't

## Common Issues

**Issue**: AI creates a new file instead of editing existing one
- **Solution**: Explicitly mention "edit the existing PlayerController.gd"

**Issue**: AI doesn't know where the script is
- **Solution**: Provide context: "in the scripts folder" or "res://scripts/PlayerController.gd"

**Issue**: AI changes wrong values
- **Solution**: Be more specific: "change the SPEED constant" instead of "make it faster"

## Next Steps

After successful AI testing:
1. Document which prompts worked best
2. Note any edge cases or failures
3. Consider adding more complex scenarios
4. Update the headless smoke test if needed

