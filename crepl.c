/*
 * Evaluations run inside a sigsetjmp/siglongjmp guard
 * SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT, Ctrl-C 
 * and an optional timeout unwind back into the prompt instead of killing the session.
 * Only Ctrl-D kills it
 * After an error it writes the session to a file, re-execs and replays
 * every accepted snippet with output suppressed. 
 * */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef CREPL_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CREPL_VERSION "0.1"
#define ALTSTACK_SIZE (256 * 1024)

static const char PRELUDE[] =
#include "prelude_inc.h"
;
//resizeable String
typedef struct { char *p; size_t n, cap; } Str;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("crepl: out of memory\n", stderr); _exit(1); }
    return p;
}

static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n ? n : 1);
    if (!p) { fputs("crepl: out of memory\n", stderr); _exit(1); }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void str_init(Str *s) { s->p = NULL; s->n = 0; s->cap = 0; }
static void str_free(Str *s) { free(s->p); str_init(s); }
static void str_clear(Str *s) { s->n = 0; if (s->p) s->p[0] = 0; }

static void str_reserve(Str *s, size_t need)
{
    if (s->cap >= need) return;
    size_t c = s->cap ? s->cap : 128;
    while (c < need) c *= 2;
    s->p = xrealloc(s->p, c);
    s->cap = c;
}

static void str_addn(Str *s, const char *d, size_t n)
{
    str_reserve(s, s->n + n + 1);
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void str_add(Str *s, const char *d) { str_addn(s, d, strlen(d)); }
static void str_addc(Str *s, char c) { str_addn(s, &c, 1); }

static void str_addf(Str *s, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) { str_addn(s, buf, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    str_addn(s, big, (size_t)n);
    free(big);
}

static const char *str_cstr(const Str *s) { return s->p ? s->p : ""; }

// Vector of strings
typedef struct { char **v; size_t n, cap; } Vec;

static void vec_init(Vec *a) { a->v = NULL; a->n = 0; a->cap = 0; }

static void vec_push(Vec *a, const char *s)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = xrealloc(a->v, a->cap * sizeof *a->v); }
    a->v[a->n++] = xstrdup(s);
}

static void vec_free(Vec *a)
{
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    vec_init(a);
}

static int vec_has(const Vec *a, const char *s)
{
    for (size_t i = 0; i < a->n; i++) if (!strcmp(a->v[i], s)) return 1;
    return 0;
}

// Entry Table
typedef struct { char *key, *text; } Ent;
typedef struct { Ent *v; size_t n, cap; } Ents;

static void ents_init(Ents *e) { e->v = NULL; e->n = 0; e->cap = 0; }

static void ents_clear(Ents *e)
{
    for (size_t i = 0; i < e->n; i++) { free(e->v[i].key); free(e->v[i].text); }
    free(e->v);
    ents_init(e);
}

/* returns 1 if an existing entry with different text was replaced */
static int ents_put(Ents *e, const char *key, const char *text)
{
    for (size_t i = 0; i < e->n; i++) {
        if (!strcmp(e->v[i].key, key)) {
            int changed = strcmp(e->v[i].text, text) != 0;
            if (changed) { free(e->v[i].text); e->v[i].text = xstrdup(text); }
            return changed;
        }
    }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 16; e->v = xrealloc(e->v, e->cap * sizeof *e->v); }
    e->v[e->n].key = xstrdup(key);
    e->v[e->n].text = xstrdup(text);
    e->n++;
    return 0;
}

typedef enum { K_PP, K_DECL, K_EXPR, K_STMT } Kind;

typedef struct {
    char  *text;
    Kind   kind;
    void  *handle;   /* dlopen handle, may be NULL */
    char  *sopath;
    Vec    keys;     /* prologue keys this snippet owns */
} Snippet;

static struct {
    Snippet *v;
    size_t   n, cap;
} G_snips;

static Ents  G_pp;
static Ents  G_decls;
static Vec   G_cflags;
static Vec   G_ldflags;
static char  G_tmpdir[128];
static char  G_self[PATH_MAX];
static char *G_cc = NULL;
static int   G_counter = 0;
static int   G_in_reload = 0;
static int   G_interactive = 1;

static struct {
    int echo;
    int warn;
    int verbose;
    int keep;
    unsigned timeout;
} G_opt = { 1, 0, 0, 0, 0 };

/* ------------------------------------------------------------------ *
 * setjmp/longjmp guard
 * ------------------------------------------------------------------ */

static sigjmp_buf          G_env;
static volatile sig_atomic_t G_guard   = 0;
static volatile sig_atomic_t G_sig     = 0;
static volatile sig_atomic_t G_exitreq = 0;
static volatile int          G_exitcode = 0;
static void * volatile       G_fault_addr = NULL;
static volatile sig_atomic_t G_interrupted = 0;

static void fault_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)uctx;
    if (G_guard) {
        G_sig = sig;
        G_fault_addr = si ? si->si_addr : NULL;
        G_guard = 0;
        siglongjmp(G_env, 1);
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

static void interrupt_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)si; (void)uctx;
    if (G_guard) {
        G_sig = sig;
        G_fault_addr = NULL;
        G_guard = 0;
        siglongjmp(G_env, 1);
    }
    G_interrupted = 1;
}

void __crepl_trap_exit(int code)
{
    if (G_guard) {
        G_exitreq = 1;
        G_exitcode = code;
        G_guard = 0;
        siglongjmp(G_env, 2);
    }
    _exit(code);
}

static void install_handlers(void)
{
    static char altstack[ALTSTACK_SIZE];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int faults[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
    for (size_t i = 0; i < sizeof faults / sizeof *faults; i++)
        sigaction(faults[i], &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = interrupt_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

static int guarded_call(void (*fn)(void))
{
    G_sig = 0;
    G_exitreq = 0;
    G_fault_addr = NULL;

    int rc = sigsetjmp(G_env, 1);
    if (rc == 0) {
        G_guard = 1;
        if (G_opt.timeout) alarm(G_opt.timeout);
        fn();
        alarm(0);
        G_guard = 0;
        fflush(stdout);
        return 0;
    }
    alarm(0);
    G_guard = 0;
    fflush(stdout);
    return rc == 2 ? 2 : 1;
}

static int G_saved_out = -1;

static void mute_output(void)
{
    if (G_saved_out >= 0) return;
    fflush(stdout);
    G_saved_out = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
}

static void unmute_output(void)
{
    if (G_saved_out < 0) return;
    fflush(stdout);
    dup2(G_saved_out, STDOUT_FILENO);
    close(G_saved_out);
    G_saved_out = -1;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    int fd = G_saved_out >= 0 ? G_saved_out : STDOUT_FILENO;
    char buf[2048];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        fflush(stdout);
        ssize_t ignored = write(fd, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
        (void)ignored;
    }
}

static int run_cmd(char *const argv[], Str *out)
{
    int pfd[2];
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);

    char buf[4096];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof buf)) > 0)
        if (out) str_addn(out, buf, (size_t)n);
    close(pfd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128;
}

static void pick_compiler(void)
{
    const char *env = getenv("CREPL_CC");
    if (!env) env = getenv("CC");
    const char *cands[4];
    int nc = 0;
    if (env && *env) cands[nc++] = env;
    cands[nc++] = "cc";
    cands[nc++] = "clang";
    cands[nc++] = "gcc";

    for (int i = 0; i < nc; i++) {
        char *argv[] = { (char *)cands[i], (char *)"--version", NULL };
        Str out; str_init(&out);
        int rc = run_cmd(argv, &out);
        str_free(&out);
        if (rc == 0) { G_cc = xstrdup(cands[i]); return; }
    }
    fputs("crepl: no working C compiler found (tried cc, clang, gcc)\n", stderr);
    exit(1);
}

