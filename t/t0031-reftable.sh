#!/bin/sh
#
# Copyright (c) 2020 Google LLC
#

test_description='reftable basics'

. ./test-lib.sh

INVALID_SHA1=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

initialize ()  {
	rm -rf .git &&
	git init --ref-storage=reftable &&
	mv .git/hooks .git/hooks-disabled
}

test_expect_success 'delete ref' '
	initialize &&
	test_commit file &&
	SHA=$(git show-ref -s --verify HEAD) &&
	test_write_lines "$SHA refs/heads/master" "$SHA refs/tags/file" >expect &&
	git show-ref > actual &&
	! git update-ref -d refs/tags/file $INVALID_SHA1 &&
	test_cmp expect actual &&
	git update-ref -d refs/tags/file $SHA  &&
	test_write_lines "$SHA refs/heads/master" >expect &&
	git show-ref > actual &&
	test_cmp expect actual
'

test_expect_success 'basic operation of reftable storage: commit, reflog, repack' '
	initialize &&
	test_commit file &&
	test_write_lines refs/heads/master refs/tags/file >expect &&
	git show-ref &&
	git show-ref | cut -f2 -d" " > actual &&
	test_cmp actual expect &&
	for count in $(test_seq 1 10)
	do
		test_commit "number $count" file.t $count number-$count ||
		return 1
	done &&
	git pack-refs &&
	ls -1 .git/reftable >table-files &&
	test_line_count = 2 table-files &&
	git reflog refs/heads/master >output &&
	test_line_count = 11 output &&
	grep "commit (initial): file" output &&
	grep "commit: number 10" output &&
	git gc &&
	git reflog refs/heads/master >output &&
	test_line_count = 0 output
'

# This matches show-ref's output
print_ref() {
	echo "$(git rev-parse "$1") $1"
}

test_expect_success 'peeled tags are stored' '
	initialize &&
	test_commit file &&
	git tag -m "annotated tag" test_tag HEAD &&
	{
		print_ref "refs/heads/master" &&
		print_ref "refs/tags/file" &&
		print_ref "refs/tags/test_tag" &&
		print_ref "refs/tags/test_tag^{}"
	} >expect &&
	git show-ref -d >actual &&
	test_cmp expect actual
'

test_expect_success 'show-ref works on fresh repo' '
	initialize &&
	rm -rf .git &&
	git init --ref-storage=reftable &&
	>expect &&
	! git show-ref > actual &&
	test_cmp expect actual
'

test_expect_success 'checkout unborn branch' '
	initialize &&
	git checkout -b master
'

test_expect_success 'do not clobber existing repo' '
	rm -rf .git &&
	git init --ref-storage=files &&
	cat .git/HEAD > expect &&
	test_commit file &&
	(git init --ref-storage=reftable || true) &&
	cat .git/HEAD > actual &&
	test_cmp expect actual
'

test_done
