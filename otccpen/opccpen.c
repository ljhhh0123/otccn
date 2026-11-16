/*  Obfuscated Tiny C Compiler with PE output (win32, msvcrt.dll)
 *
 *  Port of otccelf.c to win32. The i386 code generator is unchanged:
 *  only the ELF writer was replaced by a PE writer. The output file is
 *  a small PE32 console executable which imports msvcrt.dll (and
 *  kernel32.dll for the startup code). Everything (code, data, import
 *  tables) is put in one read/write/executable section so that the
 *  buffer offsets stay contiguous, like in the ELF version.
 *
 *  build (32 bit compiler required, the code uses 'int' as pointer):
 *      i686-w64-mingw32-gcc -std=gnu89 -O2 -o otccpen.exe otccpen.c
 *  usage:
 *      otccpen source.c prog.exe
 *
 *  restrictions inherited from otcc (and a few new ones):
 *   - only 'int' (and 'char' via casts), no arrays, no structs
 *   - K&R function definitions only
 *   - undefined symbols are imported as functions (from msvcrt.dll by
 *     default, from kernel32/user32 for the names listed below), so
 *     external *data* symbols (like 'stderr') cannot be used
 *   - the output image is not relocatable: it must be loaded at
 *     0x400000 (the relocation table is stripped)
 */
#ifndef TINY
#include <stdarg.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
/* vars: value of variables 
   loc : local variable index
   glo : global variable ptr
   data: base of data segment
   ind : output code ptr
   prog: output code
   rsym: return symbol
   sym_stk: symbol stack
   dstk: symbol stack pointer
   dptr, dch: macro state
   * 'vars' format: 
   For each character TAG_TOK at offset 'i' before a
    symbol in sym_stk, we have:
    v = (int *)(vars + 8 * i + TOK_IDENT)[0] 
    p = (int *)(vars + 8 * i + TOK_IDENT)[0] 
    
    v = 0    : undefined symbol, p = list of use points.
    v = 1    : define symbol, p = pointer to define text.
    v < LOCAL: offset on stack, p = 0.
    otherwise: symbol with value 'v', p = list of use points.
   * 'sym_stk' format:
   TAG_TOK sym1 TAG_TOK sym2 .... symN '\0'
   'dstk' points to the last '\0'.
*/
int tok, tokc, tokl, ch, vars, rsym, prog, ind, loc, glo, file, sym_stk, dstk, dptr, dch, last_id, data, text, data_offset;
/* pe output state: one entry per imported symbol */
int nimp, dllcnt[4];
int imp_name[300], imp_len[300], imp_dll[300], imp_stub[300], imp_hn[300], imp_iat[300];
#define ALLOC_SIZE 99999
#define PEOUT
/* depends on the init string */
#define TOK_STR_SIZE 48
#define TOK_IDENT    0x100
#define TOK_INT      0x100
#define TOK_IF       0x120
#define TOK_ELSE     0x138
#define TOK_WHILE    0x160
#define TOK_BREAK    0x190
#define TOK_RETURN   0x1c0
#define TOK_FOR      0x1f8
#define TOK_DEFINE   0x218
#define TOK_MAIN     0x250
#define TOK_DUMMY   1
#define TOK_NUM     2
#define LOCAL   0x200
#define SYM_FORWARD 0
#define SYM_DEFINE  1
/* tokens in string heap */
#define TAG_TOK    ' '
#define TAG_MACRO  2
/* additionnal pe output defines */
#ifdef PEOUT
#define PE_BASE    0x400000     /* image base of the output file */
#define PE_HDR     0x400        /* size of the headers == file offset of .text */
#define PE_RVA     0x1000       /* rva of the single section */
#define PE_SCRATCH PE_HDR       /* argc + argv, written by the startup code */
#define PE_GLO     (PE_HDR + 0x100)  /* first global variable */
#define RVA(t)     ((t) + data_offset - PE_BASE)
#define VA(t)      ((t) + data_offset)
#endif
/* dlls used by the output file. 'msvcrt' is the default one, the others
   are only used for the symbols listed below */
