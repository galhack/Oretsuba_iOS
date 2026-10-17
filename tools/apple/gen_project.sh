#!/bin/sh
# Generate app/Oretsuba.xcodeproj from app/project.yml with XcodeGen.
#   ORE_TEAM=<your team id> tools/apple/gen_project.sh
# Without ORE_TEAM the team id is read from app/.team (one line, not in git)
# when that file exists; the Mac build needs none (it is signed locally).
# XcodeGen comes from the PATH or build/tools (downloaded on first use).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
XG="$(command -v xcodegen || true)"
if [ -z "$XG" ]; then
	XG="$ROOT/build/tools/xcodegen/bin/xcodegen"
	if [ ! -x "$XG" ]; then
		mkdir -p "$ROOT/build/tools"
		curl -sL -o "$ROOT/build/tools/xcodegen.zip" https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip
		unzip -q -o "$ROOT/build/tools/xcodegen.zip" -d "$ROOT/build/tools"
	fi
fi
if [ -z "${ORE_TEAM:-}" ] && [ -f "$ROOT/app/.team" ]; then
	ORE_TEAM="$(tr -d ' \n' < "$ROOT/app/.team")"
fi
export ORE_TEAM="${ORE_TEAM:-}"
export ORE_BUNDLE_SUFFIX="${ORE_TEAM:-player}"
"$XG" generate --spec "$ROOT/app/project.yml" --project "$ROOT/app"
