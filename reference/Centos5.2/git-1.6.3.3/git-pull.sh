#!/bin/sh
#
# Copyright (c) 2005 Junio C Hamano
#
# Fetch one or more remote refs and merge it/them into the current HEAD.

USAGE='[-n | --no-stat] [--[no-]commit] [--[no-]squash] [--[no-]ff] [-s strategy]... [<fetch-options>] <repo> <head>...'
LONG_USAGE='Fetch one or more remote refs and merge it/them into the current HEAD.'
SUBDIRECTORY_OK=Yes
OPTIONS_SPEC=
. git-sh-setup
set_reflog_action "pull $*"
require_work_tree
cd_to_toplevel

test -z "$(git ls-files -u)" ||
	die "You are in the middle of a conflicted merge."

strategy_args= diffstat= no_commit= squash= no_ff= log_arg= verbosity=
curr_branch=$(git symbolic-ref -q HEAD)
curr_branch_short=$(echo "$curr_branch" | sed "s|refs/heads/||")
rebase=$(git config --bool branch.$curr_branch_short.rebase)
while :
do
	case "$1" in
	-q|--quiet)
		verbosity="$verbosity -q" ;;
	-v|--verbose)
		verbosity="$verbosity -v" ;;
	-n|--no-stat|--no-summary)
		diffstat=--no-stat ;;
	--stat|--summary)
		diffstat=--stat ;;
	--log|--no-log)
		log_arg=$1 ;;
	--no-c|--no-co|--no-com|--no-comm|--no-commi|--no-commit)
		no_commit=--no-commit ;;
	--c|--co|--com|--comm|--commi|--commit)
		no_commit=--commit ;;
	--sq|--squ|--squa|--squas|--squash)
		squash=--squash ;;
	--no-sq|--no-squ|--no-squa|--no-squas|--no-squash)
		squash=--no-squash ;;
	--ff)
		no_ff=--ff ;;
	--no-ff)
		no_ff=--no-ff ;;
	-s=*|--s=*|--st=*|--str=*|--stra=*|--strat=*|--strate=*|\
		--strateg=*|--strategy=*|\
	-s|--s|--st|--str|--stra|--strat|--strate|--strateg|--strategy)
		case "$#,$1" in
		*,*=*)
			strategy=`expr "z$1" : 'z-[^=]*=\(.*\)'` ;;
		1,*)
			usage ;;
		*)
			strategy="$2"
			shift ;;
		esac
		strategy_args="${strategy_args}-s $strategy "
		;;
	-r|--r|--re|--reb|--reba|--rebas|--rebase)
		rebase=true
		;;
	--no-r|--no-re|--no-reb|--no-reba|--no-rebas|--no-rebase)
		rebase=false
		;;
	-h|--h|--he|--hel|--help)
		usage
		;;
	*)
		# Pass thru anything that may be meant for fetch.
		break
		;;
	esac
	shift
done

error_on_no_merge_candidates () {
	exec >&2
	for opt
	do
		case "$opt" in
		-t|--t|--ta|--tag|--tags)
			echo "Fetching tags only, you probably meant:"
			echo "  git fetch --tags"
			exit 1
		esac
	done

	curr_branch=${curr_branch#refs/heads/}

	if [ -z "$curr_branch" ]; then
		echo "You are not currently on a branch, so I cannot use any"
		echo "'branch.<branchname>.merge' in your configuration file."
		echo "Please specify which branch you want to merge on the command"
		echo "line and try again (e.g. 'git pull <repository> <refspec>')."
		echo "See git-pull(1) for details."
	else
		echo "You asked me to pull without telling me which branch you"
		echo "want to merge with, and 'branch.${curr_branch}.merge' in"
		echo "your configuration file does not tell me either.	Please"
		echo "specify which branch you want to merge on the command line and"
		echo "try again (e.g. 'git pull <repository> <refspec>')."
		echo "See git-pull(1) for details."
		echo
		echo "If you often merge with the same branch, you may want to"
		echo "configure the following variables in your configuration"
		echo "file:"
		echo
		echo "    branch.${curr_branch}.remote = <nickname>"
		echo "    branch.${curr_branch}.merge = <remote-ref>"
		echo "    remote.<nickname>.url = <url>"
		echo "    remote.<nickname>.fetch = <refspec>"
		echo
		echo "See git-config(1) for details."
	fi
	exit 1
}

test true = "$rebase" && {
	git update-index --ignore-submodules --refresh &&
	git diff-files --ignore-submodules --quiet &&
	git diff-index --ignore-submodules --cached --quiet HEAD -- ||
	die "refusing to pull with rebase: your working tree is not up-to-date"

	. git-parse-remote &&
	origin="$1"
	test -z "$origin" && origin=$(get_default_remote)
	reflist="$(get_remote_refs_for_fetch "$@" 2>/dev/null |
		sed "s|refs/heads/\(.*\):|\1|")" &&
	oldremoteref="$(git rev-parse -q --verify \
		"refs/remotes/$origin/$reflist")"
}
orig_head=$(git rev-parse -q --verify HEAD)
git fetch $verbosity --update-head-ok "$@" || exit 1

curr_head=$(git rev-parse -q --verify HEAD)
if test -n "$orig_head" && test "$curr_head" != "$orig_head"
then
	# The fetch involved updating the current branch.

	# The working tree and the index file is still based on the
	# $orig_head commit, but we are merging into $curr_head.
	# First update the working tree to match $curr_head.

	echo >&2 "Warning: fetch updated the current branch head."
	echo >&2 "Warning: fast forwarding your working tree from"
	echo >&2 "Warning: commit $orig_head."
	git update-index -q --refresh
	git read-tree -u -m "$orig_head" "$curr_head" ||
		die 'Cannot fast-forward your working tree.
After making sure that you saved anything precious from
$ git diff '$orig_head'
output, run
$ git reset --hard
to recover.'

fi

merge_head=$(sed -e '/	not-for-merge	/d' \
	-e 's/	.*//' "$GIT_DIR"/FETCH_HEAD | \
	tr '\012' ' ')

case "$merge_head" in
'')
	case $? in
	0) error_on_no_merge_candidates "$@";;
	1) echo >&2 "You are not currently on a branch; you must explicitly"
	   echo >&2 "specify which branch you wish to merge:"
	   echo >&2 "  git pull <remote> <branch>"
	   exit 1;;
	*) exit $?;;
	esac
	;;
?*' '?*)
	if test -z "$orig_head"
	then
		die "Cannot merge multiple branches into empty head"
	fi
	if test true = "$rebase"
	then
		die "Cannot rebase onto multiple branches"
	fi
	;;
esac

if test -z "$orig_head"
then
	git update-ref -m "initial pull" HEAD $merge_head "$curr_head" &&
	git read-tree --reset -u HEAD || exit 1
	exit
fi

merge_name=$(git fmt-merge-msg $log_arg <"$GIT_DIR/FETCH_HEAD") || exit
test true = "$rebase" &&
	exec git-rebase $diffstat $strategy_args --onto $merge_head \
	${oldremoteref:-$merge_head}
exec git-merge $diffstat $no_commit $squash $no_ff $log_arg $strategy_args \
	"$merge_name" HEAD $merge_head $verbosity
