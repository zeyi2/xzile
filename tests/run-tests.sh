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

# Usage: run-tests.sh [TEST.el...]
#
# With no arguments, run the whole suite. Each test copies an input
# file, edits it with the commands in TEST.el, and compares the result
# with TEST.output. The editor must exit successfully and print nothing
# on stderr. Set XZILE to the editor to test (default: src/xzile).
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

run_test () {
	test=${1%.el}
	name=${test#"$srcdir"/tests/}
	edit_file=$test.input
	input_file=$test.orig-input
	[ -e "$input_file" ] || input_file=$srcdir/tests/test.input

	cp "$input_file" "$edit_file" && chmod 644 "$edit_file" || return 1
	errors=$("$editor" --no-init-file "$edit_file" --load "$test.el" </dev/null 2>&1 >/dev/null)
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
	rm -f "$edit_file" "$edit_file~"
	return 0
}

pass=0
fail=0

run_list () {
	for t in "$@"; do
		if run_test "$t"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
		fi
	done
}

if [ $# -gt 0 ]; then
	run_list "$@"
else
	run_list "$srcdir"/tests/*.el "$srcdir"/tests/interactive/*.el \
		"$srcdir"/tests/regression/*.el "$srcdir"/tests/zile-only/*.el \
		"$srcdir"/tests/zile-only/interactive/*.el

	# These depend on the screen size.
	COLUMNS=80 LINES=24
	export COLUMNS LINES
	run_list "$srcdir"/tests/zile-only/fixed-screen/*.el
	unset COLUMNS LINES

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
