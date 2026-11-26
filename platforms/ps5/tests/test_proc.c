/* Host-side unit tests for the injector's process-selection policy.
 * Compiled and run with the host cc (not prospero-clang): the policy is pure
 * and has no PS5 dependencies. See payload/proc_select.c.
 *
 * Review Focus #3: among several game-like processes, pick GTA VI (here, the
 * one whose comm/tdname matches), and refuse rather than guess when the match
 * is ambiguous.
 */
#include "../payload/proc.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static shvi_proc_ent ent(pid_t pid, const char *comm, const char *tdname) {
    shvi_proc_ent e;
    memset(&e, 0, sizeof(e));
    e.pid = pid;
    snprintf(e.comm, sizeof(e.comm), "%s", comm);
    snprintf(e.tdname, sizeof(e.tdname), "%s", tdname);
    return e;
}

static void test_single_match_by_comm(void) {
    shvi_proc_ent list[] = {
        ent(100, "SceShellCore", "SceShellCore"),
        ent(101, "eboot.bin", "MainThread"),
        ent(102, "SceRedisServer", "SceRedisServer"),
    };
    int amb = 123;
    pid_t pid = shvi_select_game_pid(list, 3, "eboot.bin", 1, &amb);
    CHECK(pid == 101);
    CHECK(amb == 0);
}

static void test_match_by_tdname(void) {
    /* Some titles present their real name only on the main thread. */
    shvi_proc_ent list[] = {
        ent(200, "eboot.bin", "GTA6"),
        ent(201, "SceSysCore", "SceSysCore"),
    };
    int amb = 123;
    pid_t pid = shvi_select_game_pid(list, 2, "GTA6", 1, &amb);
    CHECK(pid == 200);
    CHECK(amb == 0);
}

static void test_skips_self(void) {
    /* The injector itself runs as eboot.bin-style homebrew; never pick self. */
    shvi_proc_ent list[] = {
        ent(50, "eboot.bin", "payload"),  /* this is us (self_pid 50) */
        ent(101, "eboot.bin", "MainThread"),
    };
    int amb = 123;
    pid_t pid = shvi_select_game_pid(list, 2, "eboot.bin", 50, &amb);
    CHECK(pid == 101);
    CHECK(amb == 0);
}

static void test_no_match_returns_minus1(void) {
    shvi_proc_ent list[] = {
        ent(100, "SceShellCore", "SceShellCore"),
        ent(102, "SceRedisServer", "SceRedisServer"),
    };
    int amb = 123;
    pid_t pid = shvi_select_game_pid(list, 2, "eboot.bin", 1, &amb);
    CHECK(pid == -1);
    CHECK(amb == 0);
}

static void test_ambiguous_refuses(void) {
    /* Two distinct processes match -> unsafe to guess -> refuse. */
    shvi_proc_ent list[] = {
        ent(101, "eboot.bin", "AppA"),
        ent(202, "eboot.bin", "AppB"),
    };
    int amb = 0;
    pid_t pid = shvi_select_game_pid(list, 3 - 1, "eboot.bin", 1, &amb);
    CHECK(pid == -1);
    CHECK(amb == 1);
}

static void test_self_excluded_leaves_single_match(void) {
    /* Self also "matches" the name but is excluded, so the remaining single
     * match is NOT ambiguous. */
    shvi_proc_ent list[] = {
        ent(50, "eboot.bin", "payload"),
        ent(101, "eboot.bin", "MainThread"),
    };
    int amb = 123;
    pid_t pid = shvi_select_game_pid(list, 2, "eboot.bin", 50, &amb);
    CHECK(pid == 101);
    CHECK(amb == 0);
}

int main(void) {
    test_single_match_by_comm();
    test_match_by_tdname();
    test_skips_self();
    test_no_match_returns_minus1();
    test_ambiguous_refuses();
    test_self_excluded_leaves_single_match();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all proc-selection tests passed\n");
    return 0;
}
