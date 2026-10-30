#!/usr/bin/env bash
set -euo pipefail
install -o debugger -g debugger -m 600 /run/lab-key.pub /home/debugger/.ssh/authorized_keys
root_password=$(cat /run/lab-root-password)
test -n "$root_password"
printf 'root:%s\n' "$root_password" | chpasswd
unset root_password
chown debugger:debugger /workspace/build
install -d -o debugger -g debugger -m 2775 /workspace/build/ccache
# Also persist the limit in the cache itself, overriding any older cache config.
sudo -u debugger ccache --max-size=10G
ssh-keygen -A
exec /usr/sbin/sshd -D -e