char *dllname[] = {"msvcrt.dll", "kernel32.dll", "user32.dll"};
char *k32name[] = {"GetCommandLineA", "ExitProcess", "GetStdHandle", "WriteFile",
    "ReadFile", "LoadLibraryA", "GetProcAddress", "VirtualAlloc", "VirtualFree",
    "Sleep", "GetModuleHandleA", "CreateFileA", "CloseHandle", "GetLastError", 0};
char *u32name[] = {"MessageBoxA", 0};
pdef(t)
{
    *(char *)dstk++ = t;
}
inp()
{
    if (dptr) {
        ch = *(char *)dptr++;
        if (ch == TAG_MACRO) {
            dptr = 0;
            ch = dch;
        }
    } else
        ch = fgetc(file);
    /*    printf("ch=%c 0x%x\n", ch, ch); */
}
isid()
{
    return isalnum(ch) | ch == '_';
}
/* read a character constant */
getq()
{
    if (ch == '\\') {
        inp();
        if (ch == 'n')
            ch = '\n';
    }
}
next()
{
    int t, l, a;
    while (isspace(ch) | ch == '#') {
        if (ch == '#') {
            inp();
            next();
            if (tok == TOK_DEFINE) {
                next();
                pdef(TAG_TOK); /* fill last ident tag */
                *(int *)tok = SYM_DEFINE;
                *(int *)(tok + 4) = dstk; /* define stack */
            }
            /* well we always save the values ! */
            while (ch != '\n') {
                pdef(ch);
                inp();
            }
            pdef(ch);
            pdef(TAG_MACRO);
        }
        inp();
    }
    tokl = 0;
    tok = ch;
    /* encode identifiers & numbers */
    if (isid()) {
        pdef(TAG_TOK);
        last_id = dstk;
        while (isid()) {
            pdef(ch);
            inp();
        }
        if (isdigit(tok)) {
            tokc = strtol(last_id, 0, 0);
            tok = TOK_NUM;
        } else {
            *(char *)dstk = TAG_TOK; /* no need to mark end of string (we
                                        suppose data is initied to zero */
            tok = strstr(sym_stk, last_id - 1) - sym_stk;
            *(char *)dstk = 0;   /* mark real end of ident for dlsym() */
            tok = tok * 8 + TOK_IDENT;
            if (tok > TOK_DEFINE) {
                tok = vars + tok;
                /*        printf("tok=%s %x\n", last_id, tok); */
                /* define handling */
                if (*(int *)tok == SYM_DEFINE) {
                    dptr = *(int *)(tok + 4);
                    dch = ch;
                    inp();
                    next();
                }
            }
        }
    } else {
        inp();
        if (tok == '\'') {
            tok = TOK_NUM;
            getq();
            tokc = ch;
            inp();
            inp();
        } else if (tok == '/' & ch == '*') {
            inp();
            while (ch) {
                while (ch != '*')
                    inp();
                inp();
                if (ch == '/')
                    ch = 0;
            }
            inp();
            next();
        } else
        {
            t = "++#m--%am*@R<^1c/@%[_[H3c%@%[_[H3c+@.B#d-@%:_^BKd<<Z/03e>>`/03e<=0f>=/f<@.f>@1f==&g!=\'g&&k||#l&@.BCh^@.BSi|@.B+j~@/%Yd!@&d*@b";
            while (l = *(char *)t++) {
                a = *(char *)t++;
                tokc = 0;
                while ((tokl = *(char *)t++ - 'b') < 0)
                    tokc = tokc * 64 + tokl + 64;
                if (l == tok & (a == ch | a == '@')) {
                    if (a == ch) {
                        inp();
                        tok = TOK_DUMMY; /* dummy token for double tokens */
                    }
                    break;
                }
            }
        }
    }
}
#ifdef TINY
#define skip(c) next()
#else
void error(char *fmt,...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%d: ", ftell((FILE *)file));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
    va_end(ap);
}
void skip(c)
{
    if (tok != c) {
        error("'%c' expected", c);
    }
    next();
}
#endif
/* from 0 to 4 bytes */
o(n)
{
    /* cannot use unsigned, so we must do a hack */
    while (n && n != -1) {
        *(char *)ind++ = n;
        n = n >> 8;
    }
}
/* put a 16 bit little endian word 'n' at unaligned address 't' */
put16(t, n)
{
    *(char *)t++ = n;
    *(char *)t = n >> 8;
}
/* put a 32 bit little endian word 'n' at unaligned address 't' */
put32(t, n)
{
    *(char *)t++ = n;
    *(char *)t++ = n >> 8;
    *(char *)t++ = n >> 16;
    *(char *)t++ = n >> 24;
}
/* get a 32 bit little endian word at unaligned address 't' */
get32(t)
{
    int n;
    return  (*(char *)t & 0xff) |
        (*(char *)(t + 1) & 0xff) << 8 |
        (*(char *)(t + 2) & 0xff) << 16 |
        (*(char *)(t + 3) & 0xff) << 24;
}
/* output a symbol and patch all references to it */
gsym1(t, b)
{
    int n;
    while (t) {
        n = get32(t); /* next value */
        /* patch absolute reference (always mov/lea before) */
        if (*(char *)(t - 1) == 0x05) {
            /* XXX: incorrect if data < 0 */
            if (b >= data && b < glo)
                put32(t, b + data_offset);
            else
                put32(t, b - prog + text + data_offset);
        } else {
            put32(t, b - t - 4);
        }
        t = n;
    }
}
gsym(t)
{
    gsym1(t, ind);
}
/* psym is used to put an instruction with a data field which is a
   reference to a symbol. It is in fact the same as oad ! */
