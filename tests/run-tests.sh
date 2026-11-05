#!/bin/sh
# Run the Lisp tests
#
# Copyright (c) 2009-2021 Free Software Foundation, Inc.
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

# Usage: run-tests.sh [TEST.el|TEST.test...]
#
# With no arguments, run the whole suite. Each test copies an input
# file, edits it with the commands in TEST.el, and compares the result
# with TEST.output. The editor must exit successfully and print nothing
# on stderr. A TEST.test is a shell script that runs the editor itself,
# given in $XZILE; it prints why if it fails. Set XZILE to the editor
# to test (default: src/xzile).
#
# N.B. Tests that use execute-kbd-macro must note that keyboard input
# is only evaluated once the script has finished running.

srcdir=$(cd "$(dirname "$0")/.." && pwd)
editor=${XZILE:-src/xzile}
case $editor in
	/*) ;;
	*) editor=$(pwd)/$editor ;;
esac

TERM=${TERM_FOR_TESTS:-vt100}
export TERM

# M-x shell runs $SHELL.
SHELL=/bin/sh
export SHELL

run_test () {
	test=${1%.el}
	name=${test#"$srcdir"/tests/}
	edit_file=$test.input
	input_file=$test.orig-input
	[ -e "$input_file" ] || input_file=$srcdir/tests/test.input

	cp "$input_file" "$edit_file" && chmod 644 "$edit_file" || return 1
	errors=$("$editor" --no-init-file "$edit_file" ${init_file:+--load "$init_file"} --load "$test.el" \
		</dev/null 2>&1 >/dev/null)
	status=$?
	if [ $status -ne 0 ]; then
		echo "FAIL: $name: exit status $status"
		[ -n "$errors" ] && echo "$errors"
		return 1
	fi
	if [ -n "$errors" ]; then
		echo "FAIL: $name: output on stderr:"
		echo "$errors"
		return 1
	fi
	if ! diff -u "$test.output" "$edit_file"; then
		echo "FAIL: $name: wrong output"
		return 1
	fi
	if [ -f "$test.expect-log" ]; then
		while IFS= read -r line; do
			if ! grep -F -q -- "$line" "$XZILE_DEBUG_LOG"; then
				echo "FAIL: $name: debug log lacks: $line"
				return 1
			fi
		done < "$test.expect-log"
	fi
	rm -f "$edit_file" "$edit_file~"
	return 0
}

pass=0
fail=0

# The remote file tests, on a disposable SSH server. Each test's
# debug log must contain the lines of its TEST.expect-log, if any.
# The tests find the remote home directory in $XZ_REMOTE_HOME. A
# TEST.test is a shell script that runs the editor itself, given the
# server's directory in $XZ_FIXTURE; it prints why if it fails.
run_remote_tests () {
	fixture=$(mktemp -d "${TMPDIR:-/tmp}/xzile-sshd.XXXXXX") || return 1
	reason=$(sh "$srcdir/tests/remote/sshd-fixture.sh" start "$fixture" 2>&1)
	status=$?
	if [ $status -ne 0 ]; then
		sh "$srcdir/tests/remote/sshd-fixture.sh" stop "$fixture"
		if [ $status -eq 77 ]; then
			echo "SKIP: remote tests: $reason"
			return 0
		fi
		echo "FAIL: remote tests: SSH server: $reason"
		fail=$((fail + 1))
		return 1
	fi
	printf '(setq ssh-config-file "%s/ssh_config")\n' "$fixture" > "$fixture/init.el"
	XZ_REMOTE_HOME=$fixture/home XZILE_DEBUG=remote
	export XZ_REMOTE_HOME XZILE_DEBUG
	init_file=$fixture/init.el
	XZILE_DEBUG_LOG=$fixture/debug.log
	export XZILE_DEBUG_LOG
	for t in "$@"; do
		rm -f "$XZILE_DEBUG_LOG"
		case $t in
		*.test)
			errors=$(XZILE=$editor XZ_FIXTURE=$fixture sh "$t" 2>&1)
			if [ $? -eq 0 ]; then
				pass=$((pass + 1))
			else
				echo "FAIL: ${t#"$srcdir"/tests/}:"
				printf '%s\n' "$errors"
				fail=$((fail + 1))
			fi
			continue
			;;
		esac
		if run_test "$t"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
		fi
	done
	unset init_file XZ_REMOTE_HOME XZILE_DEBUG XZILE_DEBUG_LOG
	sh "$srcdir/tests/remote/sshd-fixture.sh" stop "$fixture"
}

run_list () {
	for t in "$@"; do
		case $t in
		*.test)
			errors=$(XZILE=$editor sh "$t" 2>&1)
			if [ $? -eq 0 ]; then
				pass=$((pass + 1))
			else
				echo "FAIL: ${t#"$srcdir"/tests/}:"
				printf '%s\n' "$errors"
				fail=$((fail + 1))
			fi
			continue
			;;
		esac
		if run_test "$t"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
		fi
	done
}

if [ $# -gt 0 ]; then
	for t in "$@"; do
		case $t in
			*/remote/*) run_remote_tests "$t" ;;
			*) run_list "$t" ;;
		esac
	done
else
	run_list "$srcdir"/tests/*.el "$srcdir"/tests/interactive/*.el \
		"$srcdir"/tests/regression/*.el "$srcdir"/tests/zile-only/*.el \
		"$srcdir"/tests/zile-only/interactive/*.el "$srcdir"/tests/zile-only/*.test

	# These depend on the screen size.
	COLUMNS=80 LINES=24
	export COLUMNS LINES
	run_list "$srcdir"/tests/zile-only/fixed-screen/*.el
	unset COLUMNS LINES

	run_remote_tests "$srcdir"/tests/remote/*.el "$srcdir"/tests/remote/*.test

	# An unknown option is reported in the minibuffer, not on stderr.
	errors=$("$editor" --unknown-flag --load "$srcdir/tests/quit.el" </dev/null 2>&1 >/dev/null)
	if [ $? -eq 0 ] && [ -z "$errors" ]; then
		pass=$((pass + 1))
	else
		echo "FAIL: unknown option handling"
		[ -n "$errors" ] && echo "$errors"
		fail=$((fail + 1))
	fi
fi

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
