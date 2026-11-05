#!/bin/sh
# Run the xzile-gui keyboard, input method and mouse tests
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

# Usage: run-tests.sh [TEST.events...]
#
# Each test edits a copy of TEST.orig-input (default: tests/test.input)
# in xzile-gui, which is fed the SDL events in TEST.events (see
# sdl-inject.c) and then saves and quits; the file must then equal
# TEST.output. A "# locales:" line lists the locales to run the test in;
# the default, C, is what an app started from Finder or a desktop menu
# gets. If there is a TEST.hover, the links the pointer hovered, as the
# "hover" debug log gives them (see update_hover in gui.c), must match
# its lines, each a shell pattern, in turn.
# SDL's dummy video driver is used unless SDL_VIDEODRIVER is set,
# so no display is needed. Set XZILE_GUI to the editor to test (default:
# src/xzile-gui). The events are injected with LD_PRELOAD, so this runs
# on Linux; it needs CC (default: cc), PKG_CONFIG and SDL2's headers.

srcdir=$(cd "$(dirname "$0")/../.." && pwd)
editor=${XZILE_GUI:-src/xzile-gui}
case $editor in
	/*) ;;
	*) editor=$(pwd)/$editor ;;
esac
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
export SDL_VIDEODRIVER

work=$(mktemp -d "${TMPDIR:-/tmp}/xzile-gui-tests.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
${CC:-cc} -shared -fPIC `${PKG_CONFIG:-pkg-config} --cflags sdl2` -o "$work/sdl-inject.so" \
	"$srcdir/tests/gui/sdl-inject.c" -ldl || exit 1
mkdir "$work/home"

# Whether the lines of the hover log $2, without their times, match the
# patterns on the lines of $1.
match_hover () {
	sed 's/^\[[0-9]*\] hover: //' "$2" > "$2.lines" 2>/dev/null
	if [ "$(grep -c '' < "$1")" -ne "$(grep -c '' < "$2.lines")" ] ||
	   ! paste -d '\n' "$1" "$2.lines" | {
		while IFS= read -r pattern && IFS= read -r line; do
			case $line in
				$pattern) ;;
				*) exit 1 ;;
			esac
		done
	   }; then
		diff -u "$1" "$2.lines"
		return 1
	fi
}

run_test () {
	test=${1%.events}
	name=${test##*/}
	input_file=$test.orig-input
	[ -e "$input_file" ] || input_file=$srcdir/tests/test.input
	locales=$(sed -n 's/^# locales: *//p' "$test.events")
	for loc in ${locales:-C}; do
		case $loc in
			*UTF-8 | *utf8)
				if [ "$(LC_ALL=$loc locale charmap 2>/dev/null)" != UTF-8 ]; then
					echo "SKIP: $name [$loc]: no such locale"
					continue
				fi ;;
		esac
		edit_file=$work/$name.txt
		cp "$input_file" "$edit_file" || return 1
		hover_log=
		[ -e "$test.hover" ] && hover_log=$work/$name.hover && rm -f "$hover_log"
		# Every test ends by saving and quitting.
		{ echo "sleep 500"; cat "$test.events"
		  printf 'sleep 300\ndown x ctrl\ndown s ctrl\nsleep 300\ndown x ctrl\ndown c ctrl\n'
		} > "$work/events"
		errors=$(env -u LANG -u LC_CTYPE HOME="$work/home" LC_ALL=$loc \
			XZILE_DEBUG=${hover_log:+hover} XZILE_DEBUG_LOG="$hover_log" \
			XZ_INJECT="$work/events" LD_PRELOAD="$work/sdl-inject.so" \
			timeout 30 "$editor" --no-init-file "$edit_file" 2>&1 >/dev/null)
		status=$?
		if [ $status -ne 0 ]; then
			echo "FAIL: $name [$loc]: exit status $status"
			[ -n "$errors" ] && echo "$errors"
			fail=$((fail + 1))
		elif ! diff -u "$test.output" "$edit_file"; then
			echo "FAIL: $name [$loc]: wrong output"
			fail=$((fail + 1))
		elif [ -n "$hover_log" ] && ! match_hover "$test.hover" "$hover_log"; then
			echo "FAIL: $name [$loc]: wrong links hovered"
			fail=$((fail + 1))
		else
			pass=$((pass + 1))
		fi
	done
}

pass=0
fail=0
if [ $# -eq 0 ]; then
	set -- "$srcdir"/tests/gui/*.events
fi
for t in "$@"; do
	run_test "$t"
done

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
