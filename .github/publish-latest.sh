#!/bin/bash
# Puts a built .ipa on the fixed "latest" release, replacing the file of the
# same name. Both app builds (normal and NoJIT) call this, so the release is
# kept and only its files change:
#   https://github.com/<owner>/<repo>/releases/download/latest/<file>
set -euo pipefail
FILE="$1"
NOTES="Newest builds of main. Last update: $FILE from $(git rev-parse --short HEAD), $(date -u '+%Y-%m-%d %H:%M UTC').

- Ember-unsigned.ipa: the normal app (needs JIT).
- Ember-NoJIT-unsigned.ipa: runs without JIT on the interpreter; much slower. Installs next to the normal app as \"Ember NoJIT\".

Unsigned: install with AltStore, SideStore, Sideloadly or LiveContainer."
if ! gh release view latest >/dev/null 2>&1; then
    gh release create latest --target "$GITHUB_SHA" --prerelease --title "Latest build" --notes "$NOTES" \
        || true   # the other build may have just created it
fi
gh release upload latest "$FILE" --clobber
gh release edit latest --notes "$NOTES"
