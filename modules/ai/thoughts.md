# AI Module — Backlog

Enhanced AI Panel UI
1. Input/output, colorized. Same color on success, red output on failure
2. Debug Button + shortcut to see full input/output (current approach)

Support changing AI panel colors w/ theme

Consistent Aristotle branding

Better renaming in file explorer
1. Doesn't fully reload when renamed
2. Click to edit like in file explorer

AI cannot see everything. When you ask it to look at my game, it just looks at the nodes in the current scene. It should look at all scenes, all nodes, and all your files too.

AI may want to know the vals of properties while running the game (transform of player, etc). Could add a way for it to specify this, so it tells the game what to monitor. Then it's got a good view. Idea would be list the properties + time delta to record those properties. 'Screenshot' could also be a property. Give it examples too.

DONE: AI cannot see everything. When you ask it to look at my game, it just looks at the nodes in the current scene. It should look at all scenes, all nodes, and all your files too.

DONE: There's a bug where list nodes isn't showing all files in the correct way. It's not resolving the paths right.

DONE: There's a bug where list files isn't providing a list of any files.

DONE: The AI should be able to close a running game.

System Prompt Updates:

DONE: A the beginning of your conversation, you don't have any project context. Explore all scenes and files necessary to understand the game enough to answer.

DONE: The user is testing, so their commands are relative to the game, not the engine. For example, if the user says 'Center it' they probably mean centered to the camera.

DONE: The AI should heavily prefer to edit elements direclty in the scene rather than use scripting. This is more transparent, checks for errors before runtime, and leverages existing infrastrucutre.

## Bugs

- ~~**Context indicator wrong during run**: Context counter drops while model is running, rises when done — should be opposite.~~
- ~~**Model hallucinates success**: Says it fixed something confidently when it didn't. Add system prompt guidance to hedge uncertainty until the model can observe actual output.~~
- ~~**Validation error not retried**: When model returns plain text instead of JSON, it gets a `validation_error` but doesn't retry. Model should recognize this and reformat.~~
  - ~~Example: `Expected 'true', 'false', or 'null', got 'Assistant'`~~
- **Script update without verify**: Model modifies a file and then immediately reads it back — no real reason to do both in the same action set.
- **Script update sends full file**: Model passes the entire script on update instead of just the changed section. Causes bugs when the full content isn't reproduced exactly, and wastes context.
- **Model pretends action succeeded**: When a file modification fails, model continues as if it worked. Needs retry or explicit failure acknowledgment.

## System Prompt / Model Behavior

- ~~**Turn confusion**: Model is confused about when its turn starts/ends, particularly around when to begin "thinking." Needs clearer system prompt guidance. Dig into conversation history to repro.~~
- ~~**Prompt injection vulnerability**: User messages containing words like "final mode", "tool mode", "actions" can influence model behavior. Need a wrapper/sanitization layer so those strings are not treated as control signals.~~
- ~~**Prefer scene structure over scripts**: Prompt should instruct model to prefer editor/scene solutions over scripted ones where applicable (e.g. camera follow via node parenting, not a script).~~
- ~~**GDScript errors not auto-surfaced**: Model should automatically receive GDScript parse/runtime errors after all tools are called each turn.~~
- **Runtime output not surfaced**: Model should automatically see game output/failure logs, ideally alongside script errors.

## UI / UX

- **`update_todos` action missing from UI**: Can't see an action for "make task list" — may not be surfaced in the panel.
- **Model thinking not visible**: Would be very useful to see the model's exact output including thinking blocks.
- **Multi-chat window**: Need support for multiple concurrent chat sessions.

## Infrastructure / Architecture

- **Auto-compact needed**: Approaching context limits; auto-compact mechanism required.
- **Diff-based script updates**: Script modifications should use diffs, not full replacements. Saves context and prevents silent corruption bugs.
- **Git / version control integration**: Necessary for tracking changes made by the model.

Should think about merging the plan with commentary. allow commentary w/o tasks. would really need to ensure that the prompt is rock solid so we don't get just commentary hallucinating. this wouldn't be a new thing though, it already does this.

Screenshot logic doesn't properly screenshot if the game isn't focused. if it's not focused, it just screenshots whatever happens to be on that portion of the screen.