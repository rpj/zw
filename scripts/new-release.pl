#!/usr/bin/perl

# symlink the script as "release-info.pl" to enable this mode
$infoMode = 1, if ($0 =~ /release-info/i);

my $version = shift || die "version\n\n";
my $destDir = shift || die "dest dir\n\n";
my $buildDir = shift || (!$infoMode && die "build dir\n\n");

$version =~ s/v//ig;

my $tFile = "zero_watch-v$version.ino.bin";
my $target = "$destDir/$tFile";

if (!$infoMode) {
    $buildDir .= "/zero_watch.ino.bin";
    die "$buildDir does not exist\n\n", unless (-e $buildDir && !(-d $buildDir));
}
else {
    $buildDir = $target;
}

die "$destDir does not exist\n\n", unless (-e $destDir && -d $destDir);

die "Bad binary version\n\n", unless (`strings $buildDir` =~ /v([0-9\.]+)/);
my $binVer = $1;
die "Versions don't match ($binVer vs $version)\n\n", unless ($version eq $binVer);

my $md5 = (split(/\s/, `md5sum $buildDir`))[0];
`md5sum $buildDir > $target.md5sum.txt`;

my $bSize = (stat($buildDir))[7];

if (!$infoMode) {
    `cp $buildDir $target`;
    die "Copy failed ($?)\n\n", if ($?);
}

print <<__EOF__;
{
    "url":  "zero_watch_updates/$tFile",
    "md5":  "$md5",
    "size": $bSize,
    "otp": 0
}
__EOF__