static int compile_unit(const char *unit, const char *sopath, Str *err)
{
    char cpath[PATH_MAX];
    snprintf(cpath, sizeof cpath, "%s/u%d.c", G_tmpdir, ++G_counter);

    FILE *f = fopen(cpath, "w");
    if (!f) { if (err) str_addf(err, "crepl: cannot write %s: %s\n", cpath, strerror(errno)); return -1; }
    fwrite(unit, 1, strlen(unit), f);
    fclose(f);

    Vec av; vec_init(&av);
    vec_push(&av, G_cc);
    vec_push(&av, "-x");
    vec_push(&av, "c");
    vec_push(&av, cpath);
    if (sopath) {
        vec_push(&av, "-shared");
        vec_push(&av, "-fPIC");
#ifdef __APPLE__
        vec_push(&av, "-undefined");
        vec_push(&av, "dynamic_lookup");
#endif
        vec_push(&av, "-o");
        vec_push(&av, sopath);
    } else {
        vec_push(&av, "-fsyntax-only");
    }
    vec_push(&av, "-O0");
    vec_push(&av, "-g");
    vec_push(&av, "-fno-omit-frame-pointer");
    if (G_opt.warn) vec_push(&av, "-Wall");


    vec_push(&av, "-Werror=implicit-int");
    vec_push(&av, "-Werror=implicit-function-declaration");
    vec_push(&av, "-Werror=int-conversion");
    vec_push(&av, "-Werror=incompatible-pointer-types");
    vec_push(&av, "-Wno-unused-function");
    vec_push(&av, "-Wno-unused-variable");
    vec_push(&av, "-Wno-unused-value");
    vec_push(&av, "-Wno-unused-but-set-variable");
    for (size_t i = 0; i < G_cflags.n; i++) vec_push(&av, G_cflags.v[i]);
    if (sopath) for (size_t i = 0; i < G_ldflags.n; i++) vec_push(&av, G_ldflags.v[i]);
    if (sopath) vec_push(&av, "-lm");

    char **argv = xmalloc((av.n + 1) * sizeof *argv);
    for (size_t i = 0; i < av.n; i++) argv[i] = av.v[i];
    argv[av.n] = NULL;

    Str raw; str_init(&raw);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = run_cmd(argv, &raw);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (G_opt.verbose)
        say("[cc %s -> %s : %d, %.0f ms]\n", cpath, sopath ? sopath : "(syntax)", rc,
            (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    if (err && raw.n) {
        const char *s = str_cstr(&raw);
        size_t plen = strlen(cpath);
        while (*s) {
            if (!strncmp(s, cpath, plen)) { str_add(err, "<unit>"); s += plen; }
            else str_addc(err, *s++);
        }
    }
    str_free(&raw);

    if (!G_opt.keep) unlink(cpath);
    free(argv);
    vec_free(&av);
    return rc;
}

static size_t skip_ws(const char *s, size_t i)
{
    for (;;) {
        while (s[i] && isspace((unsigned char)s[i])) i++;
        if (s[i] == '/' && s[i + 1] == '/') { while (s[i] && s[i] != '\n') i++; continue; }
        if (s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (s[i] && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (s[i]) i += 2;
            continue;
        }
        return i;
    }
}

static size_t skip_quoted(const char *s, size_t i)
{
    char q = s[i++];
    while (s[i]) {
        if (s[i] == '\\' && s[i + 1]) { i += 2; continue; }
        if (s[i] == q) { i++; break; }
        i++;
    }
    return i;
}

static size_t scan_item(const char *s, size_t i, size_t *bracep)
{
    int par = 0, brk = 0;
    *bracep = (size_t)-1;

    while (s[i]) {
        if (isspace((unsigned char)s[i]) ||
            (s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*'))) {
            i = skip_ws(s, i);
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') { i = skip_quoted(s, i); continue; }

        char c = s[i];
        if (c == '(') { par++; i++; continue; }
        if (c == ')') { if (par > 0) par--; i++; continue; }
        if (c == '[') { brk++; i++; continue; }
        if (c == ']') { if (brk > 0) brk--; i++; continue; }

        if (c == '{' && par == 0 && brk == 0) {
            if (*bracep == (size_t)-1) *bracep = i;
            size_t back = i;
            while (back > 0 && isspace((unsigned char)s[back - 1])) back--;
            int is_funcdef = (back > 0 && s[back - 1] == ')');
            int d = 0;
            while (s[i]) {
                if (s[i] == '"' || s[i] == '\'') { i = skip_quoted(s, i); continue; }
                if (s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) { i = skip_ws(s, i); continue; }
                if (s[i] == '{') { d++; i++; continue; }
                if (s[i] == '}') { d--; i++; if (d == 0) break; continue; }
                i++;
            }
            if (is_funcdef) return i;
            size_t j = skip_ws(s, i);
            if (s[j] == ';') return j + 1;
            if (s[j] && s[j] != '}' &&
                (s[j] == ',' || s[j] == '=' || s[j] == '*' ||
                 s[j] == '_' || isalpha((unsigned char)s[j]))) { i = j; continue; }
            return i;
        }
        if (c == ';' && par == 0 && brk == 0) return i + 1;
        i++;
    }
    return i;
}

static char *trim_dup(const char *s, size_t n)
{
    while (n && isspace((unsigned char)s[0])) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static int starts_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    if (strncmp(s, w, n)) return 0;
    return !isalnum((unsigned char)s[n]) && s[n] != '_';
}

static char *last_toplevel_ident(const char *s, size_t n)
{
    int par = 0, brk = 0, brc = 0;
    size_t i = 0, bs = 0, be = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_quoted(s, i); continue; }
        if (c == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) { i = skip_ws(s, i); continue; }
        if (c == '(') { par++; i++; continue; }
        if (c == ')') { par--; i++; continue; }
        if (c == '[') { brk++; i++; continue; }
        if (c == ']') { brk--; i++; continue; }
        if (c == '{') { brc++; i++; continue; }
        if (c == '}') { brc--; i++; continue; }
        if ((isalpha((unsigned char)c) || c == '_')) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (par == 0 && brk == 0 && brc == 0) { bs = i; be = j; }
            i = j;
            continue;
        }
        i++;
    }
    if (be > bs) return xstrndup(s + bs, be - bs);
    return NULL;
}

static char *strip_initialisers(const char *s, size_t n)
{
    Str out; str_init(&out);
    int par = 0, brk = 0, brc = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { size_t j = skip_quoted(s, i); str_addn(&out, s + i, j - i); i = j; continue; }
        if (c == '(') par++;
        else if (c == ')') par--;
        else if (c == '[') brk++;
        else if (c == ']') brk--;
        else if (c == '{') brc++;
        else if (c == '}') brc--;
        else if (c == '=' && par == 0 && brk == 0 && brc == 0) {
            int p2 = 0, b2 = 0, c2 = 0;
            i++;
            while (i < n) {
                char d = s[i];
                if (d == '"' || d == '\'') { i = skip_quoted(s, i); continue; }
                if (d == '(') p2++;
                else if (d == ')') p2--;
                else if (d == '[') b2++;
                else if (d == ']') b2--;
                else if (d == '{') c2++;
                else if (d == '}') c2--;
                else if (d == ',' && !p2 && !b2 && !c2) break;
                i++;
            }
            continue;
        }
        str_addc(&out, c);
        i++;
    }
    char *r = trim_dup(str_cstr(&out), out.n);
    str_free(&out);
    return r;
}

static void add_pp_line(const char *line, Vec *keys)
{
    char *t = trim_dup(line, strlen(line));
    char key[512];
    const char *p = t + 1;
    while (*p && isspace((unsigned char)*p)) p++;

    if (!strncmp(p, "define", 6)) {
        const char *q = p + 6;
        while (*q && isspace((unsigned char)*q)) q++;
        size_t l = 0;
        while (q[l] && (isalnum((unsigned char)q[l]) || q[l] == '_')) l++;
        snprintf(key, sizeof key, "def:%.*s", (int)l, q);
    } else if (!strncmp(p, "include", 7)) {
        snprintf(key, sizeof key, "inc:%s", p + 7);
    } else if (!strncmp(p, "undef", 5)) {
        snprintf(key, sizeof key, "pp:%s", t);
    } else {
        snprintf(key, sizeof key, "pp:%s", t);
    }
    ents_put(&G_pp, key, t);
    if (keys && !vec_has(keys, key)) vec_push(keys, key);
    free(t);
}

static void add_decl(const char *key, const char *text, Vec *keys)
{
    ents_put(&G_decls, key, text);
    if (keys && !vec_has(keys, key)) vec_push(keys, key);
}

/*
 * Walk a chunk of top-level C and record, for every definition it contains,
 * a declaration that later snippets can be compiled against.
 */
static void derive_decls(const char *src, Vec *keys)
{
    size_t i = 0;
    while (src[i]) {
        i = skip_ws(src, i);
        if (!src[i]) break;

        if (src[i] == '#') {
            size_t j = i;
            Str line; str_init(&line);
            while (src[j]) {
                if (src[j] == '\\' && src[j + 1] == '\n') { str_addn(&line, src + j, 2); j += 2; continue; }
                if (src[j] == '\n') break;
                str_addc(&line, src[j]); j++;
            }
            add_pp_line(str_cstr(&line), keys);
            str_free(&line);
            i = j;
            continue;
        }

        size_t brace;
        size_t end = scan_item(src, i, &brace);
        if (end <= i) break;

        char *item = trim_dup(src + i, end - i);
        size_t brel = (brace == (size_t)-1) ? (size_t)-1 : brace - i;
        if (brel != (size_t)-1) {
            const char *b = strchr(item, '{');
            brel = b ? (size_t)(b - item) : (size_t)-1;
        }

        size_t ilen = strlen(item);
        char key[640];

        if (starts_word(item, "static")) {
            free(item);
            i = end;
            continue;
        }

        if (brel == (size_t)-1) {
            size_t body = ilen;
            while (body && item[body - 1] == ';') body--;

            if (starts_word(item, "typedef")) {
                char *nm = last_toplevel_ident(item, body);
                snprintf(key, sizeof key, "typedef:%s", nm ? nm : item);
                free(nm);
                add_decl(key, item, keys);
            } else if (starts_word(item, "struct") || starts_word(item, "union") ||
                       starts_word(item, "enum")) {
                snprintf(key, sizeof key, "tag:%s", item);
                add_decl(key, item, keys);
            } else if (starts_word(item, "_Static_assert") || starts_word(item, "static_assert")) {
            } else {
                int is_proto = 0;
                {
                    int par = 0;
                    for (size_t k = 0; k < body; k++) {
                        if (item[k] == '(') { par = 1; break; }
                        if (item[k] == '=') break;
                    }
                    is_proto = par;
                }
                if (is_proto) {
                    char *nm = NULL;
                    const char *op = strchr(item, '(');
                    if (op) nm = last_toplevel_ident(item, (size_t)(op - item));
                    snprintf(key, sizeof key, "fn:%s", nm ? nm : item);
                    free(nm);
                    add_decl(key, item, keys);
                } else {
                    char *bare = strip_initialisers(item, body);
                    char *nm = last_toplevel_ident(bare, strlen(bare));
                    Str d; str_init(&d);
                    if (!starts_word(bare, "extern")) str_add(&d, "extern ");
                    str_add(&d, bare);
                    str_addc(&d, ';');
                    snprintf(key, sizeof key, "var:%s", nm ? nm : bare);
                    add_decl(key, str_cstr(&d), keys);
                    str_free(&d);
                    free(nm);
                    free(bare);
                }
            }
        } else {
            char *head = trim_dup(item, brel);
            size_t hlen = strlen(head);
            int is_func = 0;
            if (hlen && head[hlen - 1] == ')') {
                int eq = 0;
                int par = 0, brk = 0;
                for (size_t k = 0; k < hlen; k++) {
                    if (head[k] == '(') par++;
                    else if (head[k] == ')') par--;
                    else if (head[k] == '[') brk++;
                    else if (head[k] == ']') brk--;
                    else if (head[k] == '=' && !par && !brk) eq = 1;
                }
                is_func = !eq;
            }

            if (is_func) {
                char *nm = NULL;
                const char *op = strchr(head, '(');
                if (op) nm = last_toplevel_ident(head, (size_t)(op - head));
                Str d; str_init(&d);
                str_add(&d, head);
                str_addc(&d, ';');
                snprintf(key, sizeof key, "fn:%s", nm ? nm : head);
                add_decl(key, str_cstr(&d), keys);
                str_free(&d);
                free(nm);
            } else if (starts_word(head, "typedef")) {
                size_t body = ilen;
                while (body && item[body - 1] == ';') body--;
                char *nm = last_toplevel_ident(item, body);
                Str d; str_init(&d);
                str_addn(&d, item, ilen);
                if (!ilen || item[ilen - 1] != ';') str_addc(&d, ';');
                snprintf(key, sizeof key, "typedef:%s", nm ? nm : head);
                add_decl(key, str_cstr(&d), keys);
                str_free(&d);
                free(nm);
            } else if (starts_word(head, "struct") || starts_word(head, "union") ||
                       starts_word(head, "enum")) {
                const char *close = strrchr(item, '}');
                size_t tlen = close ? (size_t)(close - item) + 1 : ilen;
                Str d; str_init(&d);
                str_addn(&d, item, tlen);
                str_addc(&d, ';');
                snprintf(key, sizeof key, "tag:%s", head);
                add_decl(key, str_cstr(&d), keys);
                str_free(&d);

                if (close && close[1]) {
                    char *rest = trim_dup(close + 1, ilen - tlen);
                    size_t rl = strlen(rest);
                    while (rl && rest[rl - 1] == ';') rl--;
                    if (rl) {
                        char *bare = strip_initialisers(rest, rl);
                        char *nm = last_toplevel_ident(bare, strlen(bare));
                        Str v; str_init(&v);
                        str_addf(&v, "extern %s %s;", head, bare);
                        snprintf(key, sizeof key, "var:%s", nm ? nm : bare);
                        add_decl(key, str_cstr(&v), keys);
                        str_free(&v);
                        free(nm);
                        free(bare);
                    }
                    free(rest);
                }
            } else {
                char *bare = strip_initialisers(item, ilen);
                size_t bl = strlen(bare);
                while (bl && bare[bl - 1] == ';') bare[--bl] = 0;
                char *nm = last_toplevel_ident(bare, bl);
                Str d; str_init(&d);
                if (!starts_word(bare, "extern")) str_add(&d, "extern ");
                str_add(&d, bare);
                str_addc(&d, ';');
                snprintf(key, sizeof key, "var:%s", nm ? nm : bare);
                add_decl(key, str_cstr(&d), keys);
                str_free(&d);
                free(nm);
                free(bare);
            }
            free(head);
        }

        free(item);
        i = end;
    }
}

static void build_header(Str *u)
{
    str_add(u, PRELUDE);
    str_addc(u, '\n');
    for (size_t i = 0; i < G_pp.n; i++)    { str_add(u, G_pp.v[i].text);    str_addc(u, '\n'); }
    for (size_t i = 0; i < G_decls.n; i++) { str_add(u, G_decls.v[i].text); str_addc(u, '\n'); }
}

static void add_user_line(Str *u, const char *text)
{
    str_add(u, "#line 1 \"<repl>\"\n");
    str_add(u, text);
    str_add(u, "\n#line 100000 \"<crepl>\"\n");
}

static Snippet *snips_push(const char *text, Kind kind)
{
    if (G_snips.n == G_snips.cap) {
        G_snips.cap = G_snips.cap ? G_snips.cap * 2 : 16;
        G_snips.v = xrealloc(G_snips.v, G_snips.cap * sizeof *G_snips.v);
    }
    Snippet *s = &G_snips.v[G_snips.n++];
    memset(s, 0, sizeof *s);
    s->text = xstrdup(text);
    s->kind = kind;
    vec_init(&s->keys);
    return s;
}

static void snips_drop_from(size_t idx)
{
    for (size_t i = idx; i < G_snips.n; i++) {
        Snippet *s = &G_snips.v[i];
        if (s->handle) dlclose(s->handle);
        free(s->text);
        free(s->sopath);
        vec_free(&s->keys);
    }
    G_snips.n = idx;
}

static void (*G_run_fn)(void) = NULL;
static void call_run_fn(void) { G_run_fn(); }

static int eval_text(const char *text, int quiet);
static int reload_session(long drop_index);

/*
 * Keep only the diagnostic groups anchored in the user's own text.  Snippets
 * are compiled with `#line 1 "<repl>"` in front, so anything reported against
 * "<unit>" is a complaint about crepl's wrapping, not about their code.
 * Continuation lines (source quotes, carets) are kept with their header.
 */
static void clean_diag(const Str *in, Str *out)
{
    const char *s = str_cstr(in);
    int keep = 0;

    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);

        char line[1024];
        size_t l = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, s, l);
        line[l] = 0;

        if (!strncmp(line, "<repl>:", 7)) {
            keep = !strstr(line, "__crepl_run_") &&
                   !strstr(line, "__crepl_typeprobe") &&
                   !strstr(line, "__crepl_v");
        } else if (line[0] && !isspace((unsigned char)line[0])) {
            keep = 0;
        }

        if (keep) { str_addn(out, s, len); str_addc(out, '\n'); }
        if (!nl) break;
        s = nl + 1;
    }
}

