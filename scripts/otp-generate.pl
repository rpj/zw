#!/usr/bin/perl

# usage, either:
# ./otp-generate.pl [targetHostname] [redisPassword] (redisHost)
# - or -
# ./otp-generate.pl [targetHostname] -D [currentLc]
# (the "direct" version ^)

my $targetHost = shift;
my $redisPassword = shift;
my $redisHost = shift || '192.168.1.252';

my $currentLc = -1;
my $lookAhead = 0;

if ($redisPassword eq '-D' && int($redisHost) > 0) {
  $currentLc = int($redisHost);
}
else {
  $currentLc = int(`redis-cli -h $redisHost -a '$redisPassword' get $targetHost:heartbeat 2> /dev/null`);
}

if ($currentLc <= 0) {
  print STDERR "Bad current LC ($currentLc)!\n";
  exit(-1);
}

my $detected = undef;
$_=`cat ../zero_watch.ino`;
$detected = $1, if (/OTA_WINDOW_MINUTES\s+(\d+)/g);
my $div = (int(1e6) * 60) * ($detected || 2);

if ($currentLc < $div) {
  print STDERR "Unit hasn't been running long enough (need $div, have $currentLc), please wait...\n";
  exit(0);
}

print STDERR "[OTP] I: $currentLc\n";
$currentLc = int($currentLc / $div);
print STDERR "[OTP] 0: $currentLc\n";

my @fudgeTable = (42, 69, 3, 18, 25, 12, 51, 93, 54, 76);
$currentLc += $fudgeTable[($currentLc % scalar(@fudgeTable))];
print STDERR "[OTP] D: $currentLc\n";

for ($i = 0; $i < length($targetHost); $i++) {
  $currentLc += int(ord(substr($targetHost, $i, 1)));
  print STDERR "[OTP] $i: $currentLc (+= " . (int(ord(substr($targetHost, $i, 1)))) . ")\n";
}

$currentLc = $currentLc & 0xFFFF;
print STDERR "[OTP] F: $currentLc\n";
print "$currentLc\n";
