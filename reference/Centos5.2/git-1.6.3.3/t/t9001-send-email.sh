#!/bin/sh

test_description='git send-email'
. ./test-lib.sh

if ! test_have_prereq PERL; then
	say 'skipping git send-email tests, perl not available'
	test_done
fi

PROG='git send-email'
test_expect_success \
    'prepare reference tree' \
    'echo "1A quick brown fox jumps over the" >file &&
     echo "lazy dog" >>file &&
     git add file &&
     GIT_AUTHOR_NAME="A" git commit -a -m "Initial."'

test_expect_success \
    'Setup helper tool' \
    '(echo "#!$SHELL_PATH"
      echo shift
      echo output=1
      echo "while test -f commandline\$output; do output=\$((\$output+1)); done"
      echo for a
      echo do
      echo "  echo \"!\$a!\""
      echo "done >commandline\$output"
      echo "cat > msgtxt\$output"
      ) >fake.sendmail &&
     chmod +x ./fake.sendmail &&
     git add fake.sendmail &&
     GIT_AUTHOR_NAME="A" git commit -a -m "Second."'

clean_fake_sendmail() {
	rm -f commandline* msgtxt*
}

test_expect_success 'Extract patches' '
    patches=`git format-patch -s --cc="One <one@example.com>" --cc=two@example.com -n HEAD^1`
'

# Test no confirm early to ensure remaining tests will not hang
test_no_confirm () {
	rm -f no_confirm_okay
	echo n | \
		GIT_SEND_EMAIL_NOTTY=1 \
		git send-email \
		--from="Example <from@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		$@ \
		$patches > stdout &&
		test_must_fail grep "Send this email" stdout &&
		> no_confirm_okay
}

# Exit immediately to prevent hang if a no-confirm test fails
check_no_confirm () {
	test -f no_confirm_okay || {
		say 'No confirm test failed; skipping remaining tests to prevent hanging'
		test_done
	}
}

test_expect_success 'No confirm with --suppress-cc' '
	test_no_confirm --suppress-cc=sob
'
check_no_confirm

test_expect_success 'No confirm with --confirm=never' '
	test_no_confirm --confirm=never
'
check_no_confirm

# leave sendemail.confirm set to never after this so that none of the
# remaining tests prompt unintentionally.
test_expect_success 'No confirm with sendemail.confirm=never' '
	git config sendemail.confirm never &&
	test_no_confirm --compose --subject=foo
'
check_no_confirm

test_expect_success 'Send patches' '
     git send-email --suppress-cc=sob --from="Example <nobody@example.com>" --to=nobody@example.com --smtp-server="$(pwd)/fake.sendmail" $patches 2>errors
'

cat >expected <<\EOF
!nobody@example.com!
!author@example.com!
!one@example.com!
!two@example.com!
EOF
test_expect_success \
    'Verify commandline' \
    'test_cmp expected commandline1'

cat >expected-show-all-headers <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<cc@example.com>,<author@example.com>,<one@example.com>,<two@example.com>,<bcc@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: cc@example.com, A <author@example.com>, One <one@example.com>, two@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING
In-Reply-To: <unique-message-id@example.com>
References: <unique-message-id@example.com>

Result: OK
EOF

test_expect_success 'Show all headers' '
	git send-email \
		--dry-run \
		--suppress-cc=sob \
		--from="Example <from@example.com>" \
		--to=to@example.com \
		--cc=cc@example.com \
		--bcc=bcc@example.com \
		--in-reply-to="<unique-message-id@example.com>" \
		--smtp-server relay.example.com \
		$patches |
	sed	-e "s/^\(Date:\).*/\1 DATE-STRING/" \
		-e "s/^\(Message-Id:\).*/\1 MESSAGE-ID-STRING/" \
		-e "s/^\(X-Mailer:\).*/\1 X-MAILER-STRING/" \
		>actual-show-all-headers &&
	test_cmp expected-show-all-headers actual-show-all-headers
'

test_expect_success 'Prompting works' '
	clean_fake_sendmail &&
	(echo "Example <from@example.com>"
	 echo "to@example.com"
	 echo ""
	) | GIT_SEND_EMAIL_NOTTY=1 git send-email \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches \
		2>errors &&
		grep "^From: Example <from@example.com>$" msgtxt1 &&
		grep "^To: to@example.com$" msgtxt1
'