static int diag_score(const Str *d)
{
    if (!d->n) return INT_MAX;
    int n = 0;
    const char *s = str_cstr(d);
    while ((s = strstr(s, "error:")) != NULL) { n++; s += 6; }
    if (!n) n = 1;
    const char *t = str_cstr(d);
    if (strstr(t, "expected declaration specifiers")) n += 10;
    if (strstr(t, "expected identifier or '('")) n += 10;
    if (strstr(t, "function definition is not allowed here")) n += 10;
    if (strstr(t, "data definition has no type")) n += 10;
    if (strstr(t, "expected expression before")) n += 10;
    if (strstr(t, "expected expression")) n += 4;
    return n;
}

static void print_diag(Str *err)
{
    if (!err->n) return;
    Str c; str_init(&c);
    clean_diag(err, &c);
    if (G_in_reload) {
        const char *t = str_cstr(&c);
        const char *nl = strchr(t, '\n');
        if (*t) say("replay: %.*s\n", nl ? (int)(nl - t) : (int)strlen(t), t);
        str_free(&c);
        return;
    }
    if (!c.n) {
        fflush(stdout);
        fputs("crepl: could not interpret that as a declaration, an expression "
              "or a statement\n", stderr);
        fflush(stderr);
        str_free(&c);
        return;
    }
    fflush(stdout);
    fputs(str_cstr(&c), stderr);
    if (c.p[c.n - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    str_free(&c);
}

static int has_inner_semicolon(const char *s)
{
    int par = 0, brk = 0, brc = 0;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_quoted(s, i); continue; }
        if (c == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) { i = skip_ws(s, i); continue; }
        if (c == '(') par++;
        else if (c == ')') par--;
        else if (c == '[') brk++;
        else if (c == ']') brk--;
        else if (c == '{') brc++;
        else if (c == '}') brc--;
        else if (c == ';' && !par && !brk && !brc) {
            size_t j = skip_ws(s, i + 1);
            if (j < n) return 1;
        }
        i++;
    }
    return 0;
}

