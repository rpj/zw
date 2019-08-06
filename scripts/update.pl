#!/usr/bin/perl
use File::Basename;
my $bn = basename($0);
my $lp = $0;
$lp =~ s/$bn//ig;
my $updateJson = `$lp/release-info.pl $ARGV[0] $ARGV[1]`;
my $otp = `$lp/otp-generate.pl $ARGV[2] $ARGV[3] 2> /dev/null`;
chomp($otp);
$updateJson =~ s/(\"otp\":\s+)(0)/\1$otp/ig;
$updateJson =~ s/\s*//ig;
my $cmd = "redis-cli -h 192.168.1.252 -a '$ARGV[3]' set $ARGV[2]:config:update '$updateJson'";
print "SENDING:\n\t$cmd\n";
print "RESULT:\n\t" . `$cmd`;
