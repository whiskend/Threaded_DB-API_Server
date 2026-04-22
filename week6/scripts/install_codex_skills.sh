#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_DIR="${REPO_ROOT}/skills"

TARGET_ROOT="${CODEX_HOME:-$HOME/.codex}"
TARGET_DIR="${TARGET_ROOT}/skills"
DRY_RUN=0

usage() {
  cat <<'EOF'
Install project Codex skills into the local Codex auto-discovery directory.

Usage:
  scripts/install_codex_skills.sh [--target DIR] [--dry-run]

Options:
  --target DIR  Override the Codex home directory. Skills will be installed to DIR/skills.
  --dry-run     Print planned copy operations without writing files.
  -h, --help    Show this help text.

Examples:
  scripts/install_codex_skills.sh
  scripts/install_codex_skills.sh --dry-run
  scripts/install_codex_skills.sh --target "$HOME/.codex"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      if [[ $# -lt 2 ]]; then
        echo "--target requires a directory argument" >&2
        exit 1
      fi
      TARGET_ROOT="$2"
      TARGET_DIR="${TARGET_ROOT}/skills"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -d "${SOURCE_DIR}" ]]; then
  echo "Project skills directory not found: ${SOURCE_DIR}" >&2
  exit 1
fi

SKILL_DIRS=()
while IFS= read -r skill_dir; do
  SKILL_DIRS+=("${skill_dir}")
done < <(find "${SOURCE_DIR}" -mindepth 1 -maxdepth 1 -type d | sort)

if [[ ${#SKILL_DIRS[@]} -eq 0 ]]; then
  echo "No project skills found in ${SOURCE_DIR}" >&2
  exit 1
fi

if [[ ${DRY_RUN} -eq 1 ]]; then
  echo "Dry run: would install skills into ${TARGET_DIR}"
else
  mkdir -p "${TARGET_DIR}"
  echo "Installing skills into ${TARGET_DIR}"
fi

installed_count=0

for skill_dir in "${SKILL_DIRS[@]}"; do
  skill_name="$(basename "${skill_dir}")"
  skill_file="${skill_dir}/SKILL.md"

  if [[ ! -f "${skill_file}" ]]; then
    echo "Skipping ${skill_name}: SKILL.md not found"
    continue
  fi

  dest_dir="${TARGET_DIR}/${skill_name}"
  if [[ ${DRY_RUN} -eq 1 ]]; then
    echo "Would install ${skill_name} -> ${dest_dir}"
  else
    mkdir -p "${dest_dir}"
    rsync -a \
      --delete \
      --exclude '__pycache__' \
      --exclude '*.pyc' \
      "${skill_dir}/" "${dest_dir}/"
    echo "Installed ${skill_name}"
  fi
  installed_count=$((installed_count + 1))
done

if [[ ${installed_count} -eq 0 ]]; then
  echo "No installable skills were found." >&2
  exit 1
fi

if [[ ${DRY_RUN} -eq 1 ]]; then
  echo "Dry run complete. ${installed_count} skill(s) would be installed."
else
  echo "Done. ${installed_count} skill(s) installed."
  echo "If Codex is already running, start a new session or restart the app to refresh the skill list."
fi