static char *strip_trailing_semi(const char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    while (n && s[n - 1] == ';') {
        n--;
        while (n && isspace((unsigned char)s[n - 1])) n--;
    }
    return xstrndup(s, n);
}

static void report_fault(int how)
{
    fflush(stdout);
    if (how == 2) {
        fprintf(stderr, "\n*** snippet called exit(%d) -- session kept alive\n", G_exitcode);
        fflush(stderr);
        return;
    }
    int sig = (int)G_sig;
    const char *name = strsignal(sig);
    fprintf(stderr, "\n*** %s (signal %d)", name ? name : "fatal signal", sig);
    if (sig == SIGSEGV || sig == SIGBUS)
        fprintf(stderr, " at address %p", (void *)G_fault_addr);
    if (sig == SIGALRM)
        fprintf(stderr, " -- exceeded the %u s limit", G_opt.timeout);
    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * Ask the user what to do after a fault.
 * Returns: 0 keep going, 1 drop snippet, 2 soft reload, 3 hard restart, 4 quit.
 */
static int recovery_prompt(int how, int snippet_added)
{
    int fatal = (how == 1 && G_sig != SIGINT && G_sig != SIGALRM);

    if (!G_interactive) {
        say("[non-interactive: dropping the snippet and continuing]\n");
        return snippet_added ? 1 : 0;
    }

    for (;;) {
        fflush(stdout);
        fprintf(stderr,
                "\nrecover: [k]eep going  [d]rop snippet  [r]eload session"
                "  [R]estart process%s  [q]uit\n"
                "         %s\n> ",
                fatal ? " (recommended)" : "",
                fatal ? "memory may be corrupt; R re-execs and replays the session"
                      : "state looks fine; k resumes, d also forgets the snippet");
        fflush(stderr);

        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
        if (n <= 0) { fputc('\n', stderr); return 4; }
        buf[n] = 0;

        char *p = buf;
        while (*p && isspace((unsigned char)*p)) p++;
        switch (*p) {
        case 'k': case 'c': return 0;
        case 'd': return snippet_added ? 1 : 0;
        case 'r': return 2;
        case 'R': return 3;
        case 'q': return 4;
        case 0:   return fatal ? 3 : 0;
        default:  fprintf(stderr, "  (k, d, r, R or q)\n");
        }
    }
}

static void dump_session(const char *path, long drop_index);
static void hard_restart(long drop_index);
static void cleanup_tmp(void);

static int handle_fault(int how, long snip_index)
{
    report_fault(how);
    if (G_in_reload) return 0;

    int choice = recovery_prompt(how, snip_index >= 0);
    switch (choice) {
    case 0:
        return 0;
    case 1:
        if (snip_index >= 0) {
            snips_drop_from((size_t)snip_index);
            say("dropped; %zu snippet(s) still live\n", G_snips.n);
        }
        return 0;
    case 2:
        reload_session(snip_index);
        return 0;
    case 3:
        hard_restart(snip_index);
        return 0;
    default:
        return 1;
    }
}

static int split_decl_init(const char *text, char **bare_out, char **init_out)
{
    size_t n = strlen(text);
    int par = 0, brk = 0, brc = 0, found = 0;
    Str inits; str_init(&inits);
    size_t i = 0;

    while (i < n) {
        char c = text[i];
        if (c == '"' || c == '\'') { i = skip_quoted(text, i); continue; }
        if (c == '/' && (text[i + 1] == '/' || text[i + 1] == '*')) { i = skip_ws(text, i); continue; }
        if (c == '(') { par++; i++; continue; }
        if (c == ')') { par--; i++; continue; }
        if (c == '[') { brk++; i++; continue; }
        if (c == ']') { brk--; i++; continue; }
        if (c == '{') { brc++; i++; continue; }
        if (c == '}') { brc--; i++; continue; }

        if (c == '=' && !par && !brk && !brc &&
            text[i + 1] != '=' && (i == 0 || !strchr("=!<>+-*/%&|^", text[i - 1]))) {
            char *nm = last_toplevel_ident(text, i);
            if (!nm) { str_free(&inits); return 0; }

            size_t j = skip_ws(text, i + 1);
            if (text[j] == '{') { free(nm); str_free(&inits); return 0; }

            int p2 = 0, b2 = 0, c2 = 0;
            size_t k = j;
            while (k < n) {
                char d = text[k];
                if (d == '"' || d == '\'') { k = skip_quoted(text, k); continue; }
                if (d == '(') p2++;
                else if (d == ')') p2--;
                else if (d == '[') b2++;
                else if (d == ']') b2--;
                else if (d == '{') c2++;
                else if (d == '}') c2--;
                else if ((d == ',' || d == ';') && !p2 && !b2 && !c2) break;
                k++;
            }
            char *val = trim_dup(text + j, k - j);
            if (!*val) { free(val); free(nm); str_free(&inits); return 0; }
            str_addf(&inits, "%s = %s;\n", nm, val);
            free(val);
            free(nm);
            found = 1;
            i = k;
            continue;
        }
        i++;
    }

    if (!found) { str_free(&inits); return 0; }

    char *bare = strip_initialisers(text, n);
    size_t bl = strlen(bare);
    while (bl && (bare[bl - 1] == ';' || isspace((unsigned char)bare[bl - 1]))) bare[--bl] = 0;
    if (!bl) { free(bare); str_free(&inits); return 0; }

    *bare_out = bare;
    *init_out = inits.p ? inits.p : xstrdup("");
    return 1;
}

static int all_directives(const char *s)
{
    size_t i = 0;
    for (;;) {
        i = skip_ws(s, i);
        if (!s[i]) return 1;
        if (s[i] != '#') return 0;
        while (s[i]) {
            if (s[i] == '\\' && s[i + 1]) { i += 2; continue; }
            if (s[i] == '\n') break;
            i++;
        }
    }
}

static int eval_text(const char *text, int quiet)
{
    while (*text && isspace((unsigned char)*text)) text++;
    if (!*text) return 0;

    if (*text == '#' && all_directives(text)) {
        Ents save_pp = G_pp;
        Ents copy; ents_init(&copy);
        for (size_t i = 0; i < save_pp.n; i++) ents_put(&copy, save_pp.v[i].key, save_pp.v[i].text);

        Vec keys; vec_init(&keys);
        derive_decls(text, &keys);

        Str u; str_init(&u);
        build_header(&u);
        Str err; str_init(&err);
        int rc = compile_unit(str_cstr(&u), NULL, &err);
        str_free(&u);

        if (rc != 0) {
            print_diag(&err);
            ents_clear(&G_pp);
            G_pp = copy;
            vec_free(&keys);
            str_free(&err);
            return 1;
        }
        ents_clear(&copy);
        str_free(&err);
        Snippet *s = snips_push(text, K_PP);
        s->keys = keys;
        return 0;
    }

    Str hdr; str_init(&hdr);
    build_header(&hdr);

    Str u; str_init(&u);
    Str err; str_init(&err);

    str_addn(&u, hdr.p, hdr.n);
    add_user_line(&u, text);
    int decl_ok = (compile_unit(str_cstr(&u), NULL, &err) == 0);

    if (decl_ok) {
        char so[PATH_MAX];
        snprintf(so, sizeof so, "%s/s%d.so", G_tmpdir, G_counter + 1);
        str_clear(&err);
        if (compile_unit(str_cstr(&u), so, &err) != 0) {
            print_diag(&err);
            str_free(&u); str_free(&hdr); str_free(&err);
            return 1;
        }
        if (err.n && G_opt.warn) print_diag(&err);

        void *h = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fflush(stdout);
            fprintf(stderr, "crepl: dlopen failed: %s\n", dlerror());
            str_free(&u); str_free(&hdr); str_free(&err);
            return 1;
        }

        Vec keys; vec_init(&keys);
        derive_decls(text, &keys);

        long superseded = -1;
        for (size_t i = 0; i < G_snips.n && superseded < 0; i++)
            for (size_t k = 0; k < keys.n; k++)
                if (vec_has(&G_snips.v[i].keys, keys.v[k])) { superseded = (long)i; break; }

        Snippet *s = snips_push(text, K_DECL);
        s->handle = h;
        s->sopath = xstrdup(so);
        s->keys = keys;

        str_free(&u); str_free(&hdr); str_free(&err);

        if (superseded >= 0 && !G_in_reload) {
            if (!quiet)
                say("note: this supersedes snippet #%ld; reloading the session so the new "
                    "definition wins\n", superseded + 1);
            size_t last = G_snips.n - 1;
            char **texts = xmalloc(G_snips.n * sizeof *texts);
            size_t nt = 0;
            for (size_t i = 0; i < G_snips.n; i++) {
                if (i == last) continue;
                texts[nt++] = xstrdup(i == (size_t)superseded
                                      ? G_snips.v[last].text
                                      : G_snips.v[i].text);
            }
            snips_drop_from(0);
            ents_clear(&G_pp);
            ents_clear(&G_decls);

            G_in_reload = 1;
            mute_output();
            for (size_t i = 0; i < nt; i++) eval_text(texts[i], 1);
            unmute_output();
            G_in_reload = 0;

            for (size_t i = 0; i < nt; i++) free(texts[i]);
            free(texts);
        }
        return 0;
    }

    {
        char *bare = NULL, *inits = NULL;
        if (split_decl_init(text, &bare, &inits)) {
            char fn2[64];
            snprintf(fn2, sizeof fn2, "__crepl_run_%d", G_counter + 1);

            Str du; str_init(&du);
            str_addn(&du, hdr.p, hdr.n);
            str_addf(&du, "%s;\n", bare);
            str_addf(&du, "void %s(void)\n{\n", fn2);
            add_user_line(&du, inits);
            str_add(&du, "fflush(stdout);\n}\n");

            Str derr; str_init(&derr);
            if (compile_unit(str_cstr(&du), NULL, &derr) == 0) {
                char so2[PATH_MAX];
                snprintf(so2, sizeof so2, "%s/s%d.so", G_tmpdir, G_counter + 1);
                str_clear(&derr);
                if (compile_unit(str_cstr(&du), so2, &derr) == 0) {
                    void *h2 = dlopen(so2, RTLD_NOW | RTLD_GLOBAL);
                    void (*rf)(void) = h2 ? (void (*)(void))(uintptr_t)dlsym(h2, fn2) : NULL;
                    if (h2 && rf) {
                        Str bd; str_init(&bd);
                        str_addf(&bd, "%s;", bare);
                        Vec keys; vec_init(&keys);
                        derive_decls(str_cstr(&bd), &keys);
                        str_free(&bd);

                        Snippet *sn = snips_push(text, K_DECL);
                        sn->handle = h2;
                        sn->sopath = xstrdup(so2);
                        sn->keys = keys;
                        long idx = (long)G_snips.n - 1;

                        free(bare); free(inits);
                        str_free(&du); str_free(&derr);
                        str_free(&u); str_free(&hdr); str_free(&err);

                        G_run_fn = rf;
                        int how2 = guarded_call(call_run_fn);
                        if (how2 != 0) {
                            if (G_in_reload) { report_fault(how2); return 2; }
                            return handle_fault(how2, idx) ? 3 : 2;
                        }
                        return 0;
                    }
                    if (h2) dlclose(h2);
                }
            }
            str_free(&du);
            str_free(&derr);
            free(bare);
            free(inits);
        }
    }

    Str decl_err; str_init(&decl_err);
    str_addn(&decl_err, err.p ? err.p : "", err.n);

    int try_expr = !has_inner_semicolon(text);
    char *expr = try_expr ? strip_trailing_semi(text) : NULL;

    char fname[64];
    snprintf(fname, sizeof fname, "__crepl_run_%d", G_counter + 1);

    typedef enum { V_SHOW, V_BYTES, V_VOID, V_STMT, V_NONE } Variant;
    Variant chosen = V_NONE;

    Str best; str_init(&best);
    int best_score = INT_MAX;

    for (int attempt = 0; attempt < 4 && chosen == V_NONE; attempt++) {
        if (attempt < 3 && !try_expr) continue;
        if (attempt < 3 && (!expr || !*expr)) continue;

        str_clear(&u);
        str_addn(&u, hdr.p, hdr.n);
        str_addf(&u, "void %s(void)\n{\n", fname);

        if (attempt == 0) {
            str_add(&u, "__auto_type __crepl_v = (\n");
            add_user_line(&u, expr);
            str_add(&u, ");\n");
            if (G_opt.echo) str_add(&u, "fputs(\"=> \", stdout); __CREPL_SHOW(__crepl_v); fputc('\\n', stdout);\n");
            else str_add(&u, "(void)__crepl_v;\n");
        } else if (attempt == 1) {
            str_add(&u, "__auto_type __crepl_v = (\n");
            add_user_line(&u, expr);
            str_add(&u, ");\n");
            if (G_opt.echo)
                str_add(&u, "fputs(\"=> \", stdout); __crepl_pr_bytes(&__crepl_v, sizeof __crepl_v); fputc('\\n', stdout);\n");
            else str_add(&u, "(void)__crepl_v;\n");
        } else if (attempt == 2) {
            str_add(&u, "(void)(\n");
            add_user_line(&u, expr);
            str_add(&u, ");\n");
        } else {
            str_add(&u, "{\n");
            add_user_line(&u, text);
            str_add(&u, "}\n");
        }
        str_add(&u, "fflush(stdout);\n}\n");

        str_clear(&err);
        if (compile_unit(str_cstr(&u), NULL, &err) == 0) {
            chosen = (Variant)attempt;
        } else {
            Str c; str_init(&c);
            clean_diag(&err, &c);
            int score = diag_score(&c);
            if (score < best_score) { best_score = score; str_clear(&best); str_addn(&best, c.p ? c.p : "", c.n); }
            str_free(&c);
        }
    }

    if (chosen == V_NONE) {
        Str dc; str_init(&dc);
        clean_diag(&decl_err, &dc);
        int dscore = diag_score(&dc);
        {
            size_t tl = strlen(text);
            while (tl && isspace((unsigned char)text[tl - 1])) tl--;
            if (tl && text[tl - 1] == '}' && dscore != INT_MAX) dscore -= 5;
        }
        if (dscore <= best_score) print_diag(&dc);
        else if (best.n) print_diag(&best);
        else print_diag(&dc);
        str_free(&dc);
        free(expr);
        str_free(&best);
        str_free(&u); str_free(&hdr); str_free(&err); str_free(&decl_err);
        return 1;
    }
    str_free(&best);
    str_free(&decl_err);

    char so[PATH_MAX];
    snprintf(so, sizeof so, "%s/s%d.so", G_tmpdir, G_counter + 1);
    str_clear(&err);
    if (compile_unit(str_cstr(&u), so, &err) != 0) {
        print_diag(&err);
        free(expr);
        str_free(&u); str_free(&hdr); str_free(&err);
        return 1;
    }
    if (err.n && G_opt.warn) print_diag(&err);

    void *h = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fflush(stdout);
        fprintf(stderr, "crepl: dlopen failed: %s\n", dlerror());
        free(expr);
        str_free(&u); str_free(&hdr); str_free(&err);
        return 1;
    }

    void (*fn)(void) = (void (*)(void))(uintptr_t)dlsym(h, fname);
    if (!fn) {
        fflush(stdout);
        fprintf(stderr, "crepl: cannot find %s: %s\n", fname, dlerror());
        dlclose(h);
        free(expr);
        str_free(&u); str_free(&hdr); str_free(&err);
        return 1;
    }

    Snippet *s = snips_push(text, chosen == V_STMT ? K_STMT : K_EXPR);
    s->handle = h;
    s->sopath = xstrdup(so);
    long my_index = (long)G_snips.n - 1;

    free(expr);
    str_free(&u); str_free(&hdr); str_free(&err);

    G_run_fn = fn;
    int how = guarded_call(call_run_fn);
    if (how != 0) {
        if (G_in_reload) { report_fault(how); return 2; }
        return handle_fault(how, my_index) ? 3 : 2;
    }
    return 0;
}

