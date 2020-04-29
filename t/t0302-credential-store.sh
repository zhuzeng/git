#!/bin/sh

test_description='credential-store tests'
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-credential.sh

helper_test store

test_expect_success 'when xdg file does not exist, xdg file not created' '
	test_path_is_missing "$HOME/.config/git/credentials" &&
	test -s "$HOME/.git-credentials"
'

test_expect_success 'setup xdg file' '
	rm -f "$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	>"$HOME/.config/git/credentials"
'

helper_test store

test_expect_success 'when xdg file exists, home file not created' '
	test -s "$HOME/.config/git/credentials" &&
	test_path_is_missing "$HOME/.git-credentials"
'

test_expect_success 'setup custom xdg file' '
	rm -f "$HOME/.git-credentials" &&
	rm -f "$HOME/.config/git/credentials" &&
	mkdir -p "$HOME/xdg/git" &&
	>"$HOME/xdg/git/credentials"
'

XDG_CONFIG_HOME="$HOME/xdg"
export XDG_CONFIG_HOME
helper_test store
unset XDG_CONFIG_HOME

test_expect_success 'if custom xdg file exists, home and xdg files not created' '
	test_when_finished "rm -f \"$HOME/xdg/git/credentials\"" &&
	test -s "$HOME/xdg/git/credentials" &&
	test_path_is_missing "$HOME/.git-credentials" &&
	test_path_is_missing "$HOME/.config/git/credentials"
'

test_expect_success 'get: use home file if both home and xdg files have matches' '
	echo "https://home-user:home-pass@example.com" >"$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	echo "https://xdg-user:xdg-pass@example.com" >"$HOME/.config/git/credentials" &&
	check fill store <<-\EOF
	protocol=https
	host=example.com
	--
	protocol=https
	host=example.com
	username=home-user
	password=home-pass
	--
	EOF
'

test_expect_success 'get: use xdg file if home file has no matches' '
	>"$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	echo "https://xdg-user:xdg-pass@example.com" >"$HOME/.config/git/credentials" &&
	check fill store <<-\EOF
	protocol=https
	host=example.com
	--
	protocol=https
	host=example.com
	username=xdg-user
	password=xdg-pass
	--
	EOF
'

test_expect_success POSIXPERM,SANITY 'get: use xdg file if home file is unreadable' '
	echo "https://home-user:home-pass@example.com" >"$HOME/.git-credentials" &&
	chmod -r "$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	echo "https://xdg-user:xdg-pass@example.com" >"$HOME/.config/git/credentials" &&
	check fill store <<-\EOF
	protocol=https
	host=example.com
	--
	protocol=https
	host=example.com
	username=xdg-user
	password=xdg-pass
	--
	EOF
'

test_expect_success 'store: if both xdg and home files exist, only store in home file' '
	>"$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	>"$HOME/.config/git/credentials" &&
	check approve store <<-\EOF &&
	protocol=https
	host=example.com
	username=store-user
	password=store-pass
	EOF
	echo "https://store-user:store-pass@example.com" >expected &&
	test_cmp expected "$HOME/.git-credentials" &&
	test_must_be_empty "$HOME/.config/git/credentials"
'


test_expect_success 'erase: erase matching credentials from both xdg and home files' '
	echo "https://home-user:home-pass@example.com" >"$HOME/.git-credentials" &&
	mkdir -p "$HOME/.config/git" &&
	echo "https://xdg-user:xdg-pass@example.com" >"$HOME/.config/git/credentials" &&
	check reject store <<-\EOF &&
	protocol=https
	host=example.com
	EOF
	test_must_be_empty "$HOME/.git-credentials" &&
	test_must_be_empty "$HOME/.config/git/credentials"
'

test_expect_success 'get: credentials without scheme are invalid' '
	echo "://user:secret@example.com" >"$HOME/.git-credentials" &&
	cat >expect-stdout <<-\STDOUT &&
	protocol=https
	host=example.com
	username=askpass-username
	password=askpass-password
	STDOUT
	test_config credential.helper store &&
	git credential fill <<-\EOF >stdout 2>stderr &&
	protocol=https
	host=example.com
	EOF
	test_cmp expect-stdout stdout &&
	grep "askpass: Username for '\''https://example.com'\'':" stderr &&
	grep "askpass: Password for '\''https://askpass-username@example.com'\'':" stderr &&
	test_i18ngrep "ignoring invalid credential" stderr &&
	! grep secret stderr
'

test_expect_success 'get: store file can contain empty/bogus lines' '
	echo "" > "$HOME/.git-credentials" &&
	q_to_tab <<-\CONFIG >>"$HOME/.git-credentials" &&
	#comment
	Q
	https://user:pass@example.com
	CONFIG
	cat >expect-stdout <<-\STDOUT &&
	protocol=https
	host=example.com
	username=user
	password=pass
	STDOUT
	test_config credential.helper store &&
	git credential fill <<-\EOF >stdout 2>stderr &&
	protocol=https
	host=example.com
	EOF
	test_cmp expect-stdout stdout &&
	test_i18ngrep "ignoring invalid credential" stderr &&
	test_line_count = 3 stderr
'

test_done
