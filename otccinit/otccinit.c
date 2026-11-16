/* -------------------------------------------------------------------
 * tcc.c  --  otcc (the first version of tcc) by Fabrice Bellard,
 *            fixed for 32-bit Ubuntu 18.04 and slightly cleaned up.
 *
 * Build (one binary, no -D needed):
 *   Linux 32-bit : gcc -m32 -o tc tcc_fixed.c -ldl
 *   Windows      : gcc -o tc.exe tcc_fixed.c    (MinGW32 编译器)
 *
 * Usage:
 *   ./tc prog.c            compile and run prog.c
 *   ./tc -d prog.c         same, with all debug output on stderr
 *   ./tc -b prog.c         do not run, dump machine code to prog.c.bin
 *   ./tc -d -b prog.c      trace the whole compile, then dump the code
 *                          (this is what you want before objdump/gdb)
 *   ./tc -h                short help
 *
 *   -d  debug: per-token trace, dlsym results, call targets, bounds
 *              checks, source byte dump, segment layout
 *   -b  bin:   skip execution and write the generated code to a file
 *
 * Debug helpers kept in this file (nothing else):
 *   dbg       1 when -d was given (see main)
 *   stage     current phase, printed by fatal()/error()
 *   fatal()   single error exit, prints stage + file offset
 *   TRACE     one-line trace, expands to nothing when dbg == 0
 *   CHECK     bounds check, reports and exits on overflow
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * 平台抽象层
 *   Linux  : mmap 固定基址 + dlsym(RTLD_DEFAULT, name)
 *   Windows: VirtualAlloc 固定基址 + LoadLibraryA/GetProcAddress
 *
 * 两个平台都必须用固定基址 0x10000000 申请 RWX 内存（不是随便哪个
 * 地址都行，原因见 mem_alloc() 里的注释）；外部符号解析失败都返回
 * NULL/0，所以符号查找的逻辑可以完全共用。
 * ------------------------------------------------------------------- */

/* RTLD_DEFAULT lives in dlfcn.h behind __USE_GNU; without this gcc's
   default -std=gnu11 on old glibc still hides it (Ubuntu 18.04 does),
   and 'dlsym(RTLD_DEFAULT, ...)' fails to compile. Must come before
   all includes. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
/* MinGW: gcc -o tc.exe tcc_fixed.c   (use the 32-bit MinGW compiler,
   the generated code is i386 and every pointer is kept in an int) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int hdll;   /* handle to msvcrt.dll for GetProcAddress */
#else
#include <dlfcn.h>    /* dlsym / RTLD_DEFAULT */
#include <sys/mman.h>
#endif
#include <signal.h>   /* both platforms: SIGSEGV crash reporting */

static int dbg;    /* -d: verbose trace on stderr */
static int dump;   /* -b: write machine code instead of running it */

static char *stage = "startup";

#define TRACE(...)  do { if (dbg) { fprintf(stderr, "[trace] "); \
                                    fprintf(stderr, __VA_ARGS__); \
                                    fputc('\n', stderr); fflush(stderr); } } while (0)

#define TEXT_SIZE       20000
#define DATA_SIZE       2000
#define SYM_TABLE_SIZE  10000
#define VAR_TABLE_SIZE  4096

/* The whole working area is one RWX block, split into four 64KB parts:
       idtable  p + 0x00000   symbol table + token buffer
       glo      p + 0x10000   literal data (grows with every string)
       prog     p + 0x20000   generated code
       vac...   p + 0x30000   variable tables + macro stack
*/
#define MEM_BASE  0x10000000
#define MEM_SIZE  0x40000
#define SEG_SIZE  0x10000

/* vac: offset of variables
   vat: type of variables
   loc : local variable index
   glo : global variable index
   parm: parameter variable index
   ind : output code ptr
   lsym: loop symbol stack
   rsym: return symbol
   prog: output code
   astk: arg position stack
*/
int tok, *vac, *vat, *lsym, rsym,
    prog, ind, loc, glo, vt,
    vc, *macro_stack, *macro_stack_ptr;
char *idtable, *idptr, *idlast;
FILE *file;   /* was implicitly int in the original: truncates on LP64 */
int mem;      /* base of the mmap block */

/* symbols that were called but not yet defined when the call was parsed;
   checked once the whole file has been read, so that a genuinely
   undefined function is reported instead of silently compiling to a
   'call' with displacement 0 */
#define MAX_UNDEF 64
static int undef[MAX_UNDEF];
static int vt_undef;

/* single error exit: prints phase and file offset, then stops */
static void fatal(char *msg)
{
    fprintf(stderr, "\n*** ERROR [%s] offset %ld: %s\n", stage,
            file ? ftell(file) : -1L, msg);
    exit(1);
}

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, \
    "\n*** FATAL [%s]: ", stage); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); exit(2); } } while (0)

/* ---- 平台相关: 固定基址 RWX 内存 + 外部符号解析 ---- */

/* 申请一块 256KB 的 RWX 内存，基址固定 0x10000000，切成四个 64KB 段
   (符号表 / 数据 / 代码 / 变量表)。

   基址不能随便选，这是 Bellard 对 otcc 的特殊设计:
   - 必须 >= 0x10000000: 再低会撞上程序镜像和 brk 堆（i386 下可执行
     文件加载在 0x08048000 附近），内存会被已在用的内容占据或污染；
   - 必须 < 0x80000000: 所有指针都存在 32 位 int 里，更高的地址在
     int 中会变成负数，栈帧计算会错；
   - 必须可执行: 生成的机器码就地运行，现代系统的默认堆是 NX 的，
     malloc 出来的内存一执行就 SIGSEGV/DEP 异常。

   Windows 的 VirtualAlloc 传 0 时由系统挑地址（会落在高地址或随机
   位置），所以第一个参数必须显式给 0x10000000。 */