static int reload_session(long drop_index)
{
    size_t n = G_snips.n;
    char **texts = xmalloc((n ? n : 1) * sizeof *texts);
    size_t nt = 0;
    for (size_t i = 0; i < n; i++) {
        if ((long)i == drop_index) continue;
        texts[nt++] = xstrdup(G_snips.v[i].text);
    }

    snips_drop_from(0);
    ents_clear(&G_pp);
    ents_clear(&G_decls);

    G_in_reload = 1;
    mute_output();
    size_t failed = 0;
    for (size_t i = 0; i < nt; i++)
        if (eval_text(texts[i], 1) != 0) failed++;
    unmute_output();
    G_in_reload = 0;

    for (size_t i = 0; i < nt; i++) free(texts[i]);
    free(texts);

    say("reloaded %zu snippet(s)%s\n", G_snips.n,
        failed ? " (some were skipped)" : "");
    return 0;
}

static void dump_session(const char *path, long drop_index)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "CC %s\n", G_cc);
    for (size_t i = 0; i < G_cflags.n; i++)  fprintf(f, "FLAG %s\n", G_cflags.v[i]);
    for (size_t i = 0; i < G_ldflags.n; i++) fprintf(f, "LIB %s\n", G_ldflags.v[i]);
    fprintf(f, "SET echo %d\nSET warn %d\nSET verbose %d\nSET timeout %u\n",
            G_opt.echo, G_opt.warn, G_opt.verbose, G_opt.timeout);
    for (size_t i = 0; i < G_snips.n; i++) {
        if ((long)i == drop_index) continue;
        size_t len = strlen(G_snips.v[i].text);
        fprintf(f, "SNIP %zu\n", len);
        fwrite(G_snips.v[i].text, 1, len, f);
        fputc('\n', f);
    }
    fclose(f);
}

