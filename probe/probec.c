/* C only: no libstdc++ involved */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
int main(void)
{
    HMODULE mods[512]; DWORD need = 0, i;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
        for (i = 0; i < need / sizeof(HMODULE); i++) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mods[i], path, sizeof(path))) printf("module %s\n", path);
        }
    printf("c ok\n");
    return 0;
}
