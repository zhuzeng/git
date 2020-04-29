#!/bin/sh
#
# Copyright (c) 2020 Jiang Xin
#

test_description='Test proc-receive hook'

. ./test-lib.sh

. "$TEST_DIRECTORY"/t5411/common-functions.sh

# Format the output of git-push, git-show-ref and other commands to make a
# user-friendly and stable text.  In addition to the common format method,
# we also replace the URL of different protocol for the upstream repository
# with a fixed pattern.
make_user_friendly_and_stable_output () {
	make_user_friendly_and_stable_output_common |
		sed -e "s#To ../upstream.git#To <URL/of/upstream.git>#"
}

# Refs of upstream : master(B)  next(A)
# Refs of workbench: master(A)           tags/v123
test_expect_success "setup" '
	git init --bare upstream.git &&
	git init workbench &&
	create_commits_in workbench A B &&
	(
		cd workbench &&
		# Try to make a stable fixed width for abbreviated commit ID,
		# this fixed-width oid will be replaced with "<OID>".
		git config core.abbrev 7 &&
		git remote add origin ../upstream.git &&
		git update-ref refs/heads/master $A &&
		git tag -m "v123" v123 $A &&
		git push origin \
			$B:refs/heads/master \
			$A:refs/heads/next
	) &&
	TAG=$(git -C workbench rev-parse v123) &&

	# setup pre-receive hook
	write_script upstream.git/hooks/pre-receive <<-\EOF &&
	exec >&2
	echo "# pre-receive hook"
	while read old new ref
	do
		echo "pre-receive< $old $new $ref"
	done
	EOF

	# setup post-receive hook
	write_script upstream.git/hooks/post-receive <<-\EOF &&
	exec >&2
	echo "# post-receive hook"
	while read old new ref
	do
		echo "post-receive< $old $new $ref"
	done
	EOF

	upstream=upstream.git
'

# Include test cases for both file and HTTP protocol
. "$TEST_DIRECTORY"/t5411/common-test-cases.sh

test_done
