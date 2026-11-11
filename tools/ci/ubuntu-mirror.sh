#!/bin/bash
# GitHub's Ubuntu runners: apt through Ubuntu's mirror at kernel.org when
# Ubuntu's own names do not resolve.  On Nov 11 2026 Canonical's DNS went
# down for hours (status.canonical.com): no ubuntu.com name resolved on the
# runners, apt could not fall back to another mirror by itself, and every
# job that installs packages failed or hung.  When ubuntu.com answers,
# nothing changes.
set -e
for i in 1 2 3; do
    getent hosts archive.ubuntu.com >/dev/null && { echo "ubuntu.com resolves: the runner's mirrors stay"; exit 0; }
    sleep 5
done
echo "ubuntu.com does not resolve: apt through mirrors.edge.kernel.org"
printf '%s\tpriority:1\n%s\tpriority:2\n' \
    http://mirrors.edge.kernel.org/ubuntu/ http://archive.ubuntu.com/ubuntu/ \
    | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
# security updates through the same list (the runner points them at security.ubuntu.com)
for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do
    [ -f "$f" ] && sudo sed -i 's|https\?://security\.ubuntu\.com/ubuntu/\?|mirror+file:/etc/apt/apt-mirrors.txt|g' "$f"
done
true
