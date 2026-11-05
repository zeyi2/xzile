#!/bin/sh
# A disposable SSH server for the remote file tests
#
# Copyright (c) 2026 Zeyi2 <zeyi2@nekoarch.cc>
#
# This file is part of XZile.
#
# XZile is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# XZile is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

# Usage: sshd-fixture.sh start DIR | stop DIR
#
# `start' runs an sshd of its own, as the current user, on a free port
# of 127.0.0.1 and the next one (written to DIR/port2), with fresh host
# and user keys in DIR (new or empty), and writes DIR/ssh_config with
# these hosts:
#
#   xztest     logs in with the user key; the remote home is DIR/home
#   xzfq.example  the same, under a name with a domain
#   xzbadkey   offers a key the server does not accept
#   xznohostkey  knows no host key for the server
#
# The remote user's $SHELL is DIR/remote-shell, which runs sh with
# XZ_REMOTE_SHELL=yes in its environment.
#
# Exits with status 77 if OpenSSH (ssh, sshd, ssh-keygen) is missing or
# the server cannot be started. `stop' stops the server and removes DIR.

set -u

cmd=${1-}
dir=${2-}
[ -n "$cmd" ] && [ -n "$dir" ] || { echo "usage: $0 start|stop DIR" >&2; exit 2; }

case $cmd in
stop)
	if [ -f "$dir/sshd.pid" ]; then
		kill "$(cat "$dir/sshd.pid")" 2>/dev/null
	fi
	rm -rf "$dir"
	exit 0
	;;
start) ;;
*) echo "usage: $0 start|stop DIR" >&2; exit 2 ;;
esac

sshd=
for c in "$(command -v sshd 2>/dev/null)" /usr/sbin/sshd /usr/local/sbin/sshd /sbin/sshd; do
	if [ -n "$c" ] && [ -x "$c" ]; then
		sshd=$c
		break
	fi
done
if [ -z "$sshd" ] || ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
	echo "OpenSSH (ssh, sshd, ssh-keygen) not found" >&2
	exit 77
fi

[ -d "$dir" ] || mkdir -m 700 "$dir" || exit 1
mkdir "$dir/home" || exit 1
ssh-keygen -q -t ed25519 -N '' -f "$dir/hostkey" || exit 77
ssh-keygen -q -t ed25519 -N '' -f "$dir/userkey" || exit 77
ssh-keygen -q -t ed25519 -N '' -f "$dir/otherkey" || exit 77
cp "$dir/userkey.pub" "$dir/authorized_keys"
printf '#!/bin/sh\nXZ_REMOTE_SHELL=yes\nexport XZ_REMOTE_SHELL\nexec /bin/sh "$@"\n' > "$dir/remote-shell" &&
	chmod 755 "$dir/remote-shell" || exit 1
: > "$dir/no_known_hosts"

started=
for try in 1 2 3 4 5 6 7 8 9 10; do
	port=$((20000 + ($$ * 7 + try * 1031) % 40000))
	port2=$((port + 1))
	cat > "$dir/sshd_config" <<EOF2
Port $port
Port $port2
ListenAddress 127.0.0.1
HostKey $dir/hostkey
AuthorizedKeysFile $dir/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
SetEnv HOME=$dir/home SHELL=$dir/remote-shell
PidFile $dir/sshd.pid
EOF2
	rm -f "$dir/sshd.pid"
	if "$sshd" -f "$dir/sshd_config" -E "$dir/sshd.log"; then
		for wait in 1 2 3 4 5 6 7 8 9 10; do
			[ -s "$dir/sshd.pid" ] && break
			sleep 0.2
		done
		if [ -s "$dir/sshd.pid" ]; then
			started=yes
			break
		fi
	fi
done
if [ -z "$started" ]; then
	echo "could not start sshd; see $dir/sshd.log" >&2
	exit 77
fi

hostkey=$(cut -d' ' -f1,2 "$dir/hostkey.pub")
printf '[127.0.0.1]:%s %s\n[127.0.0.1]:%s %s\n' "$port" "$hostkey" "$port2" "$hostkey" > "$dir/known_hosts"
echo "$port2" > "$dir/port2"
cat > "$dir/ssh_config" <<EOF2
Host xztest xzfq.example xzbadkey xznohostkey
  HostName 127.0.0.1
  Port $port
  IdentitiesOnly yes
  GlobalKnownHostsFile /dev/null
  StrictHostKeyChecking yes
Host xztest xzfq.example
  IdentityFile $dir/userkey
  UserKnownHostsFile $dir/known_hosts
Host xzbadkey
  IdentityFile $dir/otherkey
  UserKnownHostsFile $dir/known_hosts
Host xznohostkey
  IdentityFile $dir/userkey
  UserKnownHostsFile $dir/no_known_hosts
EOF2

# Wait until the server answers.
for wait in 1 2 3 4 5 6 7 8 9 10; do
	out=$(ssh -F "$dir/ssh_config" -o BatchMode=yes xztest 'cd && pwd' 2>/dev/null) &&
		[ "$out" = "$dir/home" ] && exit 0
	sleep 0.3
done
echo "sshd does not answer as expected; see $dir/sshd.log" >&2
exit 77
