. ./test-lib.sh

remotes_git_svn=remotes/git""-svn
git_svn_id=git""-svn-id

if test -n "$NO_SVN_TESTS"
then
	say 'skipping git svn tests, NO_SVN_TESTS defined'
	test_done
fi
if ! test_have_prereq PERL; then
	say 'skipping git svn tests, perl not available'
	test_done
fi

GIT_DIR=$PWD/.git
GIT_SVN_DIR=$GIT_DIR/svn/git-svn
SVN_TREE=$GIT_SVN_DIR/svn-tree

svn >/dev/null 2>&1
if test $? -ne 1
then
    say 'skipping git svn tests, svn not found'
    test_done
fi

svnrepo=$PWD/svnrepo
export svnrepo

perl -w -e "
use SVN::Core;
use SVN::Repos;
\$SVN::Core::VERSION gt '1.1.0' or exit(42);
system(qw/svnadmin create --fs-type fsfs/, \$ENV{svnrepo}) == 0 or exit(41);
" >&3 2>&4
x=$?
if test $x -ne 0
then
	if test $x -eq 42; then
		err='Perl SVN libraries must be >= 1.1.0'
	elif test $x -eq 41; then
		err='svnadmin failed to create fsfs repository'
	else
		err='Perl SVN libraries not found or unusable, skipping test'
	fi
	say "$err"
	test_done
fi

rawsvnrepo="$svnrepo"
svnrepo="file://$svnrepo"

poke() {
	test-chmtime +1 "$1"
}

for d in \
	"$SVN_HTTPD_PATH" \
	/usr/sbin/apache2 \
	/usr/sbin/httpd \
; do
	if test -f "$d"
	then
		SVN_HTTPD_PATH="$d"
		break
	fi
done
for d in \
	"$SVN_HTTPD_MODULE_PATH" \
	/usr/lib/apache2/modules \
	/usr/libexec/apache2 \
; do
	if test -d "$d"
	then
		SVN_HTTPD_MODULE_PATH="$d"
		break
	fi
done

start_httpd () {
	repo_base_path="$1"
	if test -z "$SVN_HTTPD_PORT"
	then
		echo >&2 'SVN_HTTPD_PORT is not defined!'
		return
	fi
	if test -z "$repo_base_path"
	then
		repo_base_path=svn
	fi

	mkdir "$GIT_DIR"/logs

	cat > "$GIT_DIR/httpd.conf" <<EOF
ServerName "git svn test"
ServerRoot "$GIT_DIR"
DocumentRoot "$GIT_DIR"
PidFile "$GIT_DIR/httpd.pid"
LockFile logs/accept.lock
Listen 127.0.0.1:$SVN_HTTPD_PORT
LoadModule dav_module $SVN_HTTPD_MODULE_PATH/mod_dav.so
LoadModule dav_svn_module $SVN_HTTPD_MODULE_PATH/mod_dav_svn.so
<Location /$repo_base_path>
	DAV svn
	SVNPath "$rawsvnrepo"
</Location>
EOF
	"$SVN_HTTPD_PATH" -f "$GIT_DIR"/httpd.conf -k start
	svnrepo="http://127.0.0.1:$SVN_HTTPD_PORT/$repo_base_path"
}

stop_httpd () {
	test -z "$SVN_HTTPD_PORT" && return
	"$SVN_HTTPD_PATH" -f "$GIT_DIR"/httpd.conf -k stop
}

convert_to_rev_db () {
	perl -w -- - "$@" <<\EOF
use strict;
@ARGV == 2 or die "Usage: convert_to_rev_db <input> <output>";
open my $wr, '+>', $ARGV[1] or die "$!: couldn't open: $ARGV[1]";
open my $rd, '<', $ARGV[0] or die "$!: couldn't open: $ARGV[0]";
my $size = (stat($rd))[7];
($size % 24) == 0 or die "Inconsistent size: $size";
while (sysread($rd, my $buf, 24) == 24) {
	my ($r, $c) = unpack('NH40', $buf);
	my $offset = $r * 41;
	seek $wr, 0, 2 or die $!;
	my $pos = tell $wr;
	if ($pos < $offset) {
		for (1 .. (($offset - $pos) / 41)) {
			print $wr (('0' x 40),"\n") or die $!;
		}
	}
	seek $wr, $offset, 0 or die $!;
	print $wr $c,"\n" or die $!;
}
close $wr or die $!;
close $rd or die $!;
EOF
}

require_svnserve () {
    if test -z "$SVNSERVE_PORT"
    then
        say 'skipping svnserve test. (set $SVNSERVE_PORT to enable)'
        test_done
    fi
}

start_svnserve () {
    svnserve --listen-port $SVNSERVE_PORT \
             --root "$rawsvnrepo" \
             --listen-once \
             --listen-host 127.0.0.1 &
}

