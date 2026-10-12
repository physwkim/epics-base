# Mirror ci-scripts cue.py: prepend Chocolatey and Strawberry to PATH, then
# run make the way 'cue.py test' does. Compare which libstdc++ probe.exe
# loads when make's recipes go through the sh.exe on PATH (Git for
# Windows) versus cmd.exe, versus no make at all.
import os, subprocess as sp, shutil, glob

os.environ['PATH'] = os.pathsep.join([r'C:\ProgramData\Chocolatey\bin',
                                      r'C:\Strawberry\c\bin',
                                      r'C:\Strawberry\perl\site\bin',
                                      r'C:\Strawberry\perl\bin',
                                      os.environ['PATH']])
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def show(title, cmd, **kw):
    print('::group::' + title, flush=True)
    rc = sp.call(cmd, shell=True, **kw)
    print('rc', rc, flush=True)
    print('::endgroup::', flush=True)
    return rc

for exe in ['sh.exe', 'make.exe', 'perl.exe', 'g++.exe']:
    show('where ' + exe, 'where ' + exe)
show('git --version', 'git --version')
show('g++ --version', 'g++ --version')
show('make --version', 'make --version')

show('make TAG=default (sh from PATH)', 'make TAG=default')
show('make TAG=cmd (SHELL=cmd.exe)', 'make TAG=cmd SHELL=cmd.exe')
nogit = os.pathsep.join(p for p in os.environ['PATH'].split(os.pathsep)
                        if 'Git' not in p)
show('make TAG=nogit (Git dirs removed from PATH)', 'make TAG=nogit',
     env=dict(os.environ, PATH=nogit))
show('direct (no make)', 'perl run.pl probe.exe probe.cpp > out-direct.txt 2>&1')
show('direct env', 'perl env.pl > env-direct.txt')

for f in sorted(glob.glob('env-*.txt') + glob.glob('out-*.txt')):
    print('::group::' + f, flush=True)
    with open(f, errors='replace') as fp:
        print(fp.read(), flush=True)
    print('::endgroup::', flush=True)
