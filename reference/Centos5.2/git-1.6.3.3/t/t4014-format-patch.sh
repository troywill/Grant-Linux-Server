#!/bin/sh
#
# Copyright (c) 2006 Junio C Hamano
#

test_description='various format-patch tests'

. ./test-lib.sh

test_expect_success setup '

	for i in 1 2 3 4 5 6 7 8 9 10; do echo "$i"; done >file &&
	cat file >elif &&
	git add file elif &&
	git commit -m Initial &&
	git checkout -b side &&

	for i in 1 2 5 6 A B C 7 8 9 10; do echo "$i"; done >file &&
	test_chmod +x elif &&
	git commit -m "Side changes #1" &&

	for i in D E F; do echo "$i"; done >>file &&
	git update-index file &&
	git commit -m "Side changes #2" &&
	git tag C2 &&

	for i in 5 6 1 2 3 A 4 B C 7 8 9 10 D E F; do echo "$i"; done >file &&
	git update-index file &&
	git commit -m "Side changes #3 with \\n backslash-n in it." &&

	git checkout master &&
	git diff-tree -p C2 | git apply --index &&
	git commit -m "Master accepts moral equivalent of #2"

'

test_expect_success "format-patch --ignore-if-in-upstream" '

	git format-patch --stdout master..side >patch0 &&
	cnt=`grep "^From " patch0 | wc -l` &&
	test $cnt = 3

'

test_expect_success "format-patch --ignore-if-in-upstream" '

	git format-patch --stdout \
		--ignore-if-in-upstream master..side >patch1 &&
	cnt=`grep "^From " patch1 | wc -l` &&
	test $cnt = 2

'

test_expect_success "format-patch result applies" '

	git checkout -b rebuild-0 master &&
	git am -3 patch0 &&
	cnt=`git rev-list master.. | wc -l` &&
	test $cnt = 2
'

test_expect_success "format-patch --ignore-if-in-upstream result applies" '

	git checkout -b rebuild-1 master &&
	git am -3 patch1 &&
	cnt=`git rev-list master.. | wc -l` &&
	test $cnt = 2
'

test_expect_success 'commit did not screw up the log message' '

	git cat-file commit side | grep "^Side .* with .* backslash-n"

'

test_expect_success 'format-patch did not screw up the log message' '

	grep "^Subject: .*Side changes #3 with .* backslash-n" patch0 &&
	grep "^Subject: .*Side changes #3 with .* backslash-n" patch1

'

test_expect_success 'replay did not screw up the log message' '

	git cat-file commit rebuild-1 | grep "^Side .* with .* backslash-n"

'

test_expect_success 'extra headers' '

	git config format.headers "To: R. E. Cipient <rcipient@example.com>
" &&
	git config --add format.headers "Cc: S. E. Cipient <scipient@example.com>
" &&
	git format-patch --stdout master..side > patch2 &&
	sed -e "/^$/q" patch2 > hdrs2 &&
	grep "^To: R. E. Cipient <rcipient@example.com>$" hdrs2 &&
	grep "^Cc: S. E. Cipient <scipient@example.com>$" hdrs2

'

test_expect_success 'extra headers without newlines' '

	git config --replace-all format.headers "To: R. E. Cipient <rcipient@example.com>" &&
	git config --add format.headers "Cc: S. E. Cipient <scipient@example.com>" &&
	git format-patch --stdout master..side >patch3 &&
	sed -e "/^$/q" patch3 > hdrs3 &&
	grep "^To: R. E. Cipient <rcipient@example.com>$" hdrs3 &&
	grep "^Cc: S. E. Cipient <scipient@example.com>$" hdrs3

'

test_expect_success 'extra headers with multiple To:s' '

	git config --replace-all format.headers "To: R. E. Cipient <rcipient@example.com>" &&
	git config --add format.headers "To: S. E. Cipient <scipient@example.com>" &&
	git format-patch --stdout master..side > patch4 &&
	sed -e "/^$/q" patch4 > hdrs4 &&
	grep "^To: R. E. Cipient <rcipient@example.com>,$" hdrs4 &&
	grep "^ *S. E. Cipient <scipient@example.com>$" hdrs4
'

test_expect_success 'additional command line cc' '

	git config --replace-all format.headers "Cc: R. E. Cipient <rcipient@example.com>" &&
	git format-patch --cc="S. E. Cipient <scipient@example.com>" --stdout master..side | sed -e "/^$/q" >patch5 &&
	grep "^Cc: R. E. Cipient <rcipient@example.com>,$" patch5 &&
	grep "^ *S. E. Cipient <scipient@example.com>$" patch5
'

test_expect_success 'command line headers' '

	git config --unset-all format.headers &&
	git format-patch --add-header="Cc: R. E. Cipient <rcipient@example.com>" --stdout master..side | sed -e "/^$/q" >patch6 &&
	grep "^Cc: R. E. Cipient <rcipient@example.com>$" patch6