z8=zzzzzzzz
z64=$z8$z8$z8$z8$z8$z8$z8$z8
z512=$z64$z64$z64$z64$z64$z64$z64$z64
test_expect_success 'reject long lines' '
	clean_fake_sendmail &&
	cp $patches longline.patch &&
	echo $z512$z512 >>longline.patch &&
	test_must_fail git send-email \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches longline.patch \
		2>errors &&
	grep longline.patch errors
'

test_expect_success 'no patch was sent' '
	! test -e commandline1
'

test_expect_success 'Author From: in message body' '
	clean_fake_sendmail &&
	git send-email \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches &&
	sed "1,/^$/d" < msgtxt1 > msgbody1
	grep "From: A <author@example.com>" msgbody1
'

test_expect_success 'Author From: not in message body' '
	clean_fake_sendmail &&
	git send-email \
		--from="A <author@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches &&
	sed "1,/^$/d" < msgtxt1 > msgbody1
	! grep "From: A <author@example.com>" msgbody1
'

test_expect_success 'allow long lines with --no-validate' '
	git send-email \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		--novalidate \
		$patches longline.patch \
		2>errors
'

test_expect_success 'Invalid In-Reply-To' '
	clean_fake_sendmail &&
	git send-email \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--in-reply-to=" " \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches
		2>errors
	! grep "^In-Reply-To: < *>" msgtxt1
'

test_expect_success 'Valid In-Reply-To when prompting' '
	clean_fake_sendmail &&
	(echo "From Example <from@example.com>"
	 echo "To Example <to@example.com>"
	 echo ""
	) | env GIT_SEND_EMAIL_NOTTY=1 git send-email \
		--smtp-server="$(pwd)/fake.sendmail" \
		$patches 2>errors &&
	! grep "^In-Reply-To: < *>" msgtxt1
'

test_expect_success 'setup fake editor' '
	(echo "#!$SHELL_PATH" &&
	 echo "echo fake edit >>\"\$1\""
	) >fake-editor &&
	chmod +x fake-editor
'

test_set_editor "$(pwd)/fake-editor"

test_expect_success '--compose works' '
	clean_fake_sendmail &&
	git send-email \
	--compose --subject foo \
	--from="Example <nobody@example.com>" \
	--to=nobody@example.com \
	--smtp-server="$(pwd)/fake.sendmail" \
	$patches \
	2>errors
'

test_expect_success 'first message is compose text' '
	grep "^fake edit" msgtxt1
'

test_expect_success 'second message is patch' '
	grep "Subject:.*Second" msgtxt2
'

cat >expected-suppress-sob <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<cc@example.com>,<author@example.com>,<one@example.com>,<two@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: cc@example.com, A <author@example.com>, One <one@example.com>, two@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_suppression () {
	git send-email \
		--dry-run \
		--suppress-cc=$1 \
		--from="Example <from@example.com>" \
		--to=to@example.com \
		--smtp-server relay.example.com \
		$patches |
	sed	-e "s/^\(Date:\).*/\1 DATE-STRING/" \
		-e "s/^\(Message-Id:\).*/\1 MESSAGE-ID-STRING/" \
		-e "s/^\(X-Mailer:\).*/\1 X-MAILER-STRING/" \
		>actual-suppress-$1 &&
	test_cmp expected-suppress-$1 actual-suppress-$1
}

test_expect_success 'sendemail.cc set' '
	git config sendemail.cc cc@example.com &&
	test_suppression sob
'

cat >expected-suppress-sob <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<author@example.com>,<one@example.com>,<two@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: A <author@example.com>, One <one@example.com>, two@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success 'sendemail.cc unset' '
	git config --unset sendemail.cc &&
	test_suppression sob
'

cat >expected-suppress-all <<\EOF
0001-Second.patch
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>
From: Example <from@example.com>
To: to@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success '--suppress-cc=all' '
	test_suppression all
'

cat >expected-suppress-body <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<author@example.com>,<one@example.com>,<two@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: A <author@example.com>, One <one@example.com>, two@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success '--suppress-cc=body' '
	test_suppression body
'

cat >expected-suppress-sob <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<author@example.com>,<one@example.com>,<two@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: A <author@example.com>, One <one@example.com>, two@example.com
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success '--suppress-cc=sob' '
	test_suppression sob
'