static void hard_restart(long drop_index)
{
    char dump[PATH_MAX];
    snprintf(dump, sizeof dump, "/tmp/crepl-session-%d.txt", (int)getpid());
    dump_session(dump, drop_index);

    say("restarting and replaying %zu snippet(s)...\n",
        G_snips.n - (drop_index >= 0 ? 1 : 0));

    cleanup_tmp();
    fflush(NULL);

    char *argv[4];
    argv[0] = G_self;
    argv[1] = (char *)"--replay";
    argv[2] = dump;
    argv[3] = NULL;
    execv(G_self, argv);

    fprintf(stderr, "crepl: could not re-exec %s: %s\n", G_self, strerror(errno));
    fprintf(stderr, "crepl: falling back to an in-process reload\n");
    reload_session(drop_index);
}

static int load_dump(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "crepl: cannot read %s\n", path); return -1; }

    Vec texts; vec_init(&texts);
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "CC ", 3)) { free(G_cc); G_cc = xstrdup(line + 3); }
        else if (!strncmp(line, "FLAG ", 5)) vec_push(&G_cflags, line + 5);
        else if (!strncmp(line, "LIB ", 4))  vec_push(&G_ldflags, line + 4);
        else if (!strncmp(line, "SET ", 4)) {
            char name[64]; unsigned val;
            if (sscanf(line + 4, "%63s %u", name, &val) == 2) {
                if (!strcmp(name, "echo")) G_opt.echo = (int)val;
                else if (!strcmp(name, "warn")) G_opt.warn = (int)val;
                else if (!strcmp(name, "verbose")) G_opt.verbose = (int)val;
                else if (!strcmp(name, "timeout")) G_opt.timeout = val;
            }
        } else if (!strncmp(line, "SNIP ", 5)) {
            size_t len = strtoul(line + 5, NULL, 10);
            char *buf = xmalloc(len + 1);
            size_t got = fread(buf, 1, len, f);
            buf[got] = 0;
            int c = fgetc(f); (void)c;
            vec_push(&texts, buf);
            free(buf);
        }
    }
    fclose(f);
    unlink(path);

    if (texts.n) {
        say("replaying %zu snippet(s)...\n", texts.n);
        G_in_reload = 1;
        mute_output();
        size_t bad = 0;
        for (size_t i = 0; i < texts.n; i++)
            if (eval_text(texts.v[i], 1) != 0) bad++;
        unmute_output();
        G_in_reload = 0;
        say("session restored: %zu live, %zu skipped\n", G_snips.n, bad);
    }
    vec_free(&texts);
    return 0;
}

