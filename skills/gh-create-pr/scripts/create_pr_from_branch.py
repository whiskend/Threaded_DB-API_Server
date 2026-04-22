#!/usr/bin/env python3
"""
Create a GitHub pull request from the current issue-linked branch.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ISSUE_NUMBER_RE = re.compile(r"#(?P<number>\d+)")
PR_URL_RE = re.compile(r"https://github\.com/[^/\s]+/[^/\s]+/pull/\d+")


def fail(message: str, exit_code: int = 1) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(exit_code)


def run(cmd: list[str], *, capture_output: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=capture_output, text=True, check=check)


def ensure_command(name: str) -> None:
    if shutil.which(name):
        return
    fail(
        f"Required command not found: {name}\n"
        f"Install it first, then rerun this script."
    )


def ensure_git_repo() -> None:
    try:
        run(["git", "rev-parse", "--show-toplevel"], capture_output=True)
    except subprocess.CalledProcessError as exc:
        stderr = (exc.stderr or "").strip()
        fail(f"Not inside a git repository.\n{stderr}".rstrip())


def ensure_gh_auth() -> None:
    result = subprocess.run(
        ["gh", "auth", "status"],
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return
    stderr = (result.stderr or result.stdout or "").strip()
    fail(
        "gh is installed but not authenticated.\n"
        "Run `gh auth login -h github.com -w` and rerun the script.\n"
        f"{stderr}".rstrip()
    )


def working_tree_dirty() -> bool:
    result = run(["git", "status", "--porcelain"], capture_output=True)
    return bool(result.stdout.strip())


def current_branch() -> str:
    result = run(["git", "branch", "--show-current"], capture_output=True)
    branch_name = result.stdout.strip()
    if not branch_name:
        fail("Could not determine the current branch. Detached HEAD is not supported.")
    return branch_name


def issue_number_from_branch(branch_name: str) -> int | None:
    match = ISSUE_NUMBER_RE.search(branch_name)
    if not match:
        return None
    return int(match.group("number"))


def normalize_body(body_text: str, issue_number: int) -> str:
    footer = f"Closes #{issue_number}"
    footer_pattern = re.compile(rf"(?mi)^\s*{re.escape(footer)}\s*$")
    if footer_pattern.search(body_text):
        return body_text
    stripped = body_text.rstrip()
    if not stripped:
        return footer
    return f"{stripped}\n\n{footer}\n"


def resolve_body_text(body: str | None, body_file: str | None, issue_number: int) -> tuple[Path | None, Path | None]:
    if body and body_file:
        fail("Pass either --body or --body-file, not both.")
    if body_file:
        source_path = Path(body_file).expanduser()
        if not source_path.exists():
            fail(f"Body file does not exist: {source_path}")
        body_text = source_path.read_text(encoding="utf-8")
    elif body is not None:
        body_text = body
    else:
        return None, None

    normalized = normalize_body(body_text, issue_number)
    with tempfile.NamedTemporaryFile("w", suffix=".md", encoding="utf-8", delete=False) as handle:
        handle.write(normalized)
        temp_path = Path(handle.name)
    return temp_path, temp_path


def push_branch(branch_name: str) -> None:
    try:
        run(["git", "push", "-u", "origin", branch_name], capture_output=True)
    except subprocess.CalledProcessError as exc:
        output = (exc.stderr or exc.stdout or "").strip()
        fail(f"`git push -u origin {branch_name}` failed.\n{output}".rstrip())


def parse_pr_url(output: str) -> str:
    match = PR_URL_RE.search(output)
    if match:
        return match.group(0)
    fail(
        "Could not parse a PR URL from gh output.\n"
        f"Raw output:\n{output}".rstrip()
    )


def create_pr(
    title: str,
    body_path: Path,
    branch_name: str,
    issue_number: int,
    repo: str | None,
    base: str | None,
    assignee: str | None,
    draft: bool,
) -> str:
    cmd = [
        "gh",
        "pr",
        "create",
        "--title",
        title,
        "--body-file",
        str(body_path),
        "--head",
        branch_name,
    ]
    if base:
        cmd.extend(["--base", base])
    if repo:
        cmd.extend(["--repo", repo])
    if assignee:
        cmd.extend(["--assignee", assignee])
    if draft:
        cmd.append("--draft")
    try:
        result = run(cmd, capture_output=True)
    except subprocess.CalledProcessError as exc:
        output = "\n".join(part for part in [exc.stdout, exc.stderr] if part).strip()
        fail(f"`gh pr create` failed.\n{output}".rstrip())
    output = "\n".join(part for part in [result.stdout, result.stderr] if part).strip()
    return parse_pr_url(output)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create a Korean GitHub pull request from the current issue-linked branch.",
    )
    parser.add_argument("--title", help="Pull request title.", required=True)
    parser.add_argument("--body", help="Pull request body as a string.")
    parser.add_argument("--body-file", help="Path to a Markdown file for the pull request body.")
    parser.add_argument("--repo", help="Optional gh repo override in owner/name form.")
    parser.add_argument("--base", help="Optional base branch override.")
    parser.add_argument("--branch", help="Optional branch override. Defaults to the current branch.")
    parser.add_argument(
        "--issue-number",
        type=int,
        help="Override the issue number when it cannot be parsed from the branch.",
    )
    parser.add_argument(
        "--assignee",
        default="@me",
        help='PR assignee login. Defaults to "@me" for the authenticated user.',
    )
    parser.add_argument(
        "--no-assignee",
        action="store_true",
        help="Create the PR without assigning anyone.",
    )
    parser.add_argument(
        "--draft",
        action="store_true",
        help="Create the PR as a draft.",
    )
    parser.add_argument(
        "--no-push",
        action="store_true",
        help="Do not push the branch to origin before creating the PR.",
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow PR creation even when the working tree has uncommitted changes.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the planned PR values without creating the PR.",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    ensure_command("git")
    ensure_git_repo()

    branch_name = args.branch or current_branch()
    issue_number = args.issue_number if args.issue_number is not None else issue_number_from_branch(branch_name)
    if issue_number is None:
        fail(
            "Could not infer an issue number from the current branch.\n"
            "Use a branch like feat-#4 or pass --issue-number explicitly."
        )

    assignee = None if args.no_assignee else args.assignee.strip()
    if not assignee and not args.no_assignee:
        fail("Assignee cannot be empty. Pass --no-assignee to skip assignment.")

    if working_tree_dirty() and not args.allow_dirty:
        fail(
            "Working tree has uncommitted changes.\n"
            "Commit or stash them first, or rerun with --allow-dirty if you want to create the PR from the current committed state only."
        )

    body_path, cleanup_path = resolve_body_text(args.body, args.body_file, issue_number)
    if body_path is None:
        fail("Provide --body or --body-file.")

    try:
        if args.dry_run:
            print(f"branch_name={branch_name}")
            print(f"issue_number={issue_number}")
            print(f"assignee={assignee or 'none'}")
            print(f"push_origin={'false' if args.no_push else 'true'}")
            print(f"closes_footer=Closes #{issue_number}")
            return

        ensure_command("gh")
        ensure_gh_auth()

        if not args.no_push:
            push_branch(branch_name)

        pr_url = create_pr(
            title=args.title,
            body_path=body_path,
            branch_name=branch_name,
            issue_number=issue_number,
            repo=args.repo,
            base=args.base,
            assignee=assignee,
            draft=args.draft,
        )
        print(f"pr_url={pr_url}")
        print(f"branch_name={branch_name}")
        print(f"issue_number={issue_number}")
        if assignee:
            print(f"assignee={assignee}")
    finally:
        if cleanup_path and cleanup_path.exists():
            cleanup_path.unlink()


if __name__ == "__main__":
    main()
