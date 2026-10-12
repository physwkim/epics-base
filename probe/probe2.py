# After 'cue.py build': run the real iocshTest.exe the way 'cue.py test'
# does (make recipe -> Git sh.exe on PATH -> Strawberry perl -> exe), and
# under gdb, then the same with SHELL=cmd.exe for contrast.
import os, subprocess as sp, glob, shutil

top = os.getcwd()
arch = 'windows-x64-mingw'
os.environ['PATH'] = os.pathsep.join([os.path.join(top, 'bin', arch),
                                      r'C:\ProgramData\Chocolatey\bin',
                                      r'C:\Strawberry\c\bin',
                                      r'C:\Strawberry\perl\site\bin',
                                      r'C:\Strawberry\perl\bin',
                                      os.environ['PATH']])
os.environ['TOP'] = top
tdir = os.path.join(top, 'modules', 'libcom', 'test', 'O.' + arch)
shutil.copy(os.path.join(top, 'probe', 'Makefile2'), os.path.join(tdir, 'Makefile2'))
os.chdir(tdir)

def show(title, cmd, **kw):
    print('::group::' + title, flush=True)
    rc = sp.call(cmd, shell=True, **kw)
    print('rc', rc, flush=True)
    print('::endgroup::', flush=True)

show('dir', 'dir iocshTest.exe iocshTest.t')
show('make -f Makefile2 TAG=sh (sh from PATH)', 'make -k -f Makefile2 TAG=sh')
show('make -f Makefile2 TAG=cmd (SHELL=cmd.exe)', 'make -k -f Makefile2 TAG=cmd SHELL=cmd.exe')

for f in sorted(glob.glob('run-*.txt') + glob.glob('tap-*.txt') + glob.glob('gdb-*.txt')):
    print('::group::' + f, flush=True)
    with open(f, errors='replace') as fp:
        print(fp.read()[:20000], flush=True)
    print('::endgroup::', flush=True)
