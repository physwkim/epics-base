# run the exe the way makeTestfile.pl's .t wrapper does: from perl
my $rc = system(@ARGV);
print "exit status ", $rc >> 8, " signal/flags ", $rc & 0xffff, "\n";
exit($rc ? 1 : 0);
