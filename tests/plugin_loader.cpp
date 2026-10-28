/*
 * Loads the plugin the way the editor does (RTLD_LAZY | RTLD_LOCAL) and
 * checks the symbols and the FuncItem layout the host indexes by stride.
 */
#include "plugin.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef const char *(*GetName_t)(void);
typedef FuncItem *(*GetFuncs_t)(int *);
typedef int (*IsUnicode_t)(void);
typedef void (*SetInfo_t)(NppData);
typedef long (*MessageProc_t)(unsigned int, unsigned long, long);

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL %s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: plugin_loader SemanticSearch.so\n");
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!h) return fail(dlerror());

    auto get_name = (GetName_t)dlsym(h, "getName");
    auto get_funcs = (GetFuncs_t)dlsym(h, "getFuncsArray");
    auto be_notified = dlsym(h, "beNotified");
    auto message_proc = (MessageProc_t)dlsym(h, "messageProc");
    auto is_unicode = (IsUnicode_t)dlsym(h, "isUnicode");
    auto set_info = (SetInfo_t)dlsym(h, "setInfo");
    if (!get_name || !get_funcs || !be_notified || !message_proc || !is_unicode || !set_info)
        return fail("missing plugin symbol");

    if (sizeof(FuncItem) != 80) return fail("FuncItem size is not 80");

    NppData data;
    memset(&data, 0, sizeof data);
    set_info(data);

    const char *name = get_name();
    if (!name || strcmp(name, "Semantic Search") != 0) return fail("getName");
    if (!is_unicode()) return fail("isUnicode");
    if (message_proc(0, 0, 0) != 1) return fail("messageProc");

    int n = 0;
    FuncItem *items = get_funcs(&n);
    if (!items || n != 2) return fail("func count");
    if (strcmp(items[0].itemName, "Semantic Heatmap Search") != 0) return fail("item 0");
    if (strcmp(items[1].itemName, "About") != 0) return fail("item 1");
    if (!items[0].pFunc || !items[1].pFunc) return fail("callbacks");

    printf("plugin_loader ok (%s, %d items, FuncItem %zu)\n", name, n, sizeof(FuncItem));
    dlclose(h);
    return 0;
}
