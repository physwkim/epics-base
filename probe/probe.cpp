/* Which C++ runtime DLLs this msvcrt mingw exe actually loads, and
 * whether constructing std::ostringstream / std::ifstream survives.
 * Mirrors the libCom tests: epicsLoadTest (ostringstream) passes on
 * the Win-22 mingw runner, iocshTest (ifstream) crashes. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <sstream>
#include <fstream>
#include <string>

static void modules(void)
{
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
        return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mods[i], path, sizeof(path)))
            printf("module %s\n", path);
    }
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    modules();
    {
        std::ostringstream s;
        s << "x" << 1;
        printf("ostringstream ok: %s\n", s.str().c_str());
        fflush(stdout);
    }
    {
        std::ifstream f(argv[0], std::ifstream::binary | std::ifstream::ate);
        printf("ifstream ok: %s size %ld\n", f.is_open() ? "open" : "closed",
            (long)f.tellg());
        fflush(stdout);
    }
    {
        std::ifstream t(argc > 1 ? argv[1] : "probe.cpp");
        std::stringstream buffer;
        buffer << t.rdbuf();
        printf("ifstream+stringstream ok: %lu bytes\n",
            (unsigned long)buffer.str().size());
        fflush(stdout);
    }
    return 0;
}
