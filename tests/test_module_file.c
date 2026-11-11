#define _XOPEN_SOURCE 700 /* mkdtemp, symlink */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lizard/builtin.h"
#include "lizard/link.h"
#include "lizard/module_file.h"
#include "test_util.h"

static void write_file(const char *dir, const char *rel, const char *text) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *file = fopen(path, "wb");
    LZ_CHECK(file != NULL);
    if (file != NULL) {
        (void)fputs(text, file);
        (void)fclose(file);
    }
}

static bool fails_with(LzVM *vm, const char *name, const char *text) {
    LzModule *module = NULL;
    LzStatus status = lz_module_load(vm, NULL, name, &module);
    bool ok = status != LZ_OK && strstr(vm->error, text) != NULL;
    if (!ok) {
        fprintf(stderr, "name '%s': wanted [%s], got status %d [%s]\n", name, text, (int)status, vm->error);
    }
    return ok;
}

int main(void) {
    /* Termux has no /tmp, so try $TMPDIR first, then /tmp, then the current folder. */
    char dir[256] = "";
    const char *bases[3] = {getenv("TMPDIR"), "/tmp", "."};
    for (int i = 0; i < 3 && dir[0] == '\0'; i++) {
        if (bases[i] == NULL || bases[i][0] == '\0') {
            continue;
        }
        char attempt[256];
        (void)snprintf(attempt, sizeof attempt, "%s/lizard-modfile-XXXXXX", bases[i]);
        if (mkdtemp(attempt) != NULL) {
            memcpy(dir, attempt, strlen(attempt) + 1);
        }
    }
    if (dir[0] == '\0') {
        fprintf(stderr, "test_module_file: no writable folder for a temporary tree, skipped\n");
        return 0;
    }
    char sub[512];
    (void)snprintf(sub, sizeof sub, "%s/tools", dir);
    LZ_CHECK(mkdir(sub, 0700) == 0);
    write_file(dir, "main.lz", "println(1)\n");
    write_file(dir, "util.lz", "println(\"util runs\")\nlet n = 7\n");
    write_file(dir, "tools/format.lz", "let f = 1\n");
    write_file(dir, "loop.lz", "import(\"main\")\n");
    char target[512];
    char link_path[512];
    (void)snprintf(target, sizeof target, "%s/util.lz", dir);
    (void)snprintf(link_path, sizeof link_path, "%s/alias.lz", dir);
    bool have_alias = symlink(target, link_path) == 0; /* some filesystems refuse symlinks */

    LzBuffer out = {0};
    LzBuffer err = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    char script[512];
    (void)snprintf(script, sizeof script, "%s/main.lz", dir);
    LZ_CHECK(lz_file_resolver_install(&vm, script));

    LzModule *util = NULL;
    LZ_CHECK(lz_module_load(&vm, NULL, "util", &util) == LZ_OK && util != NULL);
    LzModule *format = NULL;
    LZ_CHECK(lz_module_load(&vm, NULL, "tools.format", &format) == LZ_OK && format != NULL);
    LZ_CHECK(format != util);
    LzModule *again = NULL;
    if (have_alias) {
        LZ_CHECK(lz_module_load(&vm, NULL, "alias", &again) == LZ_OK);
        LZ_CHECK(again == util); /* two names, one file, one module */
    }
    LZ_CHECK(strcmp(out.data, "util runs\n") == 0);
    LZ_CHECK(vm.module_count == 3);

    LZ_CHECK(fails_with(&vm, "nothing", "cannot find module 'nothing' (looked for "));
    LZ_CHECK(fails_with(&vm, "nothing", "nothing.lz, imported from main.lz)"));
    LZ_CHECK(fails_with(&vm, "a.b", "a/b.lz"));
    LZ_CHECK(fails_with(&vm, "..", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "a/b", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "a\\b", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "/etc/passwd", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "a..b", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "1a", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "", "invalid module name"));
    LZ_CHECK(fails_with(&vm, "std.io", "the standard library is not available yet"));
    LZ_CHECK(fails_with(&vm, "std", "the standard library is not available yet"));
    LZ_CHECK(fails_with(&vm, "main", "circular import: main.lz -> main.lz")); /* back to the running script */
    LZ_CHECK(vm.module_count == 3);

    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    LZ_CHECK(lz_link_check(&vm));
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);

    (void)unlink(link_path);
    (void)snprintf(link_path, sizeof link_path, "%s/main.lz", dir);
    (void)unlink(link_path);
    (void)snprintf(link_path, sizeof link_path, "%s/util.lz", dir);
    (void)unlink(link_path);
    (void)snprintf(link_path, sizeof link_path, "%s/loop.lz", dir);
    (void)unlink(link_path);
    (void)snprintf(link_path, sizeof link_path, "%s/tools/format.lz", dir);
    (void)unlink(link_path);
    (void)rmdir(sub);
    (void)rmdir(dir);
    return LZ_TEST_RESULT();
}
