#!/usr/bin/perl

## tar archive frontend for git-fast-import
##
## For example:
##
##  mkdir project; cd project; git init
##  perl import-tars.perl *.tar.bz2
##  git whatchanged import-tars
##

use strict;
die "usage: import-tars *.tar.{gz,bz2,Z}\n" unless @ARGV;

my $branch_name = 'import-tars';
my $branch_ref = "refs/heads/$branch_name";
my $author_name = $ENV{'GIT_AUTHOR_NAME'} || 'T Ar Creator';
my $author_email = $ENV{'GIT_AUTHOR_EMAIL'} || 'tar@example.com';
my $committer_name = $ENV{'GIT_COMMITTER_NAME'} || `git config --get user.name`;
my $committer_email = $ENV{'GIT_COMMITTER_EMAIL'} || `git config --get user.email`;

chomp($committer_name, $committer_email);

open(FI, '|-', 'git', 'fast-import', '--quiet')
	or die "Unable to start git fast-import: $!\n";
foreach my $tar_file (@ARGV)
{
	my $commit_time = time;
	$tar_file =~ m,([^/]+)$,;
	my $tar_name = $1;

	if ($tar_name =~ s/\.(tar\.gz|tgz)$//) {
		open(I, '-|', 'gunzip', '-c', $tar_file)
			or die "Unable to gunzip -c $tar_file: $!\n";
	} elsif ($tar_name =~ s/\.(tar\.bz2|tbz2)$//) {
		open(I, '-|', 'bunzip2', '-c', $tar_file)
			or die "Unable to bunzip2 -c $tar_file: $!\n";
	} elsif ($tar_name =~ s/\.tar\.Z$//) {
		open(I, '-|', 'uncompress', '-c', $tar_file)
			or die "Unable to uncompress -c $tar_file: $!\n";
	} elsif ($tar_name =~ s/\.tar$//) {
		open(I, $tar_file) or die "Unable to open $tar_file: $!\n";
	} else {
		die "Unrecognized compression format: $tar_file\n";
	}

	my $author_time = 0;
	my $next_mark = 1;
	my $have_top_dir = 1;
	my ($top_dir, %files);

	while (read(I, $_, 512) == 512) {
		my ($name, $mode, $uid, $gid, $size, $mtime,
			$chksum, $typeflag, $linkname, $magic,
			$version, $uname, $gname, $devmajor, $devminor,
			$prefix) = unpack 'Z100 Z8 Z8 Z8 Z12 Z12
			Z8 Z1 Z100 Z6
			Z2 Z32 Z32 Z8 Z8 Z*', $_;
		last unless length($name);
		if ($name eq '././@LongLink') {
			# GNU tar extension
			if (read(I, $_, 512) != 512) {
				die ('Short archive');
			}
			$name = unpack 'Z257', $_;
			next unless $name;

			my $dummy;
			if (read(I, $_, 512) != 512) {
				die ('Short archive');
			}
			($dummy, $mode, $uid, $gid, $size, $mtime,
			$chksum, $typeflag, $linkname, $magic,
			$version, $uname, $gname, $devmajor, $devminor,
			$prefix) = unpack 'Z100 Z8 Z8 Z8 Z12 Z12
			Z8 Z1 Z100 Z6
			Z2 Z32 Z32 Z8 Z8 Z*', $_;
		}
		next if $name =~ m{/\z};
		$mode = oct $mode;
		$size = oct $size;
		$mtime = oct $mtime;
		next if $typeflag == 5; # directory

		print FI "blob\n", "mark :$next_mark\n";
		if ($typeflag == 2) { # symbolic link
			print FI "data ", length($linkname), "\n", $linkname;
			$mode = 0120000;
		} else {
			print FI "data $size\n";
			while ($size > 0 && read(I, $_, 512) == 512) {
				print FI substr($_, 0, $size);
				$size -= 512;
			}
		}
		print FI "\n";

		my $path;
		if ($prefix) {
			$path = "$prefix/$name";
		} else {
			$path = "$name";
		}
		$files{$path} = [$next_mark++, $mode];

		$author_time = $mtime if $mtime > $author_time;
		$path =~ m,^([^/]+)/,;
		$top_dir = $1 unless $top_dir;
		$have_top_dir = 0 if $top_dir ne $1;
	}

	print FI <<EOF;
commit $branch_ref
author $author_name <$author_email> $author_time +0000
committer $committer_name <$committer_email> $commit_time +0000
data <<END_OF_COMMIT_MESSAGE
Imported from $tar_file.
END_OF_COMMIT_MESSAGE

deleteall
EOF

	foreach my $path (keys %files)
	{
		my ($mark, $mode) = @{$files{$path}};
		$path =~ s,^([^/]+)/,, if $have_top_dir;
		$mode = $mode & 0111 ? 0755 : 0644 unless $mode == 0120000;
		printf FI "M %o :%i %s\n", $mode, $mark, $path;
	}
	print FI "\n";

	print FI <<EOF;
tag $tar_name
from $branch_ref
tagger $author_name <$author_email> $author_time +0000
data <<END_OF_TAG_MESSAGE
Package $tar_name
END_OF_TAG_MESSAGE

EOF

	close I;
}
close FI;