'

test_expect_success 'configuration headers and command line headers' '

	git config --replace-all format.headers "Cc: R. E. Cipient <rcipient@example.com>" &&
	git format-patch --add-header="Cc: S. E. Cipient <scipient@example.com>" --stdout master..side | sed -e "/^$/q" >patch7 &&
	grep "^Cc: R. E. Cipient <rcipient@example.com>,$" patch7 &&
	grep "^ *S. E. Cipient <scipient@example.com>$" patch7
'

test_expect_success 'multiple files' '

	rm -rf patches/ &&
	git checkout side &&
	git format-patch -o patches/ master &&
	ls patches/0001-Side-changes-1.patch patches/0002-Side-changes-2.patch patches/0003-Side-changes-3-with-n-backslash-n-in-it.patch
'

check_threading () {
	expect="$1" &&
	shift &&
	(git format-patch --stdout "$@"; echo $? > status.out) |
	# Prints everything between the Message-ID and In-Reply-To,
	# and replaces all Message-ID-lookalikes by a sequence number
	perl -ne '
		if (/^(message-id|references|in-reply-to)/i) {
			$printing = 1;
		} elsif (/^\S/) {
			$printing = 0;
		}
		if ($printing) {
			$h{$1}=$i++ if (/<([^>]+)>/ and !exists $h{$1});
			for $k (keys %h) {s/$k/$h{$k}/};
			print;
		}
		print "---\n" if /^From /i;
	' > actual &&
	test 0 = "$(cat status.out)" &&
	test_cmp "$expect" actual
}

cat >> expect.no-threading <<EOF
---
---
---
EOF

test_expect_success 'no threading' '
	git checkout side &&
	check_threading expect.no-threading master
'

cat > expect.thread <<EOF
---
Message-Id: <0>
---
Message-Id: <1>
In-Reply-To: <0>
References: <0>
---
Message-Id: <2>
In-Reply-To: <0>
References: <0>
EOF

test_expect_success 'thread' '
	check_threading expect.thread --thread master
'

cat > expect.in-reply-to <<EOF
---
Message-Id: <0>
In-Reply-To: <1>
References: <1>
---
Message-Id: <2>
In-Reply-To: <1>
References: <1>
---
Message-Id: <3>
In-Reply-To: <1>
References: <1>
EOF

test_expect_success 'thread in-reply-to' '
	check_threading expect.in-reply-to --in-reply-to="<test.message>" \
		--thread master
'

cat > expect.cover-letter <<EOF
---
Message-Id: <0>
---
Message-Id: <1>
In-Reply-To: <0>
References: <0>
---
Message-Id: <2>
In-Reply-To: <0>
References: <0>
---
Message-Id: <3>
In-Reply-To: <0>
References: <0>
EOF

test_expect_success 'thread cover-letter' '
	check_threading expect.cover-letter --cover-letter --thread master
'

cat > expect.cl-irt <<EOF
---
Message-Id: <0>
In-Reply-To: <1>
References: <1>
---
Message-Id: <2>
In-Reply-To: <0>
References: <1>
	<0>
---
Message-Id: <3>
In-Reply-To: <0>
References: <1>
	<0>
---
Message-Id: <4>
In-Reply-To: <0>
References: <1>
	<0>
EOF

test_expect_success 'thread cover-letter in-reply-to' '
	check_threading expect.cl-irt --cover-letter \
		--in-reply-to="<test.message>" --thread master
'

test_expect_success 'thread explicit shallow' '
	check_threading expect.cl-irt --cover-letter \
		--in-reply-to="<test.message>" --thread=shallow master
'

cat > expect.deep <<EOF
---
Message-Id: <0>
---
Message-Id: <1>
In-Reply-To: <0>
References: <0>
---
Message-Id: <2>
In-Reply-To: <1>
References: <0>
	<1>
EOF

test_expect_success 'thread deep' '
	check_threading expect.deep --thread=deep master
'

cat > expect.deep-irt <<EOF
---
Message-Id: <0>
In-Reply-To: <1>
References: <1>
---
Message-Id: <2>
In-Reply-To: <0>
References: <1>
	<0>
---
Message-Id: <3>
In-Reply-To: <2>
References: <1>
	<0>
	<2>
EOF

test_expect_success 'thread deep in-reply-to' '
	check_threading expect.deep-irt  --thread=deep \
		--in-reply-to="<test.message>" master
'

cat > expect.deep-cl <<EOF
---
Message-Id: <0>
---
Message-Id: <1>
In-Reply-To: <0>
References: <0>
---
Message-Id: <2>
In-Reply-To: <1>
References: <0>
	<1>
---
Message-Id: <3>
In-Reply-To: <2>
References: <0>
	<1>
	<2>
EOF

test_expect_success 'thread deep cover-letter' '
	check_threading expect.deep-cl --cover-letter --thread=deep master
'

