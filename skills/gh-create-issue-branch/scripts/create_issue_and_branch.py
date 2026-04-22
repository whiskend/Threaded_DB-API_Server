#!/usr/bin/env python3
"""
Create a GitHub issue with gh and switch to a branch named <type>-#<issue>.

The script uses subprocess argument lists rather than a shell string so branch
names containing "#" stay safe and predictable.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ISSUE_URL_RE = re.compile(r"/issues/(?P<number>\d+)(?:\s|$)")


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


def validate_branch_name(branch_name: str) -> None:
    result = subprocess.run(
        ["git", "check-ref-format", "--branch", branch_name],
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return
    stderr = (result.stderr or result.stdout or "").strip()
    fail(f"Invalid branch name: {branch_name}\n{stderr}".rstrip())


def parse_issue_reference(output: str) -> tuple[int, str]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    for line in reversed(lines):
        match = ISSUE_URL_RE.search(line)
        if match:
            return int(match.group("number")), line
    fail(
        "Could not parse an issue URL from gh output.\n"
        f"Raw output:\n{output}".rstrip()
    )


def resolve_body_file(body: str | None, body_file: str | None) -> tuple[Path | None, Path | None]:
    if body and body_file:
        fail("Pass either --body or --body-file, not both.")
    if body_file:
        path = Path(body_file).expanduser()
        if not path.exists():
            fail(f"Body file does not exist: {path}")
        return path, None
    if body is None:
        return None, None
    with tempfile.NamedTemporaryFile("w", suffix=".md", encoding="utf-8", delete=False) as handle:
        handle.write(body)
        temp_path = Path(handle.name)
    return temp_path, temp_path


def create_issue(title: str, body_path: Path, repo: str | None, assignee: str | None) -> tuple[int, str]:
    cmd = ["gh", "issue", "create", "--title", title, "--body-file", str(body_path)]
    if assignee:
        cmd.extend(["--assignee", assignee])
    if repo:
        cmd.extend(["--repo", repo])
    try:
        result = run(cmd, capture_output=True)
    except subprocess.CalledProcessError as exc:
        output = (exc.stderr or exc.stdout or "").strip()
        fail(f"`gh issue create` failed.\n{output}".rstrip())
    output = "\n".join(part for part in [result.stdout, result.stderr] if part).strip()
    issue_number, issue_url = parse_issue_reference(output)
    return issue_number, issue_url


def switch_branch(branch_name: str) -> None:
    try:
        run(["git", "switch", "-c", branch_name], capture_output=True)
    except subprocess.CalledProcessError as exc:
        output = (exc.stderr or exc.stdout or "").strip()
        fail(f"`git switch -c {branch_name}` failed.\n{output}".rstrip())


def push_branch(branch_name: str) -> None:
    try:
        run(["git", "push", "-u", "origin", branch_name], capture_output=True)
    except subprocess.CalledProcessError as exc:
        output = (exc.stderr or exc.stdout or "").strip()
        fail(f"`git push -u origin {branch_name}` failed.\n{output}".rstrip())


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create a GitHub issue and switch to <type>-#<issue>.",
    )
    parser.add_argument("--title", help="Issue title. Required unless --issue-number is provided.")
    parser.add_argument("--body", help="Issue body as a string.")
    parser.add_argument("--body-file", help="Path to a Markdown file for the issue body.")
    parser.add_argument("--repo", help="Optional gh repo override in owner/name form.")
    parser.add_argument("--prefix", default="feat", help="Branch type prefix. Defaults to 'feat'.")
    parser.add_argument(
        "--assignee",
        default="@me",
        help='Issue assignee login. Defaults to "@me" for the authenticated user.',
    )
    parser.add_argument(
        "--no-assignee",
        action="store_true",
        help="Create the issue without assigning anyone.",
    )
    parser.add_argument(
        "--push",
        action="store_true",
        help="Push the created branch to origin and set upstream.",
    )
    parser.add_argument(
        "--no-push",
        action="store_true",
        help="Do not push the created branch to origin.",
    )
    parser.add_argument(
        "--issue-number",
        type=int,
        help="Skip issue creation and create the branch from an existing issue number.",
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="Allow branch creation even when the working tree has uncommitted changes.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the planned issue and branch values without creating the issue or branch.",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    ensure_command("git")
    ensure_git_repo()

    prefix = args.prefix.strip().rstrip("-")
    if not prefix:
        fail("Branch prefix cannot be empty.")
    assignee = None if args.no_assignee else args.assignee.strip()
    if args.issue_number is None and not assignee and not args.no_assignee:
        fail("Assignee cannot be empty. Pass --no-assignee to skip assignment.")
    if args.push and args.no_push:
        fail("Pass either --push or --no-push, not both.")
    should_push = not args.no_push

    if working_tree_dirty() and not args.allow_dirty:
        fail(
            "Working tree has uncommitted changes.\n"
            "Commit or stash them first, or rerun with --allow-dirty if you want those changes on the new branch."
        )

    cleanup_path: Path | None = None
    issue_number: int
    issue_url: str | None = None

    try:
        if args.issue_number is not None:
            issue_number = args.issue_number
        else:
            if not args.title:
                fail("--title is required unless --issue-number is provided.")
            body_path, cleanup_path = resolve_body_file(args.body, args.body_file)
            if body_path is None:
                fail("Provide --body or --body-file when creating a new issue.")
            if args.dry_run:
                issue_number = 1
                issue_url = "https://github.com/OWNER/REPO/issues/1"
            else:
                ensure_command("gh")
                ensure_gh_auth()
                issue_number, issue_url = create_issue(args.title, body_path, args.repo, assignee)

        branch_name = f"{prefix}-#{issue_number}"
        validate_branch_name(branch_name)

        if args.dry_run:
            print(f"issue_number={issue_number}")
            if issue_url:
                print(f"issue_url={issue_url}")
            if assignee:
                print(f"assignee={assignee}")
            print(f"push_origin={'true' if should_push else 'false'}")
            print(f"branch_name={branch_name}")
            return

        switch_branch(branch_name)
        if should_push:
            push_branch(branch_name)
        if issue_url:
            print(f"issue_url={issue_url}")
        print(f"issue_number={issue_number}")
        if assignee:
            print(f"assignee={assignee}")
        print(f"push_origin={'true' if should_push else 'false'}")
        print(f"branch_name={branch_name}")
    finally:
        if cleanup_path and cleanup_path.exists():
            cleanup_path.unlink()


if __name__ == "__main__":
    main()
