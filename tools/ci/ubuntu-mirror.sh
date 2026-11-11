#!/bin/bash
# GitHub's Ubuntu runners: apt through Ubuntu's mirror at kernel.org first.
# On Nov 11 2026 the runners could not resolve any ubuntu.com name for
# hours (azure.archive, archive and security.ubuntu.com) while other hosts
# answered: every job that installs packages failed.  ubuntu.com stays as
# the fallback.
set -e
printf '%s\tpriority:1\n%s\tpriority:2\n' \
    http://mirrors.edge.kernel.org/ubuntu/ http://archive.ubuntu.com/ubuntu/ \
    | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
# security updates through the same list (the runner points them at security.ubuntu.com)
for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do
    [ -f "$f" ] && sudo sed -i 's|https\?://security\.ubuntu\.com/ubuntu/\?|mirror+file:/etc/apt/apt-mirrors.txt|g' "$f"
done
true
