#!/usr/bin/perl

my $buildDir = shift || die "build dir\n\n";
my $destDir = shift || die "dest dir\n\n";
my $version = shift || die "version\n\n";

$version =~ s/v//ig;
$buildDir .= "/zero_watch.ino.bin";

die "$buildDir does not exist\n\n", unless (-e $buildDir && !(-d $buildDir));
die "$destDir does not exist\n\n", unless (-e $destDir && -d $destDir);

die "Bad binary version\n\n", unless (`strings $buildDir` =~ /v([0-9\.]+)/);
my $binVer = $1;
die "Versions don't match ($binVer vs $version)\n\n", unless ($version eq $binVer);

my $tFile = "zero_watch-v$version.ino.bin";
my $target = "$destDir/$tFile";

my $md5 = (split(/\s/, `md5sum $buildDir`))[0];
`md5sum $buildDir > $target.md5sum.txt`;

my $bSize = (stat($buildDir))[7];

`cp $buildDir $target`;
die "Copy failed ($?)\n\n", if ($?);

print <<__EOF__;
{
    "url":  "zero_watch_updates/$tFile",
    "md5":  "$md5",
    "size": $bSize,
    "otp": 0
}
__EOF__