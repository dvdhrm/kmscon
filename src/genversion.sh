#!/bin/sh

#
# Generate $1 with:
#   const char shl_git_head[] = "<git-head-revision>";
# But do not touch $1 if the git-revision is already up-to-date.
#

if test "x$1" = "x" ; then
	echo "usage: ./genversion <file>"
	exit 1
fi

#
# Check whether this is a valid git repository.
# Set ISGIT to 1=true or 0=false.
#

ISGIT=0
REV=`git rev-parse --git-dir 2>/dev/null`
if test "x$?" = "x0" ; then
	ISGIT=1
fi

#
# Check the old revision from $1.
#

if test -f "$1" ; then
	OLDREV=`cat "$1"`
else
	if test $ISGIT = 0 ; then
		echo "WARNING: version file $1 is missing"
		echo "const char shl_git_head[] = \"UnknownRevision\";" >"$1"
		exit 0
	fi

	OLDREV=""
fi

#
# Check new revision from "git describe". However, if this is no valid
# git-repository, return success and do nothing.
#

if test $ISGIT = 0 ; then
	exit 0
fi

NEWREV=`git describe`
NEWREV="const char shl_git_head[] = \"$NEWREV\";"

#
# Exit if the file is already up to date.
# Otherwise, write the new revision into the file.
#

if test "x$OLDREV" = "x$NEWREV" ; then
	exit 0
fi

echo "$NEWREV" >"$1"
