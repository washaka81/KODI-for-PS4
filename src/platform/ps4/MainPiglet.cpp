#include "PigletApplication.hpp"

#include <cstdio>

int main() {
    FILE *lf = fopen("/data/kodi-ps4/init.log", "w");
    if (lf) { fputs("MAIN_START\n", lf); fclose(lf); }
    static nik::PigletApplication& AppRef{ nik::GetPigletApplication() };
    bool ok = AppRef.Init();
    lf = fopen("/data/kodi-ps4/init.log", "a");
    if (lf) { fprintf(lf, "INIT_RESULT=%d\n", ok ? 1 : 0); fclose(lf); }
    if (!ok) return 1;
    while (AppRef.Logic()) {
        AppRef.Render();
    }
    return 0;
}
