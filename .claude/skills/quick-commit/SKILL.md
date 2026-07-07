---
name: quick-commit
description: Generate standardized English conventional commit messages and output executable git commit commands after staging files
---
# Skill: Conventional Git Commit Assistant
## Applicable Scenarios
Generate standardized short commit messages after user runs `git add` to stage files. Auto-analyze code diff changes and output ready-to-run git commit commands.

## Hard Enforced Rules
1. Precondition: Assume all target files are already staged via `git add`. Do NOT generate any git add commands.
2. Commit message core rules
   - Use only one matching conventional prefix: fix / feat / refactor / docs / style / test / chore
   - Main message written entirely in English, 10–30 words length limit
   - No Chinese characters, no redundant descriptions, no verbose explanations
3. Mandatory output format
   Print a direct executable command: `git commit -m "your-short-conventional-message"`
4. Optional extra output
   If more than 3 files changed, append `git status` for user verification below the commit command.
5. Output constraint
   Only output the required command(s). Remove all extra commentary, explanations or redundant text.

## Example Output Sample
git commit -m "feat(driver): add virtio framebuffer flush optimization logic"
git status