#define psym oad
/* instruction + address */
oad(n, t)
{
    o(n);
    put32(ind, t);
    t = ind;
    ind = ind + 4;
    return t;
}
/* load immediate value */
li(t)
{
    oad(0xb8, t); /* mov $xx, %eax */
}
gjmp(t)
{
    return psym(0xe9, t);
}
/* l = 0: je, l == 1: jne */
gtst(l, t)
{
    o(0x0fc085); /* test %eax, %eax, je/jne xxx */
    return psym(0x84 + l, t);
}
gcmp(t)
{
    o(0xc139); /* cmp %eax,%ecx */
    li(0);
    o(0x0f); /* setxx %al */
    o(t + 0x90);
    o(0xc0);
}
gmov(l, t)
{
    int n;
    o(l + 0x83);
    n = *(int *)t;
    if (n && n < LOCAL)
        oad(0x85, n);
    else {
        t = t + 4;
        *(int *)t = psym(0x05, *(int *)t);
    }
}
/* l is one if '=' parsing wanted (quick hack) */
unary(l)
{
    int n, t, a, c;
    n = 1; /* type of expression 0 = forward, 1 = value, other =
              lvalue */
    if (tok == '\"') {
        li(glo + data_offset);
        while (ch != '\"') {
            getq();
            *(char *)glo++ = ch;
            inp();
        }
        *(char *)glo = 0;
        glo = glo + 4 & -4; /* align heap */
        inp();
        next();
    } else {
        c = tokl;
        a = tokc;
        t = tok;
        next();
        if (t == TOK_NUM) {
            li(a);
        } else if (c == 2) {
            /* -, +, !, ~ */
            unary(0);
            oad(0xb9, 0); /* movl $0, %ecx */
            if (t == '!')
                gcmp(a);
            else
                o(a);
        } else if (t == '(') {
            expr();
            skip(')');
        } else if (t == '*') {
            /* parse cast */
            skip('(');
            t = tok; /* get type */
            next(); /* skip int/char/void */
            next(); /* skip '*' or '(' */
            if (tok == '*') {
                /* function type */
                skip('*');
                skip(')');
                skip('(');
                skip(')');
                t = 0;
            }
            skip(')');
            unary(0);
            if (tok == '=') {
                next();
                o(0x50); /* push %eax */
                expr();
                o(0x59); /* pop %ecx */
                o(0x0188 + (t == TOK_INT)); /* movl %eax/%al, (%ecx) */
            } else if (t) {
                if (t == TOK_INT)
                    o(0x8b); /* mov (%eax), %eax */
                else 
                    o(0xbe0f); /* movsbl (%eax), %eax */
                ind++; /* add zero in code */
            }
        } else if (t == '&') {
            gmov(10, tok); /* leal EA, %eax */
            next();
        } else {
            n = 0;
            if (tok == '=' & l) {
                /* assignment */
                next();
                expr();
                gmov(6, t); /* mov %eax, EA */
            } else if (tok != '(') {
                /* variable */
                gmov(8, t); /* mov EA, %eax */
                if (tokl == 11) {
                    gmov(0, t);
                    o(tokc);
                    next();
                }
            }
        }
    }
    /* function call */
    if (tok == '(') {
        if (n)
            o(0x50); /* push %eax */
        /* push args and invert order */
        a = oad(0xec81, 0); /* sub $xxx, %esp */
        next();
        l = 0;
        while(tok != ')') {
            expr();
            oad(0x248489, l); /* movl %eax, xxx(%esp) */
            if (tok == ',')
                next();
            l = l + 4;
        }
        put32(a, l);
        next();
        if (n) {
            oad(0x2494ff, l); /* call *xxx(%esp) */
            l = l + 4;
        } else {
            /* forward reference */
            t = t + 4;
            *(int *)t = psym(0xe8, *(int *)t);
        }
        if (l)
            oad(0xc481, l); /* add $xxx, %esp */
    }
}
sum(l)
{
    int t, n, a;
    if (l-- == 1)
        unary(1);
    else {
        sum(l);
        a = 0;
        while (l == tokl) {
            n = tok;
            t = tokc;
            next();
            if (l > 8) {
                a = gtst(t, a); /* && and || output code generation */
                sum(l);
            } else {
                o(0x50); /* push %eax */
                sum(l);
                o(0x59); /* pop %ecx */
                
                if (l == 4 | l == 5) {
                    gcmp(t);
                } else {
                    o(t);
                    if (n == '%')
                        o(0x92); /* xchg %edx, %eax */
                }
            }
        }
        /* && and || output code generation */
        if (a && l > 8) {
            a = gtst(t, a);
            li(t ^ 1);
            gjmp(5); /* jmp $ + 5 */
            gsym(a);
            li(t);
        }
    }
}
expr()
{
    sum(11);
}
test_expr()
{
    expr();
    return gtst(0, 0);
}
block(l)
{
    int a, n, t;
    if (tok == TOK_IF) {
        next();
        skip('(');
        a = test_expr();
        skip(')');
        block(l);
        if (tok == TOK_ELSE) {
            next();
            n = gjmp(0); /* jmp */
            gsym(a);
            block(l);
            gsym(n); /* patch else jmp */
        } else {
            gsym(a); /* patch if test */
        }
    } else if (tok == TOK_WHILE | tok == TOK_FOR) {
        t = tok;
        next();
        skip('(');
        if (t == TOK_WHILE) {
            n = ind;
            a = test_expr();
        } else {
            if (tok != ';')
                expr();
            skip(';');
            n = ind;
            a = 0;
            if (tok != ';')
                a = test_expr();
            skip(';');
            if (tok != ')') {
                t = gjmp(0);
                expr();
                gjmp(n - ind - 5);
                gsym(t);
                n = t + 4;
            }
        }
        skip(')');
        block(&a);
        gjmp(n - ind - 5); /* jmp */
        gsym(a);
    } else if (tok == '{') {
        next();
        /* declarations */
        decl(1);
        while(tok != '}')
            block(l);
        next();
    } else {
        if (tok == TOK_RETURN) {
            next();
            if (tok != ';')
                expr();
            rsym = gjmp(rsym); /* jmp */
        } else if (tok == TOK_BREAK) {
            next();
            *(int *)l = gjmp(*(int *)l);
        } else if (tok != ';')
            expr();
        skip(';');
    }
}
/* 'l' is true if local declarations */
decl(l)
{
    int a;
    while (tok == TOK_INT | tok != -1 & !l) {
        if (tok == TOK_INT) {
            next();
            while (tok != ';') {
                if (l) {
                    loc = loc + 4;
                    *(int *)tok = -loc;
                } else {
                    *(int *)tok = glo;
                    glo = glo + 4;
                }
                next();
                if (tok == ',') 
                    next();
            }
            skip(';');
        } else {
            /* put function address */
            *(int *)tok = ind;
            next();
            skip('(');
            a = 8;
            while (tok != ')') {
                /* read param name and compute offset */
                *(int *)tok = a;
                a = a + 4;
                next();
                if (tok == ',')
                    next();
            }
            next(); /* skip ')' */
            rsym = loc = 0;
            o(0xe58955); /* push   %ebp, mov %esp, %ebp */
            a = oad(0xec81, 0); /* sub $xxx, %esp */
            block(0);
            gsym(rsym);
            o(0xc3c9); /* leave, ret */
            put32(a, loc); /* save local variables */
        }
    }
}
/* true if the symbol 's' is exported by kernel32.dll */
dllof(s)
{
    int i;
    for (i = 0; k32name[i]; i++)
        if (!strcmp(s, k32name[i]))
            return 1;
    for (i = 0; u32name[i]; i++)
        if (!strcmp(s, u32name[i]))
            return 2;
    return 0;
}
#define IMPLIT(s, d) pe_imp(s, sizeof(s) - 1, d)
/* import the symbol 'a' (length 'n') from dll 'd'. A 6 byte thunk
   'jmp *IAT' is emitted in the code so that the generated 'call'
   instructions (relative) can reach it. Return its address in prog. */
