#!/bin/sh

test_description='Test cloning a repository larger than 2 gigabyte'
. ./test-lib.sh

test -z "$GIT_TEST_CLONE_2GB" &&
say "Skipping expensive 2GB clone test; enable it with GIT_TEST_CLONE_2GB=t" &&
test_done &&
exit

test_expect_success 'setup' '

	git config pack.compression 0 &&
	git config pack.depth 0 &&
	blobsize=$((20*1024*1024)) &&
	blobcount=$((2*1024*1024*1024/$blobsize+1)) &&
	i=1 &&
	(while test $i -le $blobcount
	 do
		printf "Generating blob $i/$blobcount\r" >&2 &&
		printf "blob\nmark :$i\ndata $blobsize\n" &&
		#test-genrandom $i $blobsize &&
		printf "%-${blobsize}s" $i &&
		echo "M 100644 :$i $i" >> commit
		i=$(($i+1)) ||
		echo $? > exit-status
	 done &&
	 echo "commit refs/heads/master" &&
	 echo "author A U Thor <author@email.com> 123456789 +0000" &&
	 echo "committer C O Mitter <committer@email.com> 123456789 +0000" &&
	 echo "data 5" &&
	 echo ">2gb" &&
	 cat commit) |
	git fast-import &&
	test ! -f exit-status

'

test_expect_success 'clone' '

	git clone --bare --no-hardlinks . clone

'

test_done
