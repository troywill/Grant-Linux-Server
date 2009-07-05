#!/bin/sh
#
# Copyright (c) 2006 Johannes E. Schindelin
#

test_description='git shortlog
'

. ./test-lib.sh

echo 1 > a1
git add a1
tree=$(git write-tree)
commit=$( (echo "Test"; echo) | git commit-tree $tree )
git update-ref HEAD $commit

echo 2 > a1
git commit --quiet -m "This is a very, very long first line for the commit message to see if it is wrapped correctly" a1

# test if the wrapping is still valid when replacing all i's by treble clefs.
echo 3 > a1
git commit --quiet -m "$(echo "This is a very, very long first line for the commit message to see if it is wrapped correctly" | sed "s/i/1234/g" | tr 1234 '\360\235\204\236')" a1

# now fsck up the utf8
git config i18n.commitencoding non-utf-8
echo 4 > a1
git commit --quiet -m "$(echo "This is a very, very long first line for the commit message to see if it is wrapped correctly" | sed "s/i/1234/g" | tr 1234 '\370\235\204\236')" a1

echo 5 > a1
git commit --quiet -m "a								12	34	56	78" a1

git shortlog -w HEAD > out

cat > expect << EOF
A U Thor (5):
      Test
      This is a very, very long first line for the commit message to see if
         it is wrapped correctly
      Th𝄞s 𝄞s a very, very long f𝄞rst l𝄞ne for the comm𝄞t message to see 𝄞f
         𝄞t 𝄞s wrapped correctly
      Th����s ����s a very, very long f����rst l����ne for the comm����t
         message to see ����f ����t ����s wrapped correctly
      a								12	34
         56	78

EOF

test_expect_success 'shortlog wrapping' 'test_cmp expect out'

git log HEAD > log
GIT_DIR=non-existing git shortlog -w < log > out

test_expect_success 'shortlog from non-git directory' 'test_cmp expect out'

test_done