pe_imp(a, n, d)
{
    if (nimp >= 300)
        error("too many imports");
    imp_name[nimp] = a;
    imp_len[nimp] = n;
    imp_dll[nimp] = d;
    imp_stub[nimp] = ind;
    o(0xff);
    o(0x25);
    put32(ind, 0); /* patched with the address of the IAT entry */
    ind = ind + 4;
    dllcnt[d]++;
    nimp++;
    return imp_stub[nimp - 1];
}
/* patch all symbol references. Undefined symbols are imported: their
   use points are redirected to the thunk emitted above */
pe_syms()
{
    int t, a, n, b, d, c, v;
    t = sym_stk;
    while (1) {
        t++;
        a = t;
        while (*(char *)t != TAG_TOK && t < dstk)
            t++;
        if (t == dstk)
            break;
        tok = vars + (a - sym_stk) * 8 + TOK_IDENT - 8;
        b = *(int *)tok;
        n = *(int *)(tok + 4);
        if (n && b != 1) {
            if (!b) {
                /* external symbol: find its dll */
                c = *(char *)t;
                *(char *)t = 0;
                d = dllof((char *)a);
                *(char *)t = c;
                v = pe_imp(a, t - a, d);
                *(int *)tok = v;
                gsym1(n, v);
            } else {
                /* symbol defined in this file */
                gsym1(n, b);
            }
        }
    }
}
/* startup code:
   00 ff 15 xx xx xx xx   call [GetCommandLineA]
   06 89 c6               mov esi, eax
   08 bf xx xx xx xx      mov edi, argv
   0d 31 c9               xor ecx, ecx
   0f 8a 06               mov al, [esi]         <- next arg
   11 3c 20               cmp al, ' '
   13 74 04               je 0x19
   15 3c 09               cmp al, 9
   17 75 03               jne 0x1c
   19 46                  inc esi
   1a eb f3               jmp 0x0f
   1c 84 c0               test al, al
   1e 74 38               je 0x58
   20 89 37               mov [edi], esi
   22 83 c7 04            add edi, 4
   25 41                  inc ecx
   26 3c 22               cmp al, '"'
   28 75 17               jne 0x41
   2a 46                  inc esi
   2b 89 77 fc            mov [edi-4], esi
   2e 8a 06               mov al, [esi]
   30 84 c0               test al, al
   32 74 24               je 0x58
   34 3c 22               cmp al, '"'
   36 74 03               je 0x3b
   38 46                  inc esi
   39 eb f3               jmp 0x2e
   3b c6 06 00            mov byte [esi], 0
   3e 46                  inc esi
   3f eb ce               jmp 0x0f
   41 8a 06               mov al, [esi]
   43 84 c0               test al, al
   45 74 11               je 0x58
   47 3c 20               cmp al, ' '
   49 74 07               je 0x52
   4b 3c 09               cmp al, 9
   4d 74 03               je 0x52
   4f 46                  inc esi
   50 eb ef               jmp 0x41
   52 c6 06 00            mov byte [esi], 0
   55 46                  inc esi
   56 eb b7               jmp 0x0f
   58 c7 07 00 00 00 00   mov dword [edi], 0
   5e 89 0d xx xx xx xx   mov [argc], ecx
   64 68 xx xx xx xx      push argv
   69 51                  push ecx
   6a e8 xx xx xx xx      call main
   6f 50                  push eax
   70 ff 15 xx xx xx xx   call [exit]
   76 f4                  hlt
*/
char st[] =
    "\xff\x15\x00\x00\x00\x00"   /* 00 call [GetCommandLineA] */
    "\x89\xc6"                   /* 06 mov esi, eax */
    "\xbf\x00\x00\x00\x00"       /* 08 mov edi, argv */
    "\x31\xc9"                   /* 0d xor ecx, ecx */
    "\x8a\x06"                   /* 0f mov al, [esi] */
    "\x3c\x20"                   /* 11 cmp al, ' ' */
    "\x74\x04"                   /* 13 je 19 */
    "\x3c\x09"                   /* 15 cmp al, 9 */
    "\x75\x03"                   /* 17 jne 1c */
    "\x46"                       /* 19 inc esi */
    "\xeb\xf3"                   /* 1a jmp 0f */
    "\x84\xc0"                   /* 1c test al, al */
    "\x74\x38"                   /* 1e je 58 */
    "\x89\x37"                   /* 20 mov [edi], esi */
    "\x83\xc7\x04"               /* 22 add edi, 4 */
    "\x41"                       /* 25 inc ecx */
    "\x3c\x22"                   /* 26 cmp al, '"' */
    "\x75\x17"                   /* 28 jne 41 */
    "\x46"                       /* 2a inc esi */
    "\x89\x77\xfc"               /* 2b mov [edi-4], esi */
    "\x8a\x06"                   /* 2e mov al, [esi] */
    "\x84\xc0"                   /* 30 test al, al */
    "\x74\x24"                   /* 32 je 58 */
    "\x3c\x22"                   /* 34 cmp al, '"' */
    "\x74\x03"                   /* 36 je 3b */
    "\x46"                       /* 38 inc esi */
    "\xeb\xf3"                   /* 39 jmp 2e */
    "\xc6\x06\x00"               /* 3b mov byte [esi], 0 */
    "\x46"                       /* 3e inc esi */
    "\xeb\xce"                   /* 3f jmp 0f */
    "\x8a\x06"                   /* 41 mov al, [esi] */
    "\x84\xc0"                   /* 43 test al, al */
    "\x74\x11"                   /* 45 je 58 */
    "\x3c\x20"                   /* 47 cmp al, ' ' */
    "\x74\x07"                   /* 49 je 52 */
    "\x3c\x09"                   /* 4b cmp al, 9 */
    "\x74\x03"                   /* 4d je 52 */
    "\x46"                       /* 4f inc esi */
    "\xeb\xef"                   /* 50 jmp 41 */
    "\xc6\x06\x00"               /* 52 mov byte [esi], 0 */
    "\x46"                       /* 55 inc esi */
    "\xeb\xb7"                   /* 56 jmp 0f */
    "\xc7\x07\x00\x00\x00\x00"   /* 58 mov dword [edi], 0 */
    "\x89\x0d\x00\x00\x00\x00"   /* 5e mov [argc], ecx */
    "\x68\x00\x00\x00\x00"       /* 64 push argv */
    "\x51"                       /* 69 push ecx */
    "\xe8\x00\x00\x00\x00"       /* 6a call main */
    "\x50"                       /* 6f push eax */
    "\xff\x15\x00\x00\x00\x00"   /* 70 call [exit] */
    "\xf4";                      /* 76 hlt */
