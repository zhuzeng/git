#!/bin/sh
#
# Copyright (c) 2019 Rohit Ashiwal
#

test_description='tests to ensure compatibility between am and interactive backends'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-rebase.sh

# This is a special case in which both am and interactive backends
# provide the same output. It was done intentionally because
# both the backends fall short of optimal behaviour.
test_expect_success 'setup' '
	git checkout -b topic &&
	q_to_tab >file <<-\EOF &&
	line 1
	Qline 2
	line 3
	EOF
	git add file &&
	git commit -m "add file" &&
	cat >file <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	git commit -am "update file" &&
	git tag side &&

	git checkout --orphan master &&
	sed -e "s/^|//" >file <<-\EOF &&
	|line 1
	|        line 2
	|line 3
	EOF
	git add file &&
	git commit -m "add file" &&
	git tag main
'

test_expect_success '--ignore-whitespace works with apply backend' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	test_must_fail git rebase --apply main side &&
	git rebase --abort &&
	git rebase --apply --ignore-whitespace main side &&
	test_cmp expect file
'

test_expect_success '--ignore-whitespace works with merge backend' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	test_must_fail git rebase --merge main side &&
	git rebase --abort &&
	git rebase --merge --ignore-whitespace main side &&
	test_cmp expect file
'

test_expect_success '--ignore-whitespace is remembered when continuing' '
	cat >expect <<-\EOF &&
	line 1
	new line 2
	line 3
	EOF
	(
		set_fake_editor &&
		FAKE_LINES="break 1" git rebase -i --ignore-whitespace main side
	) &&
	git rebase --continue &&
	test_cmp expect file
'

# This must be the last test in this file
test_expect_success '$EDITOR and friends are unchanged' '
	test_editor_unchanged
'

test_done
