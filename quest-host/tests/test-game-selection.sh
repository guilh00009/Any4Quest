#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# The JDK compiler module is also usable on distributions without a javac launcher.
java com.sun.tools.javac.Main -Xlint:all -Werror -d "$out" \
  "$root/quest-host/java/com/astrobotquest/vrhost/GameSelection.java" \
  "$root/quest-host/tests/GameSelectionTest.java"
java -ea -cp "$out" com.astrobotquest.vrhost.GameSelectionTest
