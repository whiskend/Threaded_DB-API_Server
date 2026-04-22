---
name: gh-create-pr
description: "Create a polished Korean GitHub pull request from rough user notes, always ask for the PR title, PR description, and type prefix first, format the final PR title as feat: ..., assign it to the authenticated user, and include a Closes #123 footer derived from the current type-#123 branch. Use when the user wants to open a PR with gh after work is already on an issue-linked branch."
---

# Gh Create Pr

## Goal

Turn rough PR notes into a clean Korean pull request, always collect the PR title, PR description, and type prefix first, format the final PR title as `<prefix>: <clean Korean title>`, make sure the current branch is pushed to `origin`, assign the PR to the authenticated user, and include a GitHub-closing footer like `Closes #4` based on the current branch name.

## Workflow Gate

Use this skill when the implementation work is already on an issue-linked branch.

- If the current branch already looks like `<type>-#<issue-number>`, continue with this skill.
- If no issue-linked branch exists yet, stop and tell the user to start with `$gh-create-issue-branch`.
- If the current branch does not contain an issue number and the user still wants a PR, ask for an issue number override before creating the PR.

## Core Rules

- Always ask the user for the PR title, PR description, and type prefix in normal chat before drafting the final PR. Do not assume `request_user_input` is available.
- Ask for all required inputs in a single short message.
- Write the PR title and body in Korean by default. If the user provides rough notes in English, translate them into natural Korean unless the user explicitly asks for another language.
- Ask for a type prefix such as `feat`, `fix`, `bug`, `docs`, `refactor`, or `test`, and use that prefix at the start of the final PR title.
- Keep code identifiers, commands, API names, class names, and branch names unchanged when translating them would reduce precision.
- Assign the PR to the authenticated GitHub user by default with `--assignee "@me"`. Only skip or change the assignee when the user explicitly asks for that.
- Include `Closes #<issue-number>` in the PR body. Use `Closes`, not `closed`, so GitHub will close the linked issue automatically on merge.
- Push the current branch to `origin` and set upstream by default so the PR can be opened without prompts. Only skip the push when the user explicitly wants a local-only branch.
- If the working tree is dirty, warn that uncommitted changes will not be part of the PR and ask before continuing.

## Gather Inputs

Collect these inputs before creating the PR:

- rough PR title
- rough PR description
- type prefix for the PR title, default `feat`
- optional base branch override
- optional PR assignee override, default `@me`
- optional issue number override when the current branch does not contain one

Always ask once with a compact template like:

```text
아래 형식으로 PR 초안을 보내주세요.

제목 초안:
설명 초안:
타입 prefix(기본값: feat):
Base branch(선택):
Assignee(기본값: @me):
Issue 번호 override(현재 브랜치에서 못 찾을 때만):
```

If the user already supplied free-form notes, still ask them to confirm the title, description, and prefix explicitly before creating the PR.

## Draft The Pull Request

Rewrite the PR in Korean before creating it.

- Make the title short, specific, and action-oriented in Korean.
- Convert the final PR title to `<prefix>: <clean Korean title>`.
- Prefer a body with stable Korean headings.
- Keep the change summary and testing notes concrete.
- Move uncertainty into `비고` instead of presenting guesses as facts.
- Ensure the body ends with `Closes #<issue-number>`.

Use this shape unless the repository already implies another format:

```md
## 요약
...

## 변경 사항
...

## 테스트
- ...
- ...

## 비고
...

Closes #4
```

## Create The Pull Request

Prefer the helper script at `scripts/create_pr_from_branch.py`.

Standard flow:

1. Draft the final PR title and body.
2. Confirm `gh` exists and that `gh auth status` succeeds.
3. Detect the current branch and extract the issue number from it.
4. Write the body to a temporary Markdown file if that is the easiest way to pass multiline text.
5. Run the helper script to push the current branch, create the PR, self-assign it by default, and include the `Closes #<issue-number>` footer.
6. Report the PR URL, current branch, and linked issue number.

Example invocation:

```bash
python3 /path/to/gh-create-pr/scripts/create_pr_from_branch.py \
  --title "가상 DOM keyed diff 지원 PR 생성" \
  --body-file /tmp/pr-body.md
```

If the user provides a base branch override, pass `--base <branch>`.
If the user explicitly does not want an assignee, pass `--no-assignee`.
If the user explicitly wants a local-only branch, pass `--no-push`.

## Handle Failures

- If `gh` is missing, explain how to install it and stop before attempting PR creation.
- If `gh auth status` fails, explain how to log in and stop before attempting PR creation.
- If the current branch does not include an issue number, ask for an explicit issue number override or tell the user to start with `$gh-create-issue-branch`.
- If PR creation fails because a PR already exists for the branch, report that and show the existing PR URL if available.
- If the repository is dirty, explain that only committed changes are included in the PR and ask whether to continue.

## Resources

- `scripts/create_pr_from_branch.py`: Detect the current issue-linked branch, ensure a `Closes #<issue-number>` footer, push the branch to `origin`, and create a self-assigned PR with `gh`.
