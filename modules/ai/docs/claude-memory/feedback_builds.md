---
name: feedback-builds
description: "User runs engine builds themselves — don't launch scons compiles unprompted"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 689f460a-b2fc-40f2-8cb0-991334f04ac1
  modified: 2026-07-20T22:31:18.469Z
---

The user said "don't worry about building, I will take care of that" when I kicked off a
scons build to verify changes.

**Why:** Engine builds are long and the user prefers to control when they run; global
instructions already say not to compile every time.

**How to apply:** After making C++ changes, state confidence level and flag anything worth
checking instead of building. Only compile if explicitly asked.
