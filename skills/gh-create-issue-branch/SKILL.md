---
name: gh-create-issue-branch
description: Create a polished Korean GitHub issue from rough user notes, always ask for the issue title and description first, prepend a commit-style type such as feat or fix to the final title, create and switch to a matching feat-#123 branch, and continue the implementation there. Use when the user wants an issue-first workflow driven by GitHub CLI instead of manually drafting the issue and branch.
---

# Gh Create Issue Branch

## Goal

Turn rough feature or bug notes into a clean Korean GitHub issue, always collect the issue title and description first, prepend a commit-style type such as `feat` or `fix` to the final title, create the issue with `gh`, create and switch to a branch named exactly `<type>-#<issue-number>`, push that branch to `origin`, and continue the work on that branch.

## Core Rules

- Always ask the user for the issue title and issue description in normal chat before drafting the final issue. Do not assume `request_user_input` is available.
- Ask for all required inputs in a single short message.
- Write the GitHub issue title and body in Korean by default. If the user provides rough notes in English, translate them into natural Korean unless the user explicitly asks for another language.
- Keep code identifiers, commands, API names, class names, and branch names unchanged when translating them would reduce precision.
- Assign the new issue to the authenticated GitHub user by default with `--assignee "@me"`. Only skip or change the assignee when the user explicitly asks for that.
- Push the new branch to `origin` and set upstream by default so the branch is visible on GitHub. Only skip the push when the user explicitly wants a local-only branch.
- Improve wording, structure, and acceptance criteria, but do not invent product requirements.
- Quote branch names whenever showing shell commands because `#` is safer when quoted in human-readable examples.
- Check the working tree before branching. If it is dirty, warn that existing changes will follow the new branch and ask before proceeding.

## Gather Inputs

Collect these inputs before creating the issue:

- rough issue title
- rough issue description
- optional target repo override for `gh --repo owner/name`
- optional type prefix such as `feat`, `fix`, `refactor`, or `docs`, default `feat`
- optional assignee override, default `@me`

Always ask once with a compact template like:

```text
아래 형식으로 이슈 초안을 보내주세요.

제목 초안:
설명 초안:
Repo override(선택):
타입 prefix(기본값: feat):
Assignee(기본값: @me):
```

## Draft The GitHub Issue

Rewrite the issue in Korean before creating it.

- Make the title short, specific, and action-oriented in Korean.
- Prefix the final title with the selected type in the exact form `<type>: <korean title>`.
- Prefer a body with stable Korean headings.
- Keep acceptance criteria concrete and testable.
- Move uncertainty into `Notes` or `Open Questions` instead of presenting guesses as facts.

Use this shape unless the user or repository already implies another format:

```md
## 요약
...

## 문제
...

## 범위
...

## 완료 조건
- ...
- ...

## 비고
...
```

## Create The Issue And Branch

Prefer the helper script at `scripts/create_issue_and_branch.py`.

Standard flow:

1. Draft the final issue title and body.
2. Confirm `gh` exists and that `gh auth status` succeeds.
3. Write the body to a temporary Markdown file if that is the easiest way to pass multiline text.
4. Run the helper script to create the issue, self-assign it by default, switch branches, and push the branch to `origin`.
5. Report the issue URL, issue number, and branch name.
6. Continue implementation on the new branch.

Example invocation:

```bash
python3 /path/to/gh-create-issue-branch/scripts/create_issue_and_branch.py \
  --title "feat: 가상 DOM keyed diff 지원 추가" \
  --body-file /tmp/issue-body.md \
  --assignee "@me" \
  --push \
  --prefix feat
```

The script creates a branch named exactly `<type>-#<issue-number>`.

If the user provides a repo override, pass `--repo owner/name`.
If the user explicitly does not want an assignee, pass `--no-assignee`.
If the user explicitly wants a local-only branch, pass `--no-push`.

## Continue The Work

After the branch is created:

- tell the user which issue was created
- tell the user which branch is now checked out
- perform the requested implementation work on that branch

If branch creation succeeded but the implementation should not start yet, stop after reporting the issue URL and branch name.

## Handle Failures

- If `gh` is missing, explain how to install it and stop before attempting issue creation.
- If `gh auth status` fails, explain how to log in and stop before attempting issue creation.
- If the branch already exists, tell the user and ask whether to switch to it or choose another naming rule.
- If the repository is dirty, explain the risk and ask whether to continue with the current uncommitted changes.

## Resource

- `scripts/create_issue_and_branch.py`: Create the issue with `gh`, extract the issue number, validate the `<type>-#<issue>` branch name, and switch to the new branch.
