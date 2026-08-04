# Claude Instructions

## Committing
Never commit code without explicit confirmation that it has been tested first. Do not commit just because implementation is complete.

Do not claim that the code compiles until you have compiled it. But you don't need to compile each time, it takes up a lot of context. You can give it to me if you are confident.


## Sub-Agent Shortcut
The term 'SA' means 'spin off a sub-agent in the background to accomplish this task. For example, I could go 'SA to determine if all files are being returned by the "list_files" command', which translates to 'Spin off a sub-agent in the background to determine if all files are being returned by the "list_files" command.

# Project Instructions

When changing existing engine features, consider reviewing related commits. They may contain important insights into decision making.

Before commiting a change, please check the 'docs' folder. Update the documentation with the changes made, if it is at a conceptual level on-par with the existing documents. Do not include overly technical information. Either change an existing document, or create a new document, depending on if the change modifies existing functionality or creates new ones (or hasn't been documented).

Logs of the engine's AI chats are located here: C:\Users\Matthew\Documents\Godot AI Chats.
When I ask you to look at the most recent chats, search here for the most recently updated files.

There's an external tool called 'godot-ai-dashboard' located at C:\Users\Matthew\Documents\godot-ai-dashboard. I might also call it the watchtower. It provides critical insights, and is very useful.

Don't build the task yourself, unless instructed. Instead, instruct me when you are done and I will build. I have macros, particularly F5, which contains the build command in the case where I explicitly instruct you to build.

After making a change to the AI, if relevant, suggest a prompt to send to the AI that would test your changes.