static void cleanup_tmp(void)
{
    if (G_opt.keep || !G_tmpdir[0]) return;
    DIR *d = opendir(G_tmpdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", G_tmpdir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(G_tmpdir);
    G_tmpdir[0] = 0;
}

static void on_exit_cleanup(void) { cleanup_tmp(); }

int main(int argc, char **argv);

static const char *HELP =
"crepl " CREPL_VERSION " -- a C REPL that recovers from faults\n"
"\n"
"Input\n"
"  #include <stdio.h>      directives join the persistent prologue\n"
"  int x = 41;             top-level declarations become session globals\n"
"  x + 1                   an expression is evaluated and its value printed\n"
"  for (...) { ... }       anything else runs as a statement\n"
"  :{ ... :}               bracket a multi-line block (or just leave braces open)\n"
"\n"
"Commands\n"
"  :help                   this text\n"
"  :quit                   leave\n"
"  :list                   numbered session snippets\n"
"  :prologue               show the declarations every snippet is compiled against\n"
"  :type EXPR              print the static type of EXPR\n"
"  :load FILE              compile a .c file into the session\n"
"  :save FILE              write the session out as a C file\n"
"  :main [args...]         call main() if a loaded file defines one\n"
"  :decl TEXT              force TEXT into the prologue verbatim\n"
"  :undo [n]               drop the last n snippets and reload\n"
"  :reload                 rebuild the session from scratch, in process\n"
"  :restart                re-exec crepl and replay the session\n"
"  :reset                  throw the session away\n"
"  :cflags -I. -DFOO=1     add compiler flags\n"
"  :libs -lcurl            add link flags\n"
"  :set OPT [VALUE]        echo|warn|verbose|keep|timeout\n"
"\n"
"After a fault you are asked what to do.  'keep' resumes in the same process,\n"
"'restart' re-execs and replays the session into a clean one.\n";

static void cmd_list(void)
{
    if (!G_snips.n) { printf("(session is empty)\n"); return; }
    static const char *kn[] = { "pp", "decl", "expr", "stmt" };
    for (size_t i = 0; i < G_snips.n; i++) {
        printf("%3zu  %-4s  ", i + 1, kn[G_snips.v[i].kind]);
        const char *t = G_snips.v[i].text;
        for (; *t; t++) { putchar(*t == '\n' ? ' ' : *t); }
        putchar('\n');
    }
}

static void cmd_prologue(void)
{
    for (size_t i = 0; i < G_pp.n; i++) printf("%s\n", G_pp.v[i].text);
    for (size_t i = 0; i < G_decls.n; i++) printf("%s\n", G_decls.v[i].text);
    if (!G_pp.n && !G_decls.n) printf("(nothing beyond the built-in prelude)\n");
}

static void cmd_type(const char *expr)
{
    Str u; str_init(&u);
    build_header(&u);
    str_add(&u, "struct __crepl_T { int __x; };\n");
    str_add(&u, "void __crepl_typeprobe(void)\n{\n__auto_type __crepl_v = (\n");
    add_user_line(&u, expr);
    str_add(&u, ");\n__crepl_v = (struct __crepl_T){0};\n(void)__crepl_v;\n}\n");

    Str err; str_init(&err);
    int rc = compile_unit(str_cstr(&u), NULL, &err);
    str_free(&u);

    if (rc == 0) { printf("(could not determine the type)\n"); str_free(&err); return; }

    const char *s = str_cstr(&err);
    const char *best = NULL;
    size_t blen = 0;
    while ((s = strchr(s, '\'')) != NULL) {
        const char *e = strchr(s + 1, '\'');
        if (!e) break;
        size_t l = (size_t)(e - s - 1);
        int internal = (l >= 8 && !strncmp(s + 1, "__crepl_", 8)) ||
                       (l >= 15 && !strncmp(s + 1, "struct __crepl_", 15));
        if (l && !internal) { best = s + 1; blen = l; break; }
        s = e + 1;
    }
    if (best) printf("%.*s\n", (int)blen, best);
    else {
        if (strstr(str_cstr(&err), "void value not ignored") ||
            strstr(str_cstr(&err), "initializer") ||
            strstr(str_cstr(&err), "void"))
            printf("void\n");
        else print_diag(&err);
    }
    str_free(&err);
}

static void cmd_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "crepl: cannot write %s: %s\n", path, strerror(errno)); return; }
    fprintf(f, "/* generated by crepl */\n");
    for (size_t i = 0; i < G_pp.n; i++) fprintf(f, "%s\n", G_pp.v[i].text);
    fputc('\n', f);
    for (size_t i = 0; i < G_snips.n; i++) {
        if (G_snips.v[i].kind == K_PP) continue;
        if (G_snips.v[i].kind == K_DECL) fprintf(f, "%s\n\n", G_snips.v[i].text);
    }
    fprintf(f, "int main(void)\n{\n");
    for (size_t i = 0; i < G_snips.n; i++) {
        Kind k = G_snips.v[i].kind;
        if (k == K_STMT) fprintf(f, "    %s\n", G_snips.v[i].text);
        else if (k == K_EXPR) fprintf(f, "    (void)(%s);\n", G_snips.v[i].text);
    }
    fprintf(f, "    return 0;\n}\n");
    fclose(f);
    printf("wrote %s\n", path);
}

static void cmd_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "crepl: cannot open %s: %s\n", path, strerror(errno)); return; }
    Str src; str_init(&src);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) str_addn(&src, buf, n);
    fclose(f);
    if (eval_text(str_cstr(&src), 0) == 0) printf("loaded %s\n", path);
    str_free(&src);
}

static int G_main_argc;
static char **G_main_argv;
static int (*G_main_fn)(int, char **);
static int G_main_ret;
static void call_main(void) { G_main_ret = G_main_fn(G_main_argc, G_main_argv); }

static void cmd_main(char *rest)
{
    void *sym = NULL;
    for (size_t i = G_snips.n; i-- > 0 && !sym; )
        if (G_snips.v[i].handle) sym = dlsym(G_snips.v[i].handle, "main");
    if (sym == (void *)(uintptr_t)&main) sym = NULL;
    if (!sym) {
        fprintf(stderr, "crepl: no main() in the session (\":load\" a file that defines one)\n");
        return;
    }

    Vec args; vec_init(&args);
    vec_push(&args, "crepl-main");
    char *tok = strtok(rest, " \t");
    while (tok) { vec_push(&args, tok); tok = strtok(NULL, " \t"); }

    char **argv = xmalloc((args.n + 1) * sizeof *argv);
    for (size_t i = 0; i < args.n; i++) argv[i] = args.v[i];
    argv[args.n] = NULL;

    G_main_fn = (int (*)(int, char **))(uintptr_t)sym;
    G_main_argc = (int)args.n;
    G_main_argv = argv;
    G_main_ret = 0;

    int how = guarded_call(call_main);
    if (how == 0) printf("main returned %d\n", G_main_ret);
    else handle_fault(how, -1);

    free(argv);
    vec_free(&args);
}

static void cmd_set(char *rest)
{
    char name[64] = { 0 };
    char val[64] = { 0 };
    int nf = sscanf(rest, "%63s %63s", name, val);
    if (nf < 1) {
        printf("echo=%d warn=%d verbose=%d keep=%d timeout=%u\n",
               G_opt.echo, G_opt.warn, G_opt.verbose, G_opt.keep, G_opt.timeout);
        return;
    }
    int on = 1;
    if (nf >= 2) {
        if (!strcmp(val, "off") || !strcmp(val, "0") || !strcmp(val, "no")) on = 0;
        else if (!strcmp(val, "on") || !strcmp(val, "1") || !strcmp(val, "yes")) on = 1;
        else on = atoi(val);
    }
    if (!strcmp(name, "echo")) G_opt.echo = on;
    else if (!strcmp(name, "warn")) G_opt.warn = on;
    else if (!strcmp(name, "verbose")) G_opt.verbose = on;
    else if (!strcmp(name, "keep")) G_opt.keep = on;
    else if (!strcmp(name, "timeout")) G_opt.timeout = (unsigned)(nf >= 2 ? atoi(val) : 0);
    else { fprintf(stderr, "crepl: unknown option %s\n", name); return; }
    printf("echo=%d warn=%d verbose=%d keep=%d timeout=%u\n",
           G_opt.echo, G_opt.warn, G_opt.verbose, G_opt.keep, G_opt.timeout);
}