cat >expected-suppress-bodycc <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(mbox) Adding cc: One <one@example.com> from line 'Cc: One <one@example.com>, two@example.com'
(mbox) Adding cc: two@example.com from line 'Cc: One <one@example.com>, two@example.com'
(body) Adding cc: C O Mitter <committer@example.com> from line 'Signed-off-by: C O Mitter <committer@example.com>'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<author@example.com>,<one@example.com>,<two@example.com>,<committer@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: A <author@example.com>, One <one@example.com>, two@example.com, C O Mitter <committer@example.com>
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success '--suppress-cc=bodycc' '
	test_suppression bodycc
'

cat >expected-suppress-cc <<\EOF
0001-Second.patch
(mbox) Adding cc: A <author@example.com> from line 'From: A <author@example.com>'
(body) Adding cc: C O Mitter <committer@example.com> from line 'Signed-off-by: C O Mitter <committer@example.com>'
Dry-OK. Log says:
Server: relay.example.com
MAIL FROM:<from@example.com>
RCPT TO:<to@example.com>,<author@example.com>,<committer@example.com>
From: Example <from@example.com>
To: to@example.com
Cc: A <author@example.com>, C O Mitter <committer@example.com>
Subject: [PATCH 1/1] Second.
Date: DATE-STRING
Message-Id: MESSAGE-ID-STRING
X-Mailer: X-MAILER-STRING

Result: OK
EOF

test_expect_success '--suppress-cc=cc' '
	test_suppression cc
'

test_confirm () {
	echo y | \
		GIT_SEND_EMAIL_NOTTY=1 \
		git send-email \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--smtp-server="$(pwd)/fake.sendmail" \
		$@ $patches > stdout &&
	grep "Send this email" stdout
}

test_expect_success '--confirm=always' '
	test_confirm --confirm=always --suppress-cc=all
'

test_expect_success '--confirm=auto' '
	test_confirm --confirm=auto
'

test_expect_success '--confirm=cc' '
	test_confirm --confirm=cc
'

test_expect_success '--confirm=compose' '
	test_confirm --confirm=compose --compose
'

test_expect_success 'confirm by default (due to cc)' '
	CONFIRM=$(git config --get sendemail.confirm) &&
	git config --unset sendemail.confirm &&
	test_confirm
	ret="$?"
	git config sendemail.confirm ${CONFIRM:-never}
	test $ret = "0"
'

test_expect_success 'confirm by default (due to --compose)' '
	CONFIRM=$(git config --get sendemail.confirm) &&
	git config --unset sendemail.confirm &&
	test_confirm --suppress-cc=all --compose
	ret="$?"
	git config sendemail.confirm ${CONFIRM:-never}
	test $ret = "0"
'

