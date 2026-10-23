#!/bin/bash
# Update an installed dashboard stack to a release in one step.
#
#   bash deploy/update.sh            # latest release tag
#   bash deploy/update.sh v1.2.15    # a given tag (or a commit, for rollback)
#   bash deploy/update.sh main       # tip of main (testing only)
#
# Run as the normal user from the repo checkout; asks for sudo once for the
# installer. Refuses to run over local edits. Prints the rollback command.
set -euo pipefail

cd "$(dirname "$0")/.."
say() { echo -e "\033[1;32m==> $*\033[0m"; }
die() { echo -e "\033[1;31m!!  $*\033[0m" >&2; exit 1; }

git diff --quiet && git diff --cached --quiet \
  || die "local changes in $(pwd): commit or 'git stash' them first"

say "Fetching releases"
# --force: a local tag that differs from GitHub's (one made by hand on the
# boat) is replaced instead of failing the fetch; errors are shown.
git fetch --tags --force origin || die "git fetch failed (message above): check the network and GitHub access"
REF="${1:-$(git tag --list 'v*' --sort=-v:refname | head -1)}"
[[ -n "$REF" ]] || die "no release tags found; pass a tag or 'main'"
OLD="$(git describe --tags --always)"

if git show-ref --verify --quiet "refs/remotes/origin/$REF"; then
  git checkout --quiet "$REF" && git pull --quiet --ff-only origin "$REF"
else
  git checkout --quiet "$REF" 2>/dev/null || die "unknown tag or commit: $REF"
fi
NEW="$(git describe --tags --always)"
say "Updating $OLD -> $NEW"

# MOOS_DIR is passed through when set (pre-built MOOS outside the default
# search path, e.g. ~/Documents/automation/moos-ivp/build/MOOS/MOOSCore).
sudo ${MOOS_DIR:+MOOS_DIR="$MOOS_DIR"} bash deploy/install.sh

say "Service status"
for u in influxdb grafana-server telegraf usv-signalk-bridge usv-can-bridge \
         usv-mayara-bridge usv-frigate-bridge usv-auth nginx; do
  systemctl cat "$u" &>/dev/null || continue
  printf '  %-22s %s\n' "$u" "$(systemctl is-active "$u")"
done

if pgrep -x MOOSDB >/dev/null; then
  echo
  echo "  MOOS is running: grafana_bridge was rebuilt but pAntler still holds"
  echo "  the old one. Restart the mission when convenient to pick it up."
fi
echo
say "Done: $NEW. Reload the browser with Ctrl+F5."
echo "  Rollback: bash deploy/update.sh $OLD"