cat > expect.deep-cl-irt <<EOF
---
Message-Id: <0>
In-Reply-To: <1>
References: <1>
---
Message-Id: <2>
In-Reply-To: <0>
References: <1>
	<0>
---
Message-Id: <3>
In-Reply-To: <2>
References: <1>
	<0>
	<2>
---
Message-Id: <4>
In-Reply-To: <3>
References: <1>
	<0>
	<2>
	<3>
EOF

test_expect_success 'thread deep cover-letter in-reply-to' '
	check_threading expect.deep-cl-irt --cover-letter \
		--in-reply-to="<test.message>" --thread=deep master
'

test_expect_success 'thread via config' '
	git config format.thread true &&
	check_threading expect.thread master
'

test_expect_success 'thread deep via config' '
	git config format.thread deep &&
	check_threading expect.deep master
'

test_expect_success 'thread config + override' '
	git config format.thread deep &&
	check_threading expect.thread --thread master
'

test_expect_success 'thread config + --no-thread' '
	git config format.thread deep &&
	check_threading expect.no-threading --no-thread master
'

test_expect_success 'excessive subject' '

	rm -rf patches/ &&
	git checkout side &&
	for i in 5 6 1 2 3 A 4 B C 7 8 9 10 D E F; do echo "$i"; done >>file &&
	git update-index file &&
	git commit -m "This is an excessively long subject line for a message due to the habit some projects have of not having a short, one-line subject at the start of the commit message, but rather sticking a whole paragraph right at the start as the only thing in the commit message. It had better not become the filename for the patch." &&
	git format-patch -o patches/ master..side &&
	ls patches/0004-This-is-an-excessively-long-subject-line-for-a-messa.patch
'

test_expect_success 'cover-letter inherits diff options' '

	git mv file foo &&
	git commit -m foo &&
	git format-patch --cover-letter -1 &&
	! grep "file => foo .* 0 *$" 0000-cover-letter.patch &&
	git format-patch --cover-letter -1 -M &&
	grep "file => foo .* 0 *$" 0000-cover-letter.patch

'

cat > expect << EOF
  This is an excessively long subject line for a message due to the
    habit some projects have of not having a short, one-line subject at
    the start of the commit message, but rather sticking a whole
    paragraph right at the start as the only thing in the commit
    message. It had better not become the filename for the patch.
  foo

EOF

test_expect_success 'shortlog of cover-letter wraps overly-long onelines' '

	git format-patch --cover-letter -2 &&
	sed -e "1,/A U Thor/d" -e "/^$/q" < 0000-cover-letter.patch > output &&
	test_cmp expect output

'

cat > expect << EOF
---
 file |   16 ++++++++++++++++
 1 files changed, 16 insertions(+), 0 deletions(-)

diff --git a/file b/file
index 40f36c6..2dc5c23 100644
--- a/file
+++ b/file
@@ -13,4 +13,20 @@ C
 10
 D
 E
 F
+5
EOF

test_expect_success 'format-patch respects -U' '

	git format-patch -U4 -2 &&
	sed -e "1,/^$/d" -e "/^+5/q" < 0001-This-is-an-excessively-long-subject-line-for-a-messa.patch > output &&
	test_cmp expect output

'

test_expect_success 'format-patch from a subdirectory (1)' '
	filename=$(
		rm -rf sub &&
		mkdir -p sub/dir &&
		cd sub/dir &&
		git format-patch -1
	) &&
	case "$filename" in
	0*)
		;; # ok
	*)
		echo "Oops? $filename"
		false
		;;
	esac &&
	test -f "$filename"
'

test_expect_success 'format-patch from a subdirectory (2)' '
	filename=$(
		rm -rf sub &&
		mkdir -p sub/dir &&
		cd sub/dir &&
		git format-patch -1 -o ..
	) &&
	case "$filename" in
	../0*)
		;; # ok
	*)
		echo "Oops? $filename"
		false
		;;
	esac &&
	basename=$(expr "$filename" : ".*/\(.*\)") &&
	test -f "sub/$basename"
'

test_expect_success 'format-patch from a subdirectory (3)' '
	here="$TEST_DIRECTORY/$test" &&
	rm -f 0* &&
	filename=$(
		rm -rf sub &&
		mkdir -p sub/dir &&
		cd sub/dir &&
		git format-patch -1 -o "$here"
	) &&
	basename=$(expr "$filename" : ".*/\(.*\)") &&
	test -f "$basename"
'

test_expect_success 'format-patch --in-reply-to' '
	git format-patch -1 --stdout --in-reply-to "baz@foo.bar" > patch8 &&
	grep "^In-Reply-To: <baz@foo.bar>" patch8 &&
	grep "^References: <baz@foo.bar>" patch8
'

test_expect_success 'format-patch --signoff' '
	git format-patch -1 --signoff --stdout |
	grep "^Signed-off-by: $GIT_COMMITTER_NAME <$GIT_COMMITTER_EMAIL>"
'

test_done
