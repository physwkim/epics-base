# TAG names the run; recipes work under sh.exe and cmd.exe alike.
# PERL is the full path, as EPICS' makeTestfile.pl recipes use it.
TAG ?= default
PERL ?= C:/Strawberry/perl/bin/perl

all: env-$(TAG).txt out-$(TAG).txt outc-$(TAG).txt outs-$(TAG).txt outsh-$(TAG).txt

probe.exe: probe.cpp
	g++ -O2 -Wall -o probe.exe probe.cpp -lpsapi

probe-static.exe: probe.cpp
	g++ -O2 -Wall -static-libstdc++ -static-libgcc -o probe-static.exe probe.cpp -lpsapi

probec.exe: probec.c
	gcc -O2 -Wall -o probec.exe probec.c -lpsapi

env-$(TAG).txt:
	$(PERL) env.pl > env-$(TAG).txt

out-$(TAG).txt: probe.exe
	$(PERL) run.pl probe.exe probe.cpp > out-$(TAG).txt 2>&1

outc-$(TAG).txt: probec.exe
	$(PERL) run.pl probec.exe > outc-$(TAG).txt 2>&1

outs-$(TAG).txt: probe-static.exe
	$(PERL) run.pl probe-static.exe probe.cpp > outs-$(TAG).txt 2>&1

outsh-$(TAG).txt: probe.exe
	probe.exe probe.cpp > outsh-$(TAG).txt 2>&1
