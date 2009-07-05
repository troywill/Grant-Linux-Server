#!/bin/sh
#
# Copyright (c) 2006 Junio C Hamano
#

test_description='commit and log output encodings'

. ./test-lib.sh

compare_with () {
	git show -s $1 | sed -e '1,/^$/d' -e 's/^    //' >current &&
	test_cmp current "$2"
}

test_expect_success setup '
	: >F &&
	git add F &&
	T=$(git write-tree) &&
	C=$(git commit-tree $T <"$TEST_DIRECTORY"/t3900/1-UTF-8.txt) &&
	git update-ref HEAD $C &&
	git tag C0
'

test_expect_success 'no encoding header for base case' '
	E=$(git cat-file commit C0 | sed -ne "s/^encoding //p") &&
	test z = "z$E"
'

for H in ISO-8859-1 EUCJP ISO-2022-JP
do
	test_expect_success "$H setup" '
		git config i18n.commitencoding $H &&
		git checkout -b $H C0 &&
		echo $H >F &&
		git commit -a -F "$TEST_DIRECTORY"/t3900/$H.txt
	'
done

for H in ISO-8859-1 EUCJP ISO-2022-JP
do
	test_expect_success "check encoding header for $H" '
		E=$(git cat-file commit '$H' | sed -ne "s/^encoding //p") &&
		test "z$E" = "z'$H'"
	'
done

test_expect_success 'config to remove customization' '
	git config --unset-all i18n.commitencoding &&
	if Z=$(git config --get-all i18n.commitencoding)
	then
		echo Oops, should have failed.
		false
	else
		test z = "z$Z"
	fi &&
	git config i18n.commitencoding utf-8
'

test_expect_success 'ISO-8859-1 should be shown in UTF-8 now' '
	compare_with ISO-8859-1 "$TEST_DIRECTORY"/t3900/1-UTF-8.txt
'

for H in EUCJP ISO-2022-JP
do
	test_expect_success "$H should be shown in UTF-8 now" '
		compare_with '$H' "$TEST_DIRECTORY"/t3900/2-UTF-8.txt
	'
done

test_expect_success 'config to add customization' '
	git config --unset-all i18n.commitencoding &&
	if Z=$(git config --get-all i18n.commitencoding)
	then
		echo Oops, should have failed.
		false
	else
		test z = "z$Z"
	fi
'

for H in ISO-8859-1 EUCJP ISO-2022-JP
do
	test_expect_success "$H should be shown in itself now" '
		git config i18n.commitencoding '$H' &&
		compare_with '$H' "$TEST_DIRECTORY"/t3900/'$H'.txt
	'
done

test_expect_success 'config to tweak customization' '
	git config i18n.logoutputencoding utf-8
'

test_expect_success 'ISO-8859-1 should be shown in UTF-8 now' '
	compare_with ISO-8859-1 "$TEST_DIRECTORY"/t3900/1-UTF-8.txt
'

for H in EUCJP ISO-2022-JP
do
	test_expect_success "$H should be shown in UTF-8 now" '
		compare_with '$H' "$TEST_DIRECTORY"/t3900/2-UTF-8.txt
	'
done

for J in EUCJP ISO-2022-JP
do
	git config i18n.logoutputencoding $J
	for H in EUCJP ISO-2022-JP
	do
		test_expect_success "$H should be shown in $J now" '
			compare_with '$H' "$TEST_DIRECTORY"/t3900/'$J'.txt
		'
	done
done

for H in ISO-8859-1 EUCJP ISO-2022-JP
do
	test_expect_success "No conversion with $H" '
		compare_with "--encoding=none '$H'" "$TEST_DIRECTORY"/t3900/'$H'.txt
	'
done

test_done
