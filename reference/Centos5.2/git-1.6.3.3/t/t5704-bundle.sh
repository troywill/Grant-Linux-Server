#!/bin/sh

test_description='some bundle related tests'
. ./test-lib.sh

test_expect_success 'setup' '

	: > file &&
	git add file &&
	test_tick &&
	git commit -m initial &&
	test_tick &&
	git tag -m tag tag &&
	: > file2 &&
	git add file2 &&
	: > file3 &&
	test_tick &&
	git commit -m second &&
	git add file3 &&
	test_tick &&
	git commit -m third

'

test_expect_success 'tags can be excluded by rev-list options' '

	git bundle create bundle --all --since=7.Apr.2005.15:16:00.-0700 &&
	git ls-remote bundle > output &&
	! grep tag output

'

test_done
