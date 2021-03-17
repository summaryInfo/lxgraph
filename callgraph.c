
#include "util.h"
#include "hashtable.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <clang-c/Index.h>


struct strtab_item {
    ht_head_t head;
    const char *name;
};

typedef struct strtab_item *literal;

bool strtab_item_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct strtab_item *ai = (const struct strtab_item *)a;
    const struct strtab_item *bi = (const struct strtab_item *)b;
    return !strcmp(ai->name, bi->name);
}

literal strtab_put(struct hashtable *ht, const char *str) {
    size_t len = strlen(str);
    struct strtab_item key = { .head = { .hash = hash64(str, len) }, .name = str };
    ht_head_t **h = ht_lookup_ptr(ht, (ht_head_t *)&key);
    if (*h) return (literal)*h;

    struct strtab_item *new = malloc(sizeof *new + len + 1);
    assert(new);
    new->head = key.head;
    new->name = (char *)(new + 1);
    strcpy((char *)new->name, str);

    ht_insert_hint(ht, h, (ht_head_t *)new);
    return (literal)new;
}

struct callgraph {
    literal function;
    literal file;
    struct hashtable strtab;

    struct invokation {
        literal caller;
        literal callee;
    } *calls;
    size_t calls_caps;
    size_t calls_size;

    struct definition {
        literal file;
        literal name;
    } *defs;
    size_t defs_caps;
    size_t defs_size;
};

inline static void set_current(struct callgraph *cg, const char *fun, const char *file) {
    assert(!cg->function); // No nested functions allowed
    cg->function = strtab_put(&cg->strtab, fun);
    cg->file = strtab_put(&cg->strtab, file);
}

inline static void add_function_definition(struct callgraph *cg, literal id) {
    if (adjust_buffer((void **)&cg->defs, &cg->defs_caps, cg->defs_size + 1, sizeof *cg->defs)) {
        cg->defs[cg->defs_size++] = (struct definition) { cg->file, id };
    }
}

inline static void add_function_call(struct callgraph *cg, const char *str) {
    literal id = strtab_put(&cg->strtab, str);
    if (adjust_buffer((void **)&cg->calls, &cg->calls_caps, cg->calls_size + 1, sizeof *cg->calls)) {
        // TODO Calls outside functions?
        cg->calls[cg->calls_size++] = (struct invokation) { cg->function, id };
    }
}

static enum CXChildVisitResult visit(CXCursor cur, CXCursor parent, CXClientData data) {
    struct callgraph *cg = (struct callgraph *)data;

    (void)parent;

    enum CXCursorKind kind = clang_getCursorKind(cur);
    switch (kind) {
    case CXCursor_BlockExpr:
    case CXCursor_ParenExpr:
    case CXCursor_UnaryOperator:
    case CXCursor_ArraySubscriptExpr:
    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator:
    case CXCursor_ConditionalOperator:
    case CXCursor_CStyleCastExpr:
    case CXCursor_CompoundLiteralExpr:
    case CXCursor_InitListExpr:
    case CXCursor_StmtExpr:
    case CXCursor_GenericSelectionExpr:
    case CXCursor_UnaryExpr:
    case CXCursor_CompoundStmt:
    case CXCursor_CaseStmt:
    case CXCursor_DefaultStmt:
    case CXCursor_IfStmt:
    case CXCursor_SwitchStmt:
    case CXCursor_WhileStmt:
    case CXCursor_DoStmt:
    case CXCursor_ForStmt:
    case CXCursor_ReturnStmt:
    case CXCursor_VarDecl:
    case CXCursor_CallExpr:
        return CXChildVisit_Recurse;
    case CXCursor_FunctionDecl:;
        CXString name = clang_getCursorDisplayName(cur);
        CXString tu = clang_getTranslationUnitSpelling(clang_Cursor_getTranslationUnit(cur));
        set_current(cg, clang_getCString(name), clang_getCString(name));
        // TODO Declaration or definition?
        add_function_definition(cg, cg->function);
        clang_visitChildren(cur, visit, data);
        set_current(cg, NULL, clang_getCString(tu));
        clang_disposeString(name);
        clang_disposeString(tu);
        break;
    case CXCursor_DeclRefExpr:;
        CXCursor decl = clang_getCursorReferenced(cur);
        if (clang_getCursorKind(decl) == CXCursor_FunctionDecl) {
            CXString fname = clang_getCursorDisplayName(decl);
            add_function_call(cg, clang_getCString(fname));
            clang_disposeString(fname);
        }
        break;
    default:
        break;
    }
    return CXChildVisit_Continue;
}

struct callgraph *parse_file(const char *path) {
    struct callgraph *cg = calloc(1, sizeof *cg);
    
    // TODO Optimize: dont create index on every file
    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit unit = clang_parseTranslationUnit(index, path, NULL, 0, NULL, 0, CXTranslationUnit_None);
    if (!unit) {
        warn("Cannot parse file '%s'", path);
        clang_disposeIndex(index);
        free(cg);
        return NULL;
    }

    ht_init(&cg->strtab, HT_INIT_CAPS, strtab_item_cmp);
    assert(cg->strtab.data);
    CXCursor cur = clang_getTranslationUnitCursor(unit);
    clang_visitChildren(cur, visit, NULL);

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    return cg;
}
