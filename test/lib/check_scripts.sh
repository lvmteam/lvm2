#!/usr/bin/env bash
# Copyright (C) 2024-2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Static analysis checks for LVM2 test shell scripts.
# Catches common quoting bugs and unnecessary syntax.
# Reports findings and optionally fixes them via sed.
#
# Usage: check_scripts.sh [directory]
#   directory defaults to test/shell

set -euo pipefail

DIR="${1:-test/shell}"
BSLASH_HITS=""
DEV_HITS=""
VAR_HITS=""
BTICK_HITS=""

die() {
	echo "Error: $@" >&2
	exit 2
}

# Shared quote-state walker for check_unquoted_dev and check_unquoted_vars.
# Set LVM_CHECK_QUOTE_MODE to "dev" or "pathid" before piping lines in.
read -r -d '' _QUOTE_CHECK_PERL <<'PERL' || true
	next if /^[^:]+:\d+:\s*#/;
	next if /\\\$/;
	if ($ENV{LVM_CHECK_QUOTE_MODE} eq "dev") {
		next if /\$DM_DEV_DIR/;
		next if /\$\{dev/;
		next if /\\\$dev/;
	}
	my $line = $_;
	my @dq_stack = (0);
	my $in_sq = 0;
	my $found = 0;
	for (my $i = 0; $i < length($line); $i++) {
		my $c = substr($line, $i, 1);
		my $prev = $i > 0 ? substr($line, $i-1, 1) : "";
		if ($in_sq) {
			$in_sq = 0 if $c eq "\x27";
			next;
		}
		if ($c eq "\x27" && !$dq_stack[-1]) {
			$in_sq = 1;
		} elsif ($c eq "\"" && $prev ne "\\") {
			$dq_stack[-1] = !$dq_stack[-1];
		} elsif ($c eq "\$" && substr($line,$i+1,1) eq "(") {
			push @dq_stack, 0;
			$i++;
		} elsif ($c eq ")" && @dq_stack > 1) {
			pop @dq_stack;
		} elsif ($c eq "\$" && !$in_sq && !$dq_stack[-1]) {
			my $rest = substr($line, $i+1);
			my $match = 0;
			if ($ENV{LVM_CHECK_QUOTE_MODE} eq "dev") {
				if ($rest =~ /^(dev\d*)\b/ && $rest !~ /^dev[a-zA-Z_]/) {
					$match = 1;
				}
			} else {
				if ($rest =~ /^(DF|DFDIR|BKDIR|O?PVID\d*|id\d+|idname|idtype)\b/) {
					$match = 1;
				}
			}
			if ($match) {
				if ($prev eq "=") {
					my $before = substr($line, 0, $i);
					$before =~ s/^[^:]+:\d+:\s*//;
					next if $before =~ /^(local\s+|export\s+|declare\s+|typeset\s+|readonly\s+)?[A-Za-z_]\w*=$/;
				}
				$found = 1;
				last;
			}
		}
	}
	print if $found;
PERL

_filter_unquoted_quote_mode() {
	local mode=$1
	shift
	LVM_CHECK_QUOTE_MODE=$mode perl -ne "$_QUOTE_CHECK_PERL"
}

# Check: unnecessary backslash continuation after && or ||
# These operators already imply line continuation in bash.
check_backslash_continuation() {
	BSLASH_HITS=$(grep -rn -P '(&&|\|\|)\s*\\$' "$DIR"/*.sh 2>/dev/null |
		grep -v -P ':\d+:\s*#') || true
	if [[ -n "$BSLASH_HITS" ]]; then
		local count
		count=$(echo "$BSLASH_HITS" | wc -l)
		echo "Unnecessary backslash after && or || ($count):"
		echo "$BSLASH_HITS"
		echo
	fi
}

# Check: unquoted $dev* variables in command arguments
# $vg and $lv names are safe (controlled LVMTEST prefix), but
# $dev* paths must be quoted to handle special characters.
check_unquoted_dev() {
	DEV_HITS=$(grep -rn '$dev' "$DIR"/*.sh 2>/dev/null |
		_filter_unquoted_quote_mode dev) || true
	if [[ -n "$DEV_HITS" ]]; then
		local count
		count=$(echo "$DEV_HITS" | wc -l)
		echo "Unquoted \$dev variables ($count):"
		echo "$DEV_HITS"
		echo
	fi
}

# Check: unquoted path/id variables that should be quoted
# Uses the same quote-state walker as check_unquoted_dev.
check_unquoted_vars() {
	VAR_HITS=$(grep -rn -E '\$(DF|DFDIR|BKDIR|PVID[0-9]*|OPVID[0-9]*|id[0-9]+|idname|idtype)\b' "$DIR"/*.sh 2>/dev/null |
		_filter_unquoted_quote_mode pathid) || true
	if [[ -n "$VAR_HITS" ]]; then
		local count
		count=$(echo "$VAR_HITS" | wc -l)
		echo "Unquoted path/id variables ($count):"
		echo "$VAR_HITS"
		echo
	fi
}

# Check: backtick command substitution instead of $()
check_backticks() {
	BTICK_HITS=$(grep -rn '`' "$DIR"/*.sh 2>/dev/null |
		grep -v -P ':\d+:\s*#') || true
	if [[ -n "$BTICK_HITS" ]]; then
		local count
		count=$(echo "$BTICK_HITS" | wc -l)
		echo "Backtick command substitution instead of \$() ($count):"
		echo "$BTICK_HITS"
		echo
	fi
}

# Fix: remove trailing backslash after && or ||
fix_backslash() {
	local files
	files=$(echo "$BSLASH_HITS" | cut -d: -f1 | sort -u)
	for f in $files; do
		sed -i -E 's/(&&|\|\|)\s*\\$/\1/' "$f"
	done
	echo "Fixed backslash continuations in $(echo "$files" | wc -l) file(s)"
}

_fix_unquoted_on_hits() {
	local mode=$1
	local hits=$2
	local label=$3
	local files f lines

	files=$(echo "$hits" | cut -d: -f1 | sort -u)
	for f in $files; do
		lines=$(echo "$hits" | grep "^${f}:" | cut -d: -f2 | tr '\n' ',' | sed 's/,$//')
		LVM_FIX_QUOTE_MODE=$mode LVM_FIX_QUOTE_LINES=$lines perl -i -pe '
			BEGIN { %fix = map { $_ => 1 } split /,/, $ENV{LVM_FIX_QUOTE_LINES}; }
			if ($fix{$.}) {
				if ($ENV{LVM_FIX_QUOTE_MODE} eq "dev") {
					s/(?<=\s)(\$dev\d*)(?!["\w])/"$1"/g;
					s/,(\$dev\d*)(?!["\w])/,"$1"/g;
					s/(?<=\s)(\w+=)(\$dev\d*)(?!["\w])/$1"$2"/g;
				} else {
					s/(?<=\s)(\$(?:DF|DFDIR|BKDIR|O?PVID\d*|id\d+|idname|idtype)\b)(?!["\w])/"$1"/g;
					s/,(\$(?:DF|DFDIR|BKDIR|O?PVID\d*|id\d+|idname|idtype)\b)(?!["\w])/,"$1"/g;
					s/(?<=\s)(\w+=)(\$(?:DF|DFDIR|BKDIR|O?PVID\d*|id\d+|idname|idtype)\b)(?!["\w])/$1"$2"/g;
				}
			}
		' "$f"
	done
	echo "Fixed $label in $(echo "$files" | wc -l) file(s)"
}

fix_unquoted_dev() {
	_fix_unquoted_on_hits dev "$DEV_HITS" 'unquoted $dev'
}

fix_unquoted_vars() {
	_fix_unquoted_on_hits pathid "$VAR_HITS" 'unquoted path/id vars'
}

# Fix: replace backticks with $() on checker-flagged lines only
fix_backticks() {
	local files
	files=$(echo "$BTICK_HITS" | cut -d: -f1 | sort -u)
	for f in $files; do
		local lines
		lines=$(echo "$BTICK_HITS" | grep "^${f}:" | cut -d: -f2 | tr '\n' ',' | sed 's/,$//')
		perl -i -pe '
			BEGIN { %fix = map { $_ => 1 } split /,/, "'"$lines"'"; }
			if ($fix{$.}) {
				s/`([^`]*)`/\$($1)/g;
			}
		' "$f"
	done
	echo "Fixed backticks in $(echo "$files" | wc -l) file(s)"
}

verify_syntax() {
	local files f err=0
	files=$(echo "$1" | cut -d: -f1 | sort -u)
	for f in $files; do
		if ! bash -n "$f" 2>/dev/null; then
			echo "SYNTAX ERROR in $f after fix!" >&2
			err=1
		fi
	done
	return $err
}

[[ -d "$DIR" ]] || die "Directory '$DIR' not found"

echo "Checking test scripts in $DIR ..."
echo

check_backslash_continuation
check_unquoted_dev
check_unquoted_vars
check_backticks

TOTAL=0
[[ -n "$BSLASH_HITS" ]] && TOTAL=$(( TOTAL + $(echo "$BSLASH_HITS" | wc -l) ))
[[ -n "$DEV_HITS" ]] && TOTAL=$(( TOTAL + $(echo "$DEV_HITS" | wc -l) ))
[[ -n "$VAR_HITS" ]] && TOTAL=$(( TOTAL + $(echo "$VAR_HITS" | wc -l) ))
[[ -n "$BTICK_HITS" ]] && TOTAL=$(( TOTAL + $(echo "$BTICK_HITS" | wc -l) ))

if [[ "$TOTAL" -eq 0 ]]; then
	echo "All checks passed."
	exit 0
fi

echo "$TOTAL issue(s) found."

# Non-interactive mode (piped or redirected) - just report
if [[ ! -t 0 ]]; then
	exit 1
fi

echo
read -r -p "Fix these issues? [y/N] " answer
case "$answer" in
	[yY])
		[[ -n "$BSLASH_HITS" ]] && fix_backslash
		[[ -n "$DEV_HITS" ]] && fix_unquoted_dev
		[[ -n "$VAR_HITS" ]] && fix_unquoted_vars
		[[ -n "$BTICK_HITS" ]] && fix_backticks
		echo
		# Verify syntax of all modified files
		ALL_HITS=""
		for H in "$BSLASH_HITS" "$DEV_HITS" "$VAR_HITS" "$BTICK_HITS"; do
			[[ -n "$H" ]] || continue
			[[ -n "$ALL_HITS" ]] && ALL_HITS="$ALL_HITS"$'\n'"$H" || ALL_HITS="$H"
		done
		if verify_syntax "$ALL_HITS"; then
			echo "All modified files pass bash -n syntax check."
		else
			echo "Some files have syntax errors - review changes!" >&2
			exit 2
		fi
		;;
	*)
		echo "No changes made."
		exit 1
		;;
esac
