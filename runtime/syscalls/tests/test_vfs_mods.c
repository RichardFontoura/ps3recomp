#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../sys_fs.c"

uint8_t* vm_base;
uint32_t ppu_vm_size;

static int g_pass, g_fail;
static char g_root[256];

const char* edat_resolve(const char* path, char* out, size_t cap) { (void)out; (void)cap; return path; }

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

static void touch(const char* rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    FILE* f = fopen(p, "w");
    if (f) { fputs(rel, f); fclose(f); }
}

static void dir(const char* rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    mkdir(p, 0755);
}

static int resolves_to(const char* guest_rel, const char* want_rel)
{
    char path[1024], want[512];
    snprintf(path, sizeof path, "%s/%s", g_root, guest_rel);
    snprintf(want, sizeof want, "%s/%s", g_root, want_rel);
    ps3_vfs_ps3game_fallback(path, sizeof path);
    return strcmp(path, want) == 0;
}

int main(void)
{
    snprintf(g_root, sizeof g_root, "/tmp/test_vfs_mods_%d", (int)getpid());
    mkdir(g_root, 0755);
    dir("game"); dir("game/USRDIR"); dir("game/USRDIR/SUB"); dir("mod1"); dir("mod1/SUB"); dir("mod2"); dir("mod1/DIRONLY");
    dir("game/USRDIR/DIRONLY");
    touch("game/USRDIR/A.TXT"); touch("game/USRDIR/B.TXT"); touch("game/USRDIR/C.TXT"); touch("game/USRDIR/SUB/D.TXT");
    touch("mod1/A.TXT"); touch("mod2/A.TXT"); touch("mod2/B.TXT"); touch("mod1/SUB/D.TXT"); touch("mod2/ONLYMOD.TXT");
    char mods[600];
    snprintf(mods, sizeof mods, "%s/mod1;%s/mod2", g_root, g_root);
    setenv("PS3_MODS", mods, 1);

    check(resolves_to("game/USRDIR/A.TXT", "mod1/A.TXT"), "the first mod that has a file wins");
    check(resolves_to("game/USRDIR/B.TXT", "mod2/B.TXT"), "a later mod supplies what the first lacks");
    check(resolves_to("game/USRDIR/C.TXT", "game/USRDIR/C.TXT"), "a file no mod has comes from the game");
    check(resolves_to("game/USRDIR/SUB/D.TXT", "mod1/SUB/D.TXT"), "subdirectories mirror USRDIR");
    check(resolves_to("game/USRDIR/ONLYMOD.TXT", "mod2/ONLYMOD.TXT"), "a mod may add a file the game lacks");
    check(resolves_to("game/USRDIR/DIRONLY", "game/USRDIR/DIRONLY"), "a directory in a mod does not replace one in the game");
    check(resolves_to("game/A.TXT", "game/A.TXT"), "paths outside a USRDIR are never overlaid");

    printf("VFS mod overlay: %d passed, %d failed\n", g_pass, g_fail);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) return 2;
    return g_fail != 0;
}