test_expect_success 'confirm detects EOF (inform assumes y)' '
	CONFIRM=$(git config --get sendemail.confirm) &&
	git config --unset sendemail.confirm &&
	rm -fr outdir &&
	git format-patch -2 -o outdir &&
	GIT_SEND_EMAIL_NOTTY=1 \
		git send-email \
			--from="Example <nobody@example.com>" \
			--to=nobody@example.com \
			--smtp-server="$(pwd)/fake.sendmail" \
			outdir/*.patch < /dev/null
	ret="$?"
	git config sendemail.confirm ${CONFIRM:-never}
	test $ret = "0"
'

test_expect_success 'confirm detects EOF (auto causes failure)' '
	CONFIRM=$(git config --get sendemail.confirm) &&
	git config sendemail.confirm auto &&
	GIT_SEND_EMAIL_NOTTY=1 &&
	export GIT_SEND_EMAIL_NOTTY &&
		test_must_fail git send-email \
			--from="Example <nobody@example.com>" \
			--to=nobody@example.com \
			--smtp-server="$(pwd)/fake.sendmail" \
			$patches < /dev/null
	ret="$?"
	git config sendemail.confirm ${CONFIRM:-never}
	test $ret = "0"
'

test_expect_success 'confirm doesnt loop forever' '
	CONFIRM=$(git config --get sendemail.confirm) &&
	git config sendemail.confirm auto &&
	GIT_SEND_EMAIL_NOTTY=1 &&
	export GIT_SEND_EMAIL_NOTTY &&
		yes "bogus" | test_must_fail git send-email \
			--from="Example <nobody@example.com>" \
			--to=nobody@example.com \
			--smtp-server="$(pwd)/fake.sendmail" \
			$patches
	ret="$?"
	git config sendemail.confirm ${CONFIRM:-never}
	test $ret = "0"
'

test_expect_success 'utf8 Cc is rfc2047 encoded' '
	clean_fake_sendmail &&
	rm -fr outdir &&
	git format-patch -1 -o outdir --cc="àéìöú <utf8@example.com>" &&
	git send-email \
	--from="Example <nobody@example.com>" \
	--to=nobody@example.com \
	--smtp-server="$(pwd)/fake.sendmail" \
	outdir/*.patch &&
	grep "^Cc:" msgtxt1 |
	grep "=?utf-8?q?=C3=A0=C3=A9=C3=AC=C3=B6=C3=BA?= <utf8@example.com>"
'

test_expect_success '--compose adds MIME for utf8 body' '
	clean_fake_sendmail &&
	(echo "#!$SHELL_PATH" &&
	 echo "echo utf8 body: àéìöú >>\"\$1\""
	) >fake-editor-utf8 &&
	chmod +x fake-editor-utf8 &&
	  GIT_EDITOR="\"$(pwd)/fake-editor-utf8\"" \
	  git send-email \
	  --compose --subject foo \
	  --from="Example <nobody@example.com>" \
	  --to=nobody@example.com \
	  --smtp-server="$(pwd)/fake.sendmail" \
	  $patches &&
	grep "^utf8 body" msgtxt1 &&
	grep "^Content-Type: text/plain; charset=utf-8" msgtxt1
'

test_expect_success '--compose respects user mime type' '
	clean_fake_sendmail &&
	(echo "#!$SHELL_PATH" &&
	 echo "(echo MIME-Version: 1.0"
	 echo " echo Content-Type: text/plain\\; charset=iso-8859-1"
	 echo " echo Content-Transfer-Encoding: 8bit"
	 echo " echo Subject: foo"
	 echo " echo "
	 echo " echo utf8 body: àéìöú) >\"\$1\""
	) >fake-editor-utf8-mime &&
	chmod +x fake-editor-utf8-mime &&
	  GIT_EDITOR="\"$(pwd)/fake-editor-utf8-mime\"" \
	  git send-email \
	  --compose --subject foo \
	  --from="Example <nobody@example.com>" \
	  --to=nobody@example.com \
	  --smtp-server="$(pwd)/fake.sendmail" \
	  $patches &&
	grep "^utf8 body" msgtxt1 &&
	grep "^Content-Type: text/plain; charset=iso-8859-1" msgtxt1 &&
	! grep "^Content-Type: text/plain; charset=utf-8" msgtxt1
'

test_expect_success '--compose adds MIME for utf8 subject' '
	clean_fake_sendmail &&
	  GIT_EDITOR="\"$(pwd)/fake-editor\"" \
	  git send-email \
	  --compose --subject utf8-sübjëct \
	  --from="Example <nobody@example.com>" \
	  --to=nobody@example.com \
	  --smtp-server="$(pwd)/fake.sendmail" \
	  $patches &&
	grep "^fake edit" msgtxt1 &&
	grep "^Subject: =?utf-8?q?utf8-s=C3=BCbj=C3=ABct?=" msgtxt1
'

test_expect_success 'detects ambiguous reference/file conflict' '
	echo master > master &&
	git add master &&
	git commit -m"add master" &&
	test_must_fail git send-email --dry-run master 2>errors &&
	grep disambiguate errors
'

test_expect_success 'feed two files' '
	rm -fr outdir &&
	git format-patch -2 -o outdir &&
	git send-email \
	--dry-run \
	--from="Example <nobody@example.com>" \
	--to=nobody@example.com \
	outdir/000?-*.patch 2>errors >out &&
	grep "^Subject: " out >subjects &&
	test "z$(sed -n -e 1p subjects)" = "zSubject: [PATCH 1/2] Second." &&
	test "z$(sed -n -e 2p subjects)" = "zSubject: [PATCH 2/2] add master"
'

test_expect_success 'in-reply-to but no threading' '
	git send-email \
		--dry-run \
		--from="Example <nobody@example.com>" \
		--to=nobody@example.com \
		--in-reply-to="<in-reply-id@example.com>" \
		--nothread \
		$patches |
	grep "In-Reply-To: <in-reply-id@example.com>"
'

test_done