int mem_alloc(void)
{
#ifdef _WIN32
    int p = (int)VirtualAlloc((LPVOID)MEM_BASE, MEM_SIZE,
                              MEM_RESERVE | MEM_COMMIT,
                              PAGE_EXECUTE_READWRITE);
    if (p == 0) {
        fprintf(stderr, "*** FATAL: VirtualAlloc(0x%08x, 0x%x) failed, "
                "GetLastError()=%lu\n", MEM_BASE, MEM_SIZE, GetLastError());
        exit(2);
    }
#else
    int p = (int)mmap((void *)MEM_BASE, MEM_SIZE,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((void *)p == MAP_FAILED) {
        fprintf(stderr, "*** FATAL: mmap(0x%08x, 0x%x) failed: %s\n",
                MEM_BASE, MEM_SIZE, strerror(errno));
        exit(2);
    }
#endif
    if (p < MEM_BASE || p + MEM_SIZE > 0x7fffffff) {
        fprintf(stderr, "*** FATAL: got unusable address 0x%08x, "
                "needs to be inside [0x%08x, 0x7fff0000)\n", p, MEM_BASE);
        exit(2);
    }
    return p;
}

/* 解析外部符号（printf 之类的 libc 函数）。找不到返回 0，两种平台
   行为一致，调用方据此判断是否是前向引用。 */
int sym_resolve(char *name)
{
#ifdef _WIN32
    /* msvcrt.dll 导出 printf/puts/malloc 等全部 C 运行库函数 */
    return (int)GetProcAddress((HMODULE)hdll, name);
#else
    return (int)dlsym(RTLD_DEFAULT, name);
#endif
}

/* The current value can be: */
#define VT_CONST   0x0002  /* constant in vc */
#define VT_VAR     0x0004  /* value is in eax */
#define VT_LOCAL   0x0008  /* offset on stack */

#define VT_LVAL    0x0010  /* const or var is an lvalue */
#define VT_CMP     0x0020  /* the value is stored in processor flags (in vc) */
#define VT_FORWARD 0x0040  /* value is forward reference (only used for functions) */
#define VT_JMP     0x0080  /* value is the consequence of jmp. bit 0 is set if inv */

#define VT_LVALN   -17         /* ~VT_LVAL */

/*
 *
 * VT_FUNC indicates a function. The return type is the stored type. A
 * function pointer is stored as a 'char' pointer.
 *
 * If VT_PTRMASK is non nul, then it indicates the number of pointer
 * iterations to reach the basic type.
 *
 * Basic types:
 *
 * VT_BYTE indicate a char
 *
 *
 * otherwise integer type is assumed.
 *  */

#define VT_BYTE    0x00001  /* byte pointer. HARDCODED VALUE */
#define VT_PTRMASK 0x00f00  /* pointer mask */
#define VT_PTRINC  0x00100  /* pointer increment */
#define VT_FUNC    0x01000  /* function type */
#define VT_TYPE    0x01f01  /* type mask */

#define VT_TYPEN   0xffffe0fe  /* ~VT_TYPE */
#define VT_FUNCN   -4097

/* Special infos */
#define VT_DEFINE  0x02000  /* special value for #defined symbols */

/* token values */
#define TOK_INT     256
#define TOK_VOID    257
#define TOK_CHAR    258
#define TOK_IF      259
#define TOK_ELSE    260
#define TOK_WHILE   261
#define TOK_BREAK   262
#define TOK_RETURN  263
#define TOK_DEFINE  264
#define TOK_MAIN    265

#define TOK_EQ 0x94 /* warning: depend on asm code */
#define TOK_NE 0x95 /* warning: depend on asm code */
#define TOK_LT 0x9c /* warning: depend on asm code */
#define TOK_GE 0x9d /* warning: depend on asm code */
#define TOK_LE 0x9e /* warning: depend on asm code */
#define TOK_GT 0x9f /* warning: depend on asm code */

#define TOK_LAND 0xa0
#define TOK_LOR  0xa1

#define TOK_DEC  0xa2
#define TOK_MID  0xa3 /* inc/dec, to void constant */
#define TOK_INC  0xa4

#define TOK_SHL  0xe0 /* warning: depend on asm code */
#define TOK_SHR  0xf8 /* warning: depend on asm code */

/* These are correctness checks, not debug output, so they are always
   compiled in and always active -- -d only adds trace noise on top. */
void error(char *msg)
{
    fatal(msg);
}
void warning(char *msg)
{
    fprintf(stderr, "\n*** WARNING [%s] offset %ld: %s\n", stage,
            file ? ftell(file) : -1L, msg);
}

int inp()
{
    if (file == NULL)
        fatal("inp(): no source file open");
    return fgetc(file);
}

int isid(c)
{
    return (c >= 'a' & c <= 'z') |
        (c >= 'A' & c <= 'Z') |
        c == '_';
}

int isnum(c)
{
    return c >= '0' & c <= '9';
}

/* reverse lookup: token value -> identifier string. 'idtable' holds the
   names back to back, keyword 256 is the first one. Returns "?" if the
   token is out of range. */
char *sym_name(t)
{
    char *p = idtable;
    int n = 256;
    while (p < idptr && n < t) {
        while (*p++);
        n++;
    }
    return (n == t) ? p : "?";
}

/* classify a code address for the trace: the three cases that matter
   when a call goes to the wrong place */
char *what_addr(a)
{
    if (a >= prog && a < ind)
        return "local function";
    if (a >= glo && a < glo + SEG_SIZE)
        return "!!DATA SEGMENT (bug!)";
    return "external (libc)";
}

void next();

/* skip() validates the token in BOTH modes. In the original the
   non-TEST build used '#define skip(c) next()', which silently accepts
   a wrong token and the parser then loops forever on bad input. */
void skip(c)
{
    if (tok != c) {
        fprintf(stderr, "\n*** ERROR [%s] offset %ld: '%c' expected, got "
                "'%c' (0x%02x)%s\n", stage, file ? ftell(file) : -1L, c,
                tok > 32 && tok < 127 ? tok : '?', tok & 0xff,
                tok == EOF ? " - source ended too early" : "");
        exit(1);
    }
    next();
}

void next()
{
    int c, v;
    char *q, *p;

    while (1) {
        c = inp();
        if (c == EOF) {
            tok = EOF;
            return;
        }
        if (c == 47) {
            /* '/': maybe a comment. The published otcc has no comment
               support at all, which makes real programs painful to
               write, so handle both C forms here. When it is NOT a
               comment the second character must be pushed back so that
               the normal two-char handling below sees the real stream
               -- forgetting this is what made '10 / 2' print 102. */
            int c2 = inp();
            if (c2 == 47) {
                while ((c2 = inp()) != EOF && c2 != '\n')
                    ;
                if (c2 == EOF) {
                    tok = EOF;
                    return;
                }
                continue;
            } else if (c2 == 42) {
                int prev = 0;
                for (;;) {
                    c2 = inp();
                    if (c2 == EOF)
                        fatal("unterminated block comment");
                    if (prev == 42 && c2 == 47)
                        break;
                    prev = c2;
                }
                continue;
            }
            ungetc(c2, file);
            tok = c;
            TRACE("tok=0x%02x '/' at offset %ld", tok, ftell(file) - 1);
            return;
        } else if (c == 35) {
            /* preprocessor: we handle only define */
            next();
            if (tok == TOK_DEFINE) {
                next();
                /* now tok is the macro symbol */
                vat[tok] = VT_DEFINE;
                vac[tok] = ftell(file);
            }
            /* ignore preprocessor or shell */
            while (c != '\n')
                c = inp();
        } else if (c == '\n') {
            /* end of line: check if we are in macro state. if so,
               pop new file position */
            if (macro_stack_ptr > macro_stack)
                fseek(file, *--macro_stack_ptr, 0);
        /* ' ' and tab are whitespace; 0x0d too, because Windows files
           use CRLF line endings and in binary mode we see both bytes */
        } else if (c != ' ' & c != 9 & c != 13)
            break;
    }
    if (isid(c)) {
        q = idptr;
        idlast = q;
        while (isid(c) | isnum(c)) {
            /* the symbol table lives in the first 64KB segment; keeping
               q inside it stops it from running into the data segment */
            CHECK(q < idtable + SEG_SIZE - 2,
                  "symbol table overflow: idptr=0x%08x limit=0x%08x",
                  (int)idptr, (int)idtable + SEG_SIZE);
            *q++ = c;
            c = inp();
        }
        *q++ = '\0';
        ungetc(c, file);
        p = idtable;
        tok = 256;
        while (p < idptr) {
            if (strcmp(p, idptr) == 0)
                break;
            while (*p++);
            tok++;
        }
        /* if not found, add symbol */
        if (p == idptr)
            idptr = q;
        TRACE("ident '%s' tok=0x%03x offset %ld", idlast, tok, ftell(file));
        /* eval defines */
        if (vat[tok] & VT_DEFINE) {
            CHECK(macro_stack_ptr < macro_stack + 64, "macro stack overflow");
            *macro_stack_ptr++ = ftell(file);
            fseek(file, vac[tok], 0);
            next();
        }
    } else {
        q = "<=\236>=\235!=\225&&\240||\241++\244--\242==\224<<\340>>\370";
        /* two chars */
        v = inp();
        while (*q) {
            if (*q == c & q[1] == v) {
                tok = q[2] & 0xff;
                TRACE("tok=0x%02x ('%c%c') offset %ld", tok, c, v, ftell(file));
                return;
            }
            q = q + 3;
        }
        ungetc(v, file);
        /* single char substitutions */
        if (c == '<')
            tok = TOK_LT;
        else if (c == '>')
            tok = TOK_GT;
        else
            tok = c;
        TRACE("tok=0x%02x '%c' offset %ld", tok, (c >= 32 && c < 127) ? c : '.',
              ftell(file));
    }
}

void g(c)
{
    /* every emitted byte goes through here: the single best place to
       catch a runaway code generator before it scribbles outside the
       block and corrupts the other segments */
    CHECK(ind >= prog && ind < prog + SEG_SIZE,
          "code buffer overflow: ind=0x%08x range=[0x%08x,0x%08x)",
          ind, prog, prog + SEG_SIZE);
    *(char *)ind++ = c;
}

void o(c)
{
    while (c) {
        g(c);
        c = c / 256;
    }
}

/* output a symbol and patch all calls to it */
void gsym(t)
{
    int n;
    while (t) {
        CHECK(t >= prog && t + 4 <= prog + SEG_SIZE,
              "gsym: patch slot 0x%08x outside code [0x%08x,0x%08x)",
              t, prog, prog + SEG_SIZE);
        n = *(int *)t; /* next value */
        *(int *)t = ind - t - 4;
        t = n;
    }
}

/* psym is used to put an instruction with a data field which is a
   reference to a symbol. It is in fact the same as oad ! */
#define psym oad

/* instruction + 4 bytes data. Return the address of the data */
int oad(c, s)
{
    o(c);
    CHECK(ind >= prog && ind + 4 <= prog + SEG_SIZE,
          "code buffer overflow in oad: ind=0x%08x limit=0x%08x",
          ind, prog + SEG_SIZE);
    *(int *)ind = s;
    s = ind;
    ind = ind + 4;
    return s;
}

/* push to value stack */
void vset(t, v)
{
    vt = t;
    vc = v;
}

/* generate a value in eax from vt and vc */
void gv()
{
#ifndef TINY
    int t;
#endif
    if (vt & VT_LVAL) {
        if ((vt & VT_TYPE) == VT_BYTE)
            o(0xbe0f);   /* movsbl x, %eax */
        else
            o(0x8b);     /* movl x,%eax */
        if (vt & VT_CONST)
            oad(0x05, vc);
        else if (vt & VT_LOCAL)
            oad(0x85, vc);
        else
            g(0x00);
    } else {
        if (vt & VT_CONST) {
            oad(0xb8, vc); /* mov $xx, %eax */
        } else if (vt & VT_LOCAL) {
            oad(0x858d, vc); /* lea xxx(%ebp), %eax */
        } else if (vt & VT_CMP) {
            oad(0xb8, 0); /* mov $0, %eax */
            o(0x0f); /* setxx %al */
            o(vc);
            o(0xc0);
        }
#ifndef TINY
        else if (vt & VT_JMP) {
            t = vt & 1;
            oad(0xb8, t); /* mov $1, %eax */
            oad(0xe9, 5); /* jmp after */
            gsym(vc);
            oad(0xb8, t ^ 1); /* mov $0, %eax */
        }
#endif
    }
    vt = (vt & VT_TYPE) | VT_VAR;
}

/* generate a test. set 'inv' to invert test */
/* XXX: handle constant */
int gtst(inv, t)
{
    if (vt & VT_CMP) {
        /* fast case : can jump directly since flags are set */
        g(0x0f);
        t = psym((vc - 16) ^ inv, t);
    } else
#ifndef TINY
    if (vt & VT_JMP) {
        /* && or || optimization */
        if ((vt & 1) == inv)
            t = vc;
        else {
            t = psym(0xe9, t);
            gsym(vc);
        }
    } else
    if ((vt & (VT_CONST | VT_LVAL)) == VT_CONST) {
        /* constant jmp optimization */
        if ((vc != 0) != inv)
            t = psym(0xe9, t);
    } else
#endif
    {
        gv();
        o(0xc085); /* test %eax, %eax */
        g(0x0f);
        t = psym(0x85 ^ inv, t);
    }
    return t;
}

/* return the size (in bytes) of a given type */
int type_size(t)
{
    if ((t & VT_PTRMASK) > VT_PTRINC | (t & VT_TYPE) == VT_PTRINC)
        return 4;
    else
        return 1;
}

#define POST_ADD 0x1000
#define PRE_ADD  0

/* a defines POST/PRE add. c is the token ++ or -- */
void inc(a, c)
{
    if (!(vt & VT_LVAL))
        error("lvalue expected");
    vt = vt & VT_LVALN;
    gv();
    o(0x018bc189); /* movl %eax, %ecx ; mov (%ecx), %eax */
    o(0x408d | a); /* leal x(%eax), %eax/%edx */
    g((c - TOK_MID) * type_size(vt));
    o(0x0189 | a); /* mov %eax/%edx, (%ecx) */
}

/* op is '-' or '+' (or 0) */
/* t is the type of the first operand */
/* XXX: handle ptr sub and 'int + ptr' case (only 'ptr + int' handled) */
void gen_op(op, t)
{
    gv();
    o(0x59); /* pop %ecx */
    if (op == '+' | op == '-') {
        /* XXX: incorrect for short (futur!) */
        if (type_size(t) != 1)
            o(0x02e0c1); /* shl $2, %eax */
        if (op == '-')
            o(0xd8f7); /* neg %eax */
        o(0xc801); /* add %ecx, %eax */
        vt = t;
    } else if (op == '&')
        o(0xc821);
    else if (op == '^')
        o(0xc831);
    else if (op == '|')
        o(0xc809);
    else if (op == '*')
        o(0xc1af0f); /* imul %ecx, %eax */
    else if (op == TOK_SHL | op == TOK_SHR) {
        o(0xd391); /* xchg %ecx, %eax, shl/sar %cl, %eax */
        o(op);
    } else if (op == '/' | op == '%') {
        o(0xd231);   /* xor %edx, %edx */
        o(0xf9f791); /* xchg %ecx, %eax, idiv %ecx, %eax */
        if (op == '%')
            o(0x92); /* xchg %edx, %eax */
    } else {
        o(0xc139); /* cmp %eax,%ecx */
        vset(VT_CMP, op);
    }
}

/* return 0 if no type declaration. otherwise, return the basic type
   and skip it.
   XXX: A '2' is ored to ensure non zero return if int type.
 */
int ist()
{
    int t;

    if (tok == TOK_INT | tok == TOK_CHAR | tok == TOK_VOID) {
        t = tok;
        next();
        return (t != TOK_INT) | 2;
    } else {
        return 0;
    }
}

/* Read a type declaration (except basic type), and return the
   type. If v is true, then also put variable name in 'vc' */
int typ(v, t)
{
    int u, p, n;

    t = t & -3; /* suppress the ored '2' */
    while (tok == '*') {
        next();
        t = t + VT_PTRINC;
    }

    /* recursive type */
    /* XXX: incorrect if abstract type for functions (e.g. 'int ()') */
    if (tok == '(') {
        next();
        u = typ(v, 0);
        skip(')');
    } else {
        u = 0;
        /* type identifier */
        if (v) {
            vc = tok;
            next();
        }
    }
    /* function declaration */
    if (tok == '(') {
        next();
        p = 4;
        n = vc; /* must save vc there */
        while (tok != ')') {
            /* read param name and compute offset */
            if (t = ist())
                t = typ(1, t); /* XXX: should accept both arg/non arg if v == 0 */
            else {
                vc = tok;
                t = 0;
                next();
            }
            p = p + 4;
            vat[vc] = VT_LOCAL | VT_LVAL | t;
            vac[vc] = p;
            if (tok == ',')
                next();
        }
        next(); /* skip ')' */
        vc = n;
        if (u)
            t = u + VT_BYTE;
        else
            t = t | VT_FUNC;
    }
    return t;
}

int getq(n)
{
    int c;
    if (n == '\\') {
        n = inp();
        if (n == 'n')
            n = '\n';
        else if (isnum(n)) {
            c = 0;
            while (isnum(n)) {
                c = c * 8 + n - '0';
                n = inp();
            }
            ungetc(n, file);
            return c;
        }
    }
    return n;
}

void unary()
{
    int n, t, ft, fc, p, idt = 0;

    if (isnum(tok)) {
        /* number */
        n = 0;
        while (isnum(tok)) {
            n = n * 10 + tok - '0';
            next();
        }
        vset(VT_CONST, n);
    } else if (tok == '\'') {
        vset(VT_CONST, getq(inp()));
        next(); /* skip char */
        skip('\'');
    } else if (tok == '\"') {
        vset(VT_CONST | VT_PTRINC | VT_BYTE, glo);
        /* stop at the closing quote; without the EOF test an
           unterminated string would loop forever and walk glo past the
           end of the data segment */
        while ((n = inp()) != 34) {
            if (n == EOF)
                fatal("unterminated string literal");
            CHECK(glo < mem + 0x20000, "string literal table overflow");
            *(char *)glo = getq(n);
            glo++;
        }
        *(char *)glo = 0;
        glo = (glo + 4) & -4; /* align heap */
        next();
    } else {
        t = tok;
        next();
        if (t == '(') {
            /* cast ? */
            if (t = ist()) {
                ft = typ(0, t);
                skip(')');
                unary();
                vt = (vt & VT_TYPEN) | ft;
            } else {
                expr();
                skip(')');
            }
        } else if (t == '*') {
            unary();
            if (vt & VT_LVAL)
                gv();
            if (!(vt & VT_PTRMASK))
                error("pointer expected");
            vt = (vt - VT_PTRINC) | VT_LVAL;
        } else if (t == '&') {
            unary();
            if (!(vt & VT_LVAL))
                error("lvalue expected");
            vt = vt & VT_LVALN;
            vt = vt + VT_PTRINC;
        } else
#ifndef TINY
        if (t == '!') {
            unary();
            if (vt & VT_CMP)
                vc = vc ^ 1;
            else
                vset(VT_JMP, gtst(1, 0));
        } else
        if (t == '~') {
            unary();
            if ((vt & (VT_CONST | VT_LVAL)) == VT_CONST)
                vc = ~vc;
            else {
                gv();
                o(0xd0f7);
            }
        } else
#endif
        if (t == TOK_INC | t == TOK_DEC) {
            unary();
            inc(PRE_ADD, t);
        } else if (t == '-') {
            unary();
            if ((vt & (VT_CONST | VT_LVAL)) == VT_CONST)
                vc = -vc;
            else {
                gv();
                o(0xd8f7); /* neg %eax */
            }
        } else if (t == '+') {
            unary();
        } else {
            idt = t;                      /* remember the token: 't' is
                                             reused as the argument byte
                                             count inside a call */
            vset(vat[t], vac[t]);
            /* Three cases reach here:
               1. declared prototype        : vat = VT_FUNC|VT_CONST, vac = 0
               2. never seen identifier     : vat = 0, vac = 0
                  (a call to a function defined later in the file)
               3. already defined/allocated : vat != 0, vac != 0 -> keep as is
               Both (1) and (2) must go through dlsym(): if the name is an
               external symbol we take its address, otherwise it is a
               forward reference that 'gsym' patches once the real
               definition is reached. Case (2) used to be taken as the
               constant 0, so 'call later()' jumped to address 0. */
            if (((vt & (VT_FUNC | VT_CONST)) == (VT_FUNC | VT_CONST) &&
                 vc == 0) || vt == 0) {
                CHECK(idlast >= idtable && idlast < idtable + SEG_SIZE,
                      "idlast 0x%08x outside symbol table", (int)idlast);
                n = sym_resolve(idlast);
                TRACE("  sym_resolve(\"%s\") = 0x%08x %s", idlast, n,
                      n ? "-> external" : "-> not found");
                if (n == 0) {
                    /* Not a libc symbol. If it is a function call, the
                       forward reference may still be resolved later by
                       'gsym' when the definition appears. Anything else
                       is a genuinely undefined identifier -- report it
                       instead of letting it default to address 0. */
                    vset(VT_CONST | VT_FORWARD | VT_LVAL, vac + t);
                    TRACE("  '%s' -> forward reference", idlast);
                } else
                    vset(VT_CONST | VT_LVAL, n);
            }
        }
    }

    /* post operations */
    if (tok == '?') {
        /* '?:' is not implemented. Without this check the '?' is left in
           the stream, the parser swallows it and silently produces a
           WRONG result (e.g. 'a>2 ? 100 : 200' yields 1). */
        fprintf(stderr, "\n*** ERROR [%s] offset %ld: the conditional "
                "operator '?:' is not supported by otcc\n"
                "    rewrite it as:  if (cond) r = a; else r = b;\n",
                stage, file ? ftell(file) : -1L);
        exit(1);
    }
    if (tok == TOK_INC | tok == TOK_DEC) {
        inc(POST_ADD, tok);
        next();
    } else
    if (tok == '[') {
        if (!(vt & VT_PTRMASK))
            error("pointer expected");
        gv();
        ft = vt;
        fc = vc;
        next();
        o(0x50); /* push %eax */
        expr();
        gen_op('+', ft);
        /* dereference pointer */
        vt = (ft - VT_PTRINC) | VT_LVAL;
        vc = fc;
        skip(']');
    } else
    if (tok == '(') {
        /* function call  */
        /* lvalue is implied */
        vt = vt & VT_LVALN;
        if ((vt & VT_CONST) == 0) {
            /* evaluate function address */
            gv();
            o(0x50); /* push %eax */
        }
        ft = vt;
        fc = vc;
        TRACE("call: ft=0x%05x fc=0x%08x (CONST=%d FORWARD=%d)", ft, fc,
              (ft & VT_CONST) != 0, (ft & VT_FORWARD) != 0);
        next();
        t = 0;
        while (tok != ')') {
            t = t + 4;
            expr();
            gv();
            o(0x50); /* push %eax */
            if (tok == ',')
                next();
        }
        skip(')');
        /* horrible, but needed : convert to native ordering (could
           parse parameters in reverse order, but would cost more
           code) */
        n = 0;
        p = t - 4;
        while (n < p) {
            oad(0x24848b, p); /* mov x(%esp,1), %eax */
            oad(0x248487, n); /* xchg   x(%esp,1), %eax */
            oad(0x248489, p); /* mov %eax, x(%esp,1) */
            n = n + 4;
            p = p - 4;
        }
        if (ft & VT_CONST) {
            /* forward reference */
            if (ft & VT_FORWARD) {
                TRACE("call (forward) '%s' patch slot=0x%08x", idlast, fc);
                /* 'fc' is vac[t]: gsym() overwrites it with the relative
                   offset of the definition when it appears. If the symbol
                   is never defined, gsym is never called for this slot and
                   the displacement stays 0 -- the call lands on the next
                   instruction and the program silently "works". Remember
                   the symbol and check them all after the parse. */
                *(int *)fc = psym(0xe8, *(int *)fc);
                if (vt_undef < MAX_UNDEF)
                    undef[vt_undef++] = idt;
            } else {
                TRACE("call target=0x%08x [%s] argc_bytes=%d", fc,
                      what_addr(fc), t);
                oad(0xe8, fc - ind - 5);
            }
        } else {
            TRACE("indirect call through stack slot sp+%d", t);
            oad(0x2494ff, t); /* call *xxx(%esp) */
            t = t + 4;
        }
        if (t)
            oad(0xc481, t);
        /* return value is variable */
        vt = VT_VAR;
    }
}

void uneq()
{
    int ft, fc, b;

    unary();
    if (tok == '=') {
        if (!(vt & VT_LVAL))
            error("lvalue expected");
        next();
        fc = vc;
        ft = vt;
        b = (vt & VT_TYPE) == VT_BYTE;
        if (ft & VT_VAR)
            o(0x50); /* push %eax */
        expr();
        if ((vt & VT_PTRMASK) != (ft & VT_PTRMASK))
            warning("incompatible type");
        gv();  /* generate value */

        if (ft & VT_VAR) {
            o(0x59); /* pop %ecx */
            o(0x0189 - b); /* mov %eax/%al, (%ecx) */
        } else {
            if (ft & VT_LOCAL)
                oad(0x8589 - b, fc); /* mov %eax/%al,xxx(%ebp) */
            else
                oad(0xa3 - b, fc); /* mov %eax/%al,xxx */
        }
    }
}

void sum(l)
{
    int op, t;
    if (l == 0)
        uneq();
    else {
        l--;
        sum(l);
        while (1) {
            op = tok;
            if ((l == 0 & op != '*' & op != '/' & op != '%') |
                (l == 1 & op != '+' & op != '-') |
                (l == 2 & op != TOK_SHL & op != TOK_SHR) |
                (l == 3 & (op < TOK_LT | op > TOK_GT)) |
                (l == 4 & op != TOK_EQ & op != TOK_NE) |
                (l == 5 & op != '&') |
                (l == 6 & op != '^') |
                (l == 7 & op != '|'))
                break;
            gv();
            t = vt;
            o(0x50); /* push %eax */
            next();
            sum(l);
            gen_op(op, t);
       }
    }
}

#ifdef TINY
void expr()
{
    sum(8);
}
#else
void eand()
{
    int t;

    sum(8);
    t = 0;
    while (1) {
        if (tok != TOK_LAND) {
            if (t) {
                t = gtst(1, t);
                vset(VT_JMP | 1, t);
            }
            break;
        }
        t = gtst(1, t);
        next();
        sum(8);
    }
}

void expr()
{
    int t, u;

    eand();
    t = 0;
    while (1) {
        if (tok != TOK_LOR) {
            if (t) {
                t = gtst(0, t);
                vset(VT_JMP, t);
            }
            break;
        }
        t = gtst(0, t);
        next();
        eand();
    }
}
#endif

void block()
{
    int a, c, d;

    if (tok == TOK_IF) {
        /* if test */
        next();
        skip('(');
        expr();
        skip(')');
        a = gtst(1, 0);
        block();
        c = tok;
        if (c == TOK_ELSE) {
            next();
            d = psym(0xe9, 0); /* jmp */
            gsym(a);
            block();
            gsym(d); /* patch else jmp */
        } else
            gsym(a);
    } else if (tok == TOK_WHILE) {
        next();
        d = ind;
        skip('(');
        expr();
        skip(')');
        *++lsym = gtst(1, 0);
        block();
        oad(0xe9, d - ind - 5); /* jmp */
        gsym(*lsym--);
    } else if (tok == '{') {
        next();
        /* declarations */
        decl(VT_LOCAL);
        while (tok != '}')
            block();
        next();
    } else if (tok == TOK_RETURN) {
        next();
        if (tok != ';') {
            expr();
            gv();
        }
        skip(';');
        rsym = psym(0xe9, rsym); /* jmp */
    } else if (tok == TOK_BREAK) {
        /* compute jump */
        *lsym = psym(0xe9, *lsym);
        next();
        skip(';');
    } else {
        if (tok != ';')
            expr();
        skip(';');
    }
}

/* 'l' is true if local declarations */
void decl(l)
{
    int *a, t, b;

    while (b = ist()) {
        while (1) { /* iterate thru each declaration */
            vt = typ(1, b);
            if (tok == '{') {
                /* patch forward references (XXX: does not work for
                   function pointers) */
                if (vat[vc] == 0)
                    gsym(vac[vc]);
                /* put function address */
                vat[vc] = VT_CONST | VT_LVAL | vt;
                vac[vc] = ind;
                loc = 0;
                o(0xe58955); /* push   %ebp, mov    %esp, %ebp */
                a = oad(0xec81, 0); /* sub $xxx, %esp */
                rsym = 0;
                block();
                gsym(rsym);
                o(0xc3c9); /* leave, ret */
                *a = loc; /* save local variables */
                break;
            } else if (vt & VT_FUNC) {
                /* function prototype: no storage allocated, vac = 0 so
                   that 'unary' resolves it with dlsym() */
                vat[vc] = VT_CONST | VT_LVAL | vt;
                vac[vc] = 0;
                skip(';');
                break;
            } else {
                /* variable */
                vat[vc] = l | VT_LVAL | vt;
                if (l == VT_LOCAL) {
                    loc = loc + 4;
                    vac[vc] = -loc;
                } else {
                    vac[vc] = glo;
                    glo = glo + 4;
                }
                if (tok != ',') {
                    skip(';');
                    break;
                }
                next();
            }
        }
    }

    /* Every top-level declaration must start with 'int', 'void' or
       'char'. If the token stream still has something here the parser
       silently produced nothing -- exactly what happens with "main(){}"
       (missing return type), which used to end in a mysterious
       "no main" error much later. Report it right away. */
    if (tok != EOF && l == VT_CONST) {
        fprintf(stderr, "\n*** ERROR [%s] offset %ld: unexpected token '%s' "
                "at top level\n"
                "    every declaration must begin with 'int', 'void' or "
                "'char'.\n"
                "    if you meant to define main(), write:\n"
                "        int main() { ... }\n",
                stage, file ? ftell(file) : -1L, idlast ? idlast : "?");
        exit(1);
    }
}

/* ------------------------------------------------------------------
 * segv_handler: when the generated code (or the compiler itself) takes
 * a bad pointer we still want to know WHERE. The segment addresses plus
 * the faulting EIP are usually enough to tell "jumped into data" from
 * "jumped outside the block".
 * ------------------------------------------------------------------ */
static void segv_handler(int sig)
{
    fprintf(stderr, "\n*** SIGNAL %d (%s) in stage '%s'\n", sig,
            sig == SIGSEGV ? "SIGSEGV" : "other", stage);
    fprintf(stderr, "    block   [0x%08x,0x%08x)\n", mem, mem + MEM_SIZE);
    fprintf(stderr, "    idtable  0x%08x .. 0x%08x\n", mem, mem + 0x10000);
    fprintf(stderr, "    glo      0x%08x .. 0x%08x   (now 0x%08x)\n",
            mem + 0x10000, mem + 0x20000, glo);
    fprintf(stderr, "    prog     0x%08x .. 0x%08x   (ind 0x%08x)\n",
            prog, prog + SEG_SIZE, ind);
    if (dbg)
        fprintf(stderr, "    tok=0x%02x vt=0x%04x vc=0x%08x loc=%d "
                "ident='%s'\n", tok, vt, vc, loc, idlast ? idlast : "?");
    fprintf(stderr, "    re-run with -b and objdump the .bin to see which "
            "instruction faulted,\n"
            "    or run under gdb to get the exact EIP.\n");
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
#ifdef _WIN32
    /* MinGW 的 CRT 提供 signal(SIGSEGV)，SIGBUS 没有 */
    signal(SIGSEGV, segv_handler);
#else
    signal(SIGSEGV, segv_handler);
    signal(SIGBUS, segv_handler);
#endif
}

static void usage(char *prog)
{
    printf("usage: %s [-d] [-b] src.c [args...]\n"
           "  -d    print debug trace on stderr\n"
           "  -b    do not run; write machine code to src.c.bin\n"
           "  -h    this help\n"
           "  --    stop option parsing (for programs whose args start "
           "with '-')\n", prog);
}

int main(int c, char **v)
{
    int (*t)();
    char oname[512];
    int i, narg;

    if (c < 2) {
        usage(v[0]);
        return 1;
    }

    /* parse options: everything before the source file name */
    for (i = 1; i < c; i++) {
        if (v[i][0] != '-' || v[i][1] == '\0')
            break;
        if (v[i][1] == '-' && v[i][2] == '\0') {   /* "--" */
            i++;
            break;
        }
        if (strcmp(v[i], "-d") == 0) {
            dbg = 1;
        } else if (strcmp(v[i], "-b") == 0) {
            dump = 1;
        } else if (strcmp(v[i], "-h") == 0 || strcmp(v[i], "--help") == 0) {
            usage(v[0]);
            return 0;
        } else {
            fprintf(stderr, "*** FATAL: unknown option '%s'\n", v[i]);
            usage(v[0]);
            return 1;
        }
    }
    if (i >= c) {
        fprintf(stderr, "*** FATAL: no source file given\n");
        usage(v[0]);
        return 1;
    }

    /* v[i] is the source; everything after it is passed to main() of the
       compiled program, so shift the vector down by i: v[0] then holds
       the source file name and argc for the compiled program is narg-1 */
    narg = c - i;
    v += i;

    stage = "open source";
    /* binary mode: the macro processor stores raw ftell() offsets and
       seeks back to them, which must be byte offsets. Windows text mode
       (default for "r") translates CRLF and ftell no longer matches
       fseek, so a #define'd body gets re-read forever -> macro stack
       overflow. 'rb' on Linux is identical to 'r'. */
    file = fopen(v[0], "rb");
    if (file == NULL) {
        fprintf(stderr, "*** FATAL: cannot open \"%s\": %s\n",
                v[0], strerror(errno));
        return 1;
    }
    TRACE("source \"%s\" opened, dbg=%d dump=%d", v[0], dbg, dump);
    if (sizeof(void *) != 4)
        fprintf(stderr, "*** WARNING: this build is not 32-bit (pointer size "
                "%d).\n***          use a 32-bit compiler (gcc -m32 / "
                "MinGW32) ...\n", (int)sizeof(void *));

    stage = "memory alloc";
#ifdef _WIN32
    /* Windows: 外部符号（printf 等）通过 GetProcAddress 从 msvcrt.dll
       解析，必须先加载它。这替代了 Linux 上的 dlsym(RTLD_DEFAULT)。 */
    hdll = (int)LoadLibraryA("msvcrt.dll");
    if (hdll == 0) {
        fprintf(stderr, "*** FATAL: LoadLibraryA(\"msvcrt.dll\") failed, "
                "GetLastError()=%lu\n", GetLastError());
        return 1;
    }
    TRACE("msvcrt.dll loaded, hdll=0x%08x", hdll);
#endif
    install_crash_handler();
    mem = mem_alloc();
    TRACE("block -> [0x%08x, 0x%08x)", mem, mem + MEM_SIZE);

    stage = "layout";
    idtable = (char *)mem;                /* symbols + token buffer */
    glo = mem + 0x10000;                  /* literal data segment */
    prog = mem + 0x20000;                 /* code segment */
    vac = (int *)(mem + 0x30000);         /* variable table: value */
    vat = vac + VAR_TABLE_SIZE / (int)sizeof(int);
    lsym = vat + VAR_TABLE_SIZE / (int)sizeof(int);
    macro_stack = lsym + 64;
    macro_stack_ptr = macro_stack;
    /* the variable tables must stay inside the 4th segment */
    CHECK((int)(lsym + 64) <= mem + MEM_SIZE,
          "variable tables exceed the 4th segment: need %d bytes, "
          "segment is %d",
          (int)((char *)(lsym + 64) - (char *)(mem + 0x30000)), SEG_SIZE);
    TRACE("idtable=0x%08x glo=0x%08x prog=0x%08x vac=0x%08x lsym=0x%08x",
          (int)idtable, glo, prog, (int)vac, (int)lsym);

    memcpy(idtable,
           "int\0void\0char\0if\0else\0while\0break\0return\0define\0main", 53);
    idptr = idtable + 53;
    ind = prog;

    if (dbg) {
        /* dump the raw bytes of the source: this is the fastest way to
           see what the lexer really gets. A BOM, CRLF, a non-ASCII blank
           or a stray 0x00 all change the first token in surprising
           ways. */
        long save = ftell(file);
        int ch, n = 0;
        fprintf(stderr, "[trace] --- source bytes (first 64) ---\n        ");
        rewind(file);
        while ((ch = fgetc(file)) != EOF && n < 64) {
            fprintf(stderr, "%02x ", ch & 0xff);
            if ((++n % 16) == 0)
                fprintf(stderr, "\n        ");
        }
        fprintf(stderr, "\n        text: \"");
        rewind(file);
        n = 0;
        while ((ch = fgetc(file)) != EOF && n++ < 64) {
            if (ch >= 32 && ch < 127)
                fputc(ch, stderr);
            else if (ch == '\n')
                fputs("\\n", stderr);
            else if (ch == '\t')
                fputs("\\t", stderr);
            else
                fprintf(stderr, "\\x%02x", ch & 0xff);
        }
        fprintf(stderr, "\"\n");
        fseek(file, save, SEEK_SET);
        fflush(stderr);
    }

    stage = "lexer";
    next();
    TRACE("first token = 0x%02x", tok);
    if (tok == TOK_MAIN)
        fprintf(stderr, "*** NOTE: the first token is 'main' (0x%x), so the "
                "lexer never saw a leading 'int'.\n***       add the return "
                "type: 'int main() { ... }'\n", TOK_MAIN);

    stage = "parser + codegen";
    decl(VT_CONST);
    TRACE("parse finished, %d bytes of code generated", ind - prog);

    /* Any symbol that was called but never defined is still marked with
       vat[t] == 0 (a definition sets it in 'decl'). Its call site holds
       an unpatched displacement of 0, which jumps to the next
       instruction and hides the bug completely. */
    {
        int i, bad = 0;
        for (i = 0; i < vt_undef; i++)
            if (vat[undef[i]] == 0) {
                fprintf(stderr, "*** ERROR: undefined function '%s' "
                        "is called but never defined\n", sym_name(undef[i]));
                bad = 1;
            }
        if (bad) {
            fprintf(stderr, "    add a definition, or fix the spelling -- "
                    "otcc would otherwise\n    emit a call to the next "
                    "instruction and run the wrong code.\n");
            fclose(file);
            return 1;
        }
    }

    stage = "lookup main";
    if (vat[TOK_MAIN] == 0 || vac[TOK_MAIN] == 0) {
        fprintf(stderr, "*** ERROR: no 'main' function found in \"%s\"\n",
                v[0]);
        fclose(file);
        return 1;
    }
    if (vac[TOK_MAIN] < prog || vac[TOK_MAIN] >= ind) {
        fprintf(stderr, "*** FATAL: main() at 0x%08x is outside the generated "
                "code [0x%08x,0x%08x)\n    (prototype/definition handling "
                "broke)\n", vac[TOK_MAIN], prog, ind);
        fclose(file);
        return 1;
    }
    TRACE("main() at 0x%08x", vac[TOK_MAIN]);

    if (dump) {
        /* -b: write the machine code out and stop, so it can be fed to
           objdump/gdb:
             objdump -D -b binary -m i386 --adjust-vma=0x10020000 x.c.bin */
        FILE *df;

        stage = "dump machine code";
        CHECK(strlen(v[0]) + 5 < sizeof(oname), "source path too long");
        strcpy(oname, v[0]);
        strcat(oname, ".bin");
        df = fopen(oname, "wb");   /* binary: 0x0a must not become 0x0d 0x0a */
        if (df == NULL) {
            fprintf(stderr, "*** FATAL: cannot create \"%s\": %s\n",
                    oname, strerror(errno));
            fclose(file);
            return 1;
        }
        if (fwrite((void *)prog, 1, ind - prog, df) != (size_t)(ind - prog)) {
            fprintf(stderr, "*** FATAL: short write to \"%s\"\n", oname);
            fclose(df);
            fclose(file);
            return 1;
        }
        fclose(df);
        fclose(file);
        TRACE("wrote %d bytes to \"%s\"", ind - prog, oname);
        printf("OK: %d bytes of machine code written to %s\n",
               ind - prog, oname);
        return 0;
    }

    /* default: run the generated code */
    stage = "execute generated code";
    t = (int (*)())vac[TOK_MAIN];
    TRACE("calling main() at 0x%08x with argc=%d, argv[0]=\"%s\"",
          (int)t, narg - 1, v[0]);
    fclose(file);
    return (*t)(narg - 1, v);
}
