# TAG names the run; recipes work under sh.exe and cmd.exe alike
TAG ?= default

all: env-$(TAG).txt out-$(TAG).txt

probe.exe: probe.cpp
	g++ -O2 -Wall -o probe.exe probe.cpp -lpsapi

env-$(TAG).txt:
	perl env.pl > env-$(TAG).txt

out-$(TAG).txt: probe.exe
	perl run.pl probe.exe probe.cpp > out-$(TAG).txt 2>&1