static int do_meta(char *line)
{
    char *cmd = line + 1;
    char *rest = cmd;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) { *rest++ = 0; while (*rest && isspace((unsigned char)*rest)) rest++; }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) fputs(HELP, stdout);
    else if (!strcmp(cmd, "quit") || !strcmp(cmd, "q") || !strcmp(cmd, "exit")) return 1;
    else if (!strcmp(cmd, "list") || !strcmp(cmd, "l")) cmd_list();
    else if (!strcmp(cmd, "prologue")) cmd_prologue();
    else if (!strcmp(cmd, "type") || !strcmp(cmd, "t")) {
        if (*rest) cmd_type(rest); else fprintf(stderr, "usage: :type EXPR\n");
    }
    else if (!strcmp(cmd, "load")) { if (*rest) cmd_load(rest); else fprintf(stderr, "usage: :load FILE\n"); }
    else if (!strcmp(cmd, "save")) { if (*rest) cmd_save(rest); else fprintf(stderr, "usage: :save FILE\n"); }
    else if (!strcmp(cmd, "main")) cmd_main(rest);
    else if (!strcmp(cmd, "decl")) {
        if (*rest) { Vec k; vec_init(&k); derive_decls(rest, &k);
                     Snippet *s = snips_push(rest, K_DECL); s->keys = k; printf("ok\n"); }
        else fprintf(stderr, "usage: :decl TEXT\n");
    }
    else if (!strcmp(cmd, "undo")) {
        long k = *rest ? atol(rest) : 1;
        if (k < 1) k = 1;
        if ((size_t)k > G_snips.n) k = (long)G_snips.n;
        if (!G_snips.n) { printf("(nothing to undo)\n"); return 0; }
        snips_drop_from(G_snips.n - (size_t)k);
        reload_session(-1);
    }
    else if (!strcmp(cmd, "reload")) reload_session(-1);
    else if (!strcmp(cmd, "restart")) hard_restart(-1);
    else if (!strcmp(cmd, "reset")) {
        snips_drop_from(0);
        ents_clear(&G_pp);
        ents_clear(&G_decls);
        printf("session cleared\n");
    }
    else if (!strcmp(cmd, "cflags")) {
        if (!*rest) { for (size_t i = 0; i < G_cflags.n; i++) printf("%s ", G_cflags.v[i]); printf("\n"); }
        else { char *t = strtok(rest, " \t"); while (t) { vec_push(&G_cflags, t); t = strtok(NULL, " \t"); } printf("ok\n"); }
    }
    else if (!strcmp(cmd, "libs")) {
        if (!*rest) { for (size_t i = 0; i < G_ldflags.n; i++) printf("%s ", G_ldflags.v[i]); printf("\n"); }
        else { char *t = strtok(rest, " \t"); while (t) { vec_push(&G_ldflags, t); t = strtok(NULL, " \t"); } printf("ok\n"); }
    }
    else if (!strcmp(cmd, "set")) cmd_set(rest);
    else fprintf(stderr, "crepl: unknown command :%s (try :help)\n", cmd);

    fflush(stdout);
    return 0;
}

static int bracket_depth(const char *s, int depth)
{
    size_t i = 0;
    while (s[i]) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_quoted(s, i); continue; }
        if (c == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) { i = skip_ws(s, i); continue; }
        if (c == '{' || c == '(' || c == '[') depth++;
        else if (c == '}' || c == ')' || c == ']') depth--;
        i++;
    }
    return depth;
}

static char *read_line(const char *prompt)
{
#ifdef CREPL_READLINE
    if (G_interactive) {
        char *l = readline(prompt);
        if (l && *l) add_history(l);
        return l;
    }
#endif
    if (G_interactive) { fputs(prompt, stdout); fflush(stdout); }
    Str s; str_init(&s);
    int c;
    int got = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got = 1;
        if (c == '\n') break;
        str_addc(&s, (char)c);
    }
    if (!got && s.n == 0) { str_free(&s); return NULL; }
    char *r = s.p ? s.p : xstrdup("");
    if (!G_interactive) printf("%s%s\n", prompt, r);
    return r;
}

static char *read_input(void)
{
    Str acc; str_init(&acc);
    int depth = 0;
    int block = 0;

    for (;;) {
        const char *prompt = acc.n ? "  ... " : "crepl> ";
        char *line = read_line(prompt);
        if (!line) {
            if (acc.n) { char *r = acc.p; return r; }
            str_free(&acc);
            return NULL;
        }

        char *t = line;
        while (*t && isspace((unsigned char)*t)) t++;

        if (!acc.n && !strcmp(t, ":{")) { block = 1; free(line); continue; }
        if (block && !strcmp(t, ":}")) { free(line); break; }

        if (!acc.n && *t == ':' && !block) {
            str_add(&acc, line);
            free(line);
            return acc.p;
        }

        size_t ll = strlen(line);
        int cont = ll && line[ll - 1] == '\\';
        if (cont) line[ll - 1] = 0;

        if (acc.n) str_addc(&acc, '\n');
        str_add(&acc, line);
        depth = bracket_depth(line, depth);
        free(line);

        if (block) continue;
        if (cont) continue;
        if (depth > 0) continue;

        const char *p = str_cstr(&acc);
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) { str_clear(&acc); depth = 0; continue; }
        break;
    }

    return acc.p ? acc.p : xstrdup("");
}

static void find_self(const char *argv0)
{
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", G_self, sizeof G_self - 1);
    if (n > 0) { G_self[n] = 0; return; }
#endif
    if (realpath(argv0, G_self)) return;
    snprintf(G_self, sizeof G_self, "%s", argv0);
}

int main(int argc, char **argv)
{
    const char *replay = NULL;
    Vec preload; vec_init(&preload);

    find_self(argv[0]);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { fputs(HELP, stdout); return 0; }
        else if (!strcmp(argv[i], "--version")) { printf("crepl %s\n", CREPL_VERSION); return 0; }
        else if (!strcmp(argv[i], "-k")) G_opt.keep = 1;
        else vec_push(&preload, argv[i]);
    }

    G_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    ents_init(&G_pp);
    ents_init(&G_decls);
    vec_init(&G_cflags);
    vec_init(&G_ldflags);

    snprintf(G_tmpdir, sizeof G_tmpdir, "/tmp/crepl.XXXXXX");
    if (!mkdtemp(G_tmpdir)) { perror("crepl: mkdtemp"); return 1; }
    atexit(on_exit_cleanup);

    pick_compiler();
    install_handlers();
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (replay) load_dump(replay);
    else if (G_interactive)
        printf("crepl %s -- C REPL using %s.  :help for commands, :quit to leave.\n",
               CREPL_VERSION, G_cc);

    for (size_t i = 0; i < preload.n; i++) cmd_load(preload.v[i]);
    vec_free(&preload);

    for (;;) {
        char *in = read_input();
        if (!in) { if (G_interactive) printf("\n"); break; }

        if (G_interrupted) { G_interrupted = 0; printf("^C\n"); free(in); continue; }

        char *t = in;
        while (*t && isspace((unsigned char)*t)) t++;
        if (!*t) { free(in); continue; }

        int quit = 0;
        if (*t == ':') quit = do_meta(t);
        else quit = (eval_text(t, 0) == 3);

        free(in);
        if (quit) break;
    }

    snips_drop_from(0);
    ents_clear(&G_pp);
    ents_clear(&G_decls);
    vec_free(&G_cflags);
    vec_free(&G_ldflags);
    free(G_cc);
    cleanup_tmp();
    return 0;
}