#define STARTUP_SIZE (sizeof(st) - 1)
pe_out(c)
{
    int i, d, n, t, irel, vsz, rawsz, sizeimg, tsize, ifirst, itot;
    int ilt[4], iat[4], dn[4], idx[4];
    /*****************************/
    /* code segment: startup + code + import thunks */
    text = glo;
    memcpy(prog, st, STARTUP_SIZE);
    /* the startup code always needs these two */
    IMPLIT("GetCommandLineA", 1);
    IMPLIT("exit", 0);
    /* patch all symbol references, import the undefined ones */
    pe_syms();
    tsize = ind - prog;
    glo = glo + tsize;
    /*****************************/
    /* import directory */
    glo = (glo + 3) & -4;
    irel = glo;
    glo = glo + 20 * 4; /* one descriptor per dll + the null one */
    for (i = 0; i < 4; i++) {
        ilt[i] = 0;
        iat[i] = 0;
        dn[i] = 0;
        idx[i] = 0;
    }
    /* dll names */
    for (d = 0; d < 3; d++) {
        if (dllcnt[d]) {
            dn[d] = glo;
            n = strlen(dllname[d]) + 1;
            memcpy(glo, dllname[d], n);
            glo = (glo + n + 3) & -4;
        }
    }
    /* import lookup tables, then all the import address tables (kept
       contiguous so that the IAT data directory is meaningful) */
    for (d = 0; d < 3; d++) {
        if (dllcnt[d]) {
            ilt[d] = glo;
            glo = glo + dllcnt[d] * 4 + 4;
        }
    }
    ifirst = 0;
    itot = 0;
    for (d = 0; d < 3; d++) {
        if (dllcnt[d]) {
            if (!ifirst)
                ifirst = glo;
            iat[d] = glo;
            glo = glo + dllcnt[d] * 4 + 4;
            itot = itot + dllcnt[d] * 4 + 4;
        }
    }
    /* hint/name entries */
    for (i = 0; i < nimp; i++) {
        imp_hn[i] = glo;
        glo = glo + 2; /* 16 bit hint == 0 */
        memcpy(glo, imp_name[i], imp_len[i]);
        glo = (glo + imp_len[i] + 2) & -2;
    }
    /*****************************/
    /* fill the tables and patch the code thunks */
    for (i = 0; i < nimp; i++) {
        d = imp_dll[i];
        imp_iat[i] = iat[d] + idx[d] * 4;
        put32(ilt[d] + idx[d] * 4, RVA(imp_hn[i]));
        put32(iat[d] + idx[d] * 4, RVA(imp_hn[i]));
        idx[d]++;
        put32(imp_stub[i] + 2, VA(imp_iat[i]));
    }
    for (d = 0, n = 0; d < 3; d++) {
        if (dllcnt[d]) {
            put32(irel + n * 20 + 0, RVA(ilt[d]));      /* original first thunk */
            put32(irel + n * 20 + 4, 0);                /* timestamp */
            put32(irel + n * 20 + 8, 0);                /* forwarder chain */
            put32(irel + n * 20 + 12, RVA(dn[d]));      /* dll name */
            put32(irel + n * 20 + 16, RVA(iat[d]));     /* first thunk */
            n++;
        }
    }
    /*****************************/
    /* patch the startup code */
    t = *(int *)(vars + TOK_MAIN);
    if (!t)
        error("'main' is not defined");
    put32(prog + 2, VA(imp_iat[0]));                /* call [GetCommandLineA] */
    put32(prog + 9, VA(data + PE_SCRATCH + 16));    /* mov edi, argv */
    put32(prog + 0x60, VA(data + PE_SCRATCH));      /* mov [argc], ecx */
    put32(prog + 0x65, VA(data + PE_SCRATCH + 16)); /* push argv */
    put32(prog + 0x6b, t - prog - 0x6f);            /* call main */
    put32(prog + 0x72, VA(imp_iat[1]));             /* call [exit] */
    /* copy the code AFTER relocation is done */
    memcpy(text, prog, tsize);
    /*****************************/
    /* pe headers. only 'MZ' and the offset of the pe header are needed
       in the dos header */
    vsz = glo - data - PE_HDR;
    rawsz = (vsz + 0x1ff) & -0x200;
    sizeimg = (PE_RVA + vsz + 0xfff) & -0x1000;
    *(char *)data = 'M';
    *(char *)(data + 1) = 'Z';
    put32(data + 0x3c, 0x40);       /* e_lfanew */
    put32(data + 0x40, 0x4550);     /* 'PE' 0 0 */
    put16(data + 0x44, 0x014c);     /* machine: i386 */
    put16(data + 0x46, 1);          /* number of sections */
    put32(data + 0x48, 0);          /* timestamp */
    put32(data + 0x4c, 0);          /* symbol table */
    put32(data + 0x50, 0);          /* number of symbols */
    put16(data + 0x54, 0xe0);       /* size of the optional header */
    put16(data + 0x56, 0x0103);     /* exec, 32 bit, no relocation */
    put16(data + 0x58, 0x10b);      /* optional header magic (pe32) */
    *(char *)(data + 0x5a) = 2;     /* linker version */
    *(char *)(data + 0x5b) = 0;
    put32(data + 0x5c, rawsz);      /* size of code */
    put32(data + 0x60, 0);          /* size of initialized data */
    put32(data + 0x64, 0);          /* size of uninitialized data */
    put32(data + 0x68, RVA(text));  /* entry point */
    put32(data + 0x6c, PE_RVA);     /* base of code */
    put32(data + 0x70, PE_RVA);     /* base of data */
    put32(data + 0x74, PE_BASE);    /* image base */
    put32(data + 0x78, PE_RVA);     /* section alignment */
    put32(data + 0x7c, 0x200);      /* file alignment */
    put16(data + 0x80, 4);          /* os version */
    put16(data + 0x82, 0);
    put16(data + 0x84, 0);          /* image version */
    put16(data + 0x86, 0);
    put16(data + 0x88, 4);          /* subsystem version */
    put16(data + 0x8a, 0);
    put32(data + 0x8c, 0);          /* win32 version */
    put32(data + 0x90, sizeimg);    /* size of image */
    put32(data + 0x94, PE_HDR);     /* size of headers */
    put32(data + 0x98, 0);          /* checksum */
    put16(data + 0x9c, 3);          /* console subsystem */
    put16(data + 0x9e, 0);          /* dll characteristics */
    put32(data + 0xa0, 0x100000);   /* stack reserve */
    put32(data + 0xa4, 0x4000);     /* stack commit */
    put32(data + 0xa8, 0x100000);   /* heap reserve */
    put32(data + 0xac, 0x1000);     /* heap commit */
    put32(data + 0xb0, 0);          /* loader flags */
    put32(data + 0xb4, 16);         /* number of rva and sizes */
    put32(data + 0xc0, RVA(irel));  /* import directory rva */
    put32(data + 0xc4, (n + 1) * 20);
    put32(data + 0x118, RVA(ifirst));/* import address table rva */
    put32(data + 0x11c, itot);
    /* one section: '.text' is read/write/executable and holds
       everything (code, data and import tables) */
    memcpy(data + 0x138, ".text", 6);
    put32(data + 0x140, vsz);       /* virtual size */
    put32(data + 0x144, PE_RVA);    /* virtual address */
    put32(data + 0x148, rawsz);     /* size of raw data */
    put32(data + 0x14c, PE_HDR);    /* pointer to raw data */
    put32(data + 0x150, 0);         /* relocations */
    put32(data + 0x154, 0);         /* line numbers */
    put16(data + 0x158, 0);
    put16(data + 0x15a, 0);
    put32(data + 0x15c, 0xe0000060); /* cnt_code | initialized data | rwx */
    t = fopen(c, "wb");
    if (!t) {
        printf("cannot open '%s'\n", c);
        return;
    }
    fwrite(data, 1, PE_HDR + rawsz, t);
    fclose(t);
}
main(n, t)
{
    if (n < 3) {
        printf("usage: otccpen file.c outfile\n");
        return 0;
    }
    dstk = strcpy(sym_stk = calloc(1, ALLOC_SIZE), 
                  " int if else while break return for define main ") + TOK_STR_SIZE;
    glo = data = calloc(1, ALLOC_SIZE);
    ind = prog = calloc(1, ALLOC_SIZE);
    vars = calloc(1, ALLOC_SIZE);
    t = t + 4;
    file = fopen(*(int *)t, "r");
    if (!file) {
        printf("cannot open '%s'\n", *(int *)t);
        return 1;
    }
    /* absolute address of the buffer == VA - data_offset */
    data_offset = PE_BASE + PE_RVA - PE_HDR - data;
    glo = glo + PE_GLO;
    ind = ind + STARTUP_SIZE;
    inp();
    next();
    decl(0);
    t = t + 4;
    pe_out(*(int *)t);
    return 0;
}
