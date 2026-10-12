# print the environment a make recipe runs in (shell-agnostic: no quoting)
print "SHELL=", ($ENV{SHELL} // ''), "\n";
print "MSYSTEM=", ($ENV{MSYSTEM} // ''), "\n";
print "PATH entries:\n";
print "  $_\n" for split /;/, $ENV{PATH};
