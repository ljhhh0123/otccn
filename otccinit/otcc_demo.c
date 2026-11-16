/*
 * =====================================================================
 *  otcc_demo.c  --  OTCC / TCC 初版 支持的完整语法示例
 * =====================================================================
 *
 *  编译运行:
 *      gcc -o tc tcc_fixed.c -ldl
 *      ./tc otcc_demo.c
 *
 *  如果你的系统是 64 位宿主机, 需要加 -m32:
 *      gcc -m32 -o tc tcc_fixed.c -ldl
 *
 *  说明:
 *    OTCC 是 Fabrice Bellard 写的超小型 C 编译器, 只支持 C 的一个子集.
 *    本文件中出现的每一种语法都是经过实测可用的.
 *    文件末尾列出了"不支持"的语法, 避免踩坑.
 * =====================================================================
 */


/* ---------------------------------------------------------------------
 * 1. 外部函数声明
 *
 *    顶层声明必须以类型关键字开头 (int / void / char).
 *    写 "printf();" 或 "main(){}" 都会报错.
 *    不需要 #include, 把用到的 libc 函数声明一下即可.
 * --------------------------------------------------------------------- */
int printf();


/* ---------------------------------------------------------------------
 * 2. #define 宏
 *
 *    只支持最简单的对象式宏 (不带参数).
 *    不支持 #include / #ifdef / 带参数的宏.
 * --------------------------------------------------------------------- */
#define N 10
#define SIZE 4


/* ---------------------------------------------------------------------
 * 3. 全局变量
 *
 *    只能声明成 int, 且不能带初始化 (不能写 int g = 5;).
 *    需要在函数里赋值.
 * --------------------------------------------------------------------- */
int g_counter;
int g_total;


/* ---------------------------------------------------------------------
 * 4. 函数定义
 *
 *    必须是 K&R 风格: 参数只写名字, 不写类型 (一律按 int 处理).
 *    返回值类型必须显式写 int 或 void.
 *
 *    定义顺序无所谓: 可以先调用后定义, 前向引用会自动回填.
 * --------------------------------------------------------------------- */

/* 单参数, 有返回值 */
int square(x)
{
    return x * x;
}

/* 多参数 */
int add3(a, b, c)
{
    return a + b + c;
}

/* 递归 */
int fib(n)
{
    if (n <= 2)
        return 1;
    else
        return fib(n - 1) + fib(n - 2);
}

/* void 函数: 没有返回值 */
void print_int(v)
{
    printf("%d ", v);
}

/* 前向引用: next_even 在下面才定义, 这里先调用 */
int use_forward(x)
{
    return next_even(x);
}

int next_even(x)
{
    return x + (x & 1);
}


/* ---------------------------------------------------------------------
 * 5. 主函数
 * --------------------------------------------------------------------- */
int main()
{
    /* ---- 5.1 局部变量 ----
       必须写在函数体或语句块的"开头", 不能中途声明 (C89 规则).
       类型只支持 int 和 char, 指针只支持 int*.
       不支持数组、结构体、浮点. */

    int a;
    int b;
    int c;
    int i;
    int sum;
    int *p;
    char ch;


    /* ---- 5.2 算术运算: + - * / % ---- */
    printf("=== 1. arithmetic ===\n");
    a = 2 + 3 * 4;              /* 优先级: 2 + 12 = 14 */
    b = 10 / 3;                 /* 整除: 3 */
    c = 10 % 3;                 /* 取余: 1 */
    a = a - 4;                  /* 14 - 4 = 10 */
    printf("2+3*4-4=%d 10/3=%d 10%%3=%d\n", a, b, c);


    /* ---- 5.3 位运算 & | ^ ~, 移位 << >> ---- */
    printf("\n=== 2. bitwise and shift ===\n");
    a = 12 & 10;                /* 1100 & 1010 = 1000 = 8 */
    b = 12 | 10;                /* 1100 | 1010 = 1110 = 14 */
    c = 12 ^ 10;                /* 1100 ^ 1010 = 0110 = 6 */
    printf("12&10=%d 12|10=%d 12^10=%d\n", a, b, c);

    a = 1 << 4;                 /* 16 */
    b = 64 >> 2;                /* 16 */
    c = ~0;                     /* -1 */
    printf("1<<4=%d 64>>2=%d ~0=%d\n", a, b, c);


    /* ---- 5.4 比较运算与逻辑运算 ---- */
    printf("\n=== 3. comparison and logic ===\n");
    /* < > <= >= == != 结果都是 1 或 0 */
    printf("3<5=%d 5==5=%d 3!=4=%d 2<=2=%d 9>1=%d 8>=8=%d\n",
           3 < 5, 5 == 5, 3 != 4, 2 <= 2, 9 > 1, 8 >= 8);

    /* && 和 || 支持短路求值, ! 是逻辑非 */
    if (1 && 1)
        printf("1 && 1  -> true\n");
    if (0 || 1)
        printf("0 || 1  -> true\n");
    if (!0)
        printf("!0       -> true\n");


    /* ---- 5.5 条件分支 ---- */
    printf("\n=== 4. if / else if / else ===\n");
    a = 5;
    if (a > 10)
        printf("a>10\n");
    else if (a > 3)
        printf("3 < a <= 10\n");
    else
        printf("a<=3\n");


    /* ---- 5.6 while 循环 + break ---- */
    printf("\n=== 5. while + break ===\n");
    i = 0;
    sum = 0;
    while (i < 20) {
        if (i >= 5)
            break;              /* break 跳出最近的一层 while */
        sum = sum + i;
        i++;
    }
    printf("0+1+2+3+4=%d (i stopped at %d)\n", sum, i);
    /* 注意: 没有 for, 没有 do-while, 没有 continue */


    /* ---- 5.7 自增自减, 前置与后置都支持 ---- */
    printf("\n=== 6. increment / decrement ===\n");
    i = 5;
    printf("i=5  i++ -> %d", i++);   /* 先返回 5, 再变 6 */
    printf("  now i=%d\n", i);
    printf("++i -> %d\n", ++i);      /* 先变 7, 再返回 */
    printf("i-- -> %d", i--);        /* 先返回 7, 再变 6 */
    printf("  now i=%d\n", i);
    printf("--i -> %d\n", --i);      /* 先变 5, 再返回 */


    /* ---- 5.8 函数调用: 递归 / 多参数 / void / 前向引用 ---- */
    printf("\n=== 7. functions ===\n");
    printf("square(7)      = %d\n", square(7));
    printf("add3(1,2,3)    = %d\n", add3(1, 2, 3));
    printf("fib(10)        = %d\n", fib(10));
    printf("use_forward(4) = %d  (called before its definition)\n",
           use_forward(4));

    print_int(42);
    printf("<-- void function has no return value\n");


    /* ---- 5.9 指针: & 取地址, * 解引用, p[0] 下标 ---- */
    printf("\n=== 8. pointers ===\n");
    a = 100;
    p = &a;                     /* p 指向 a */
    printf("a=%d  *p=%d\n", a, *p);

    *p = 200;                   /* 通过指针修改 a */
    printf("after *p=200: a=%d\n", a);

    b = 300;
    p = &b;                     /* 改指向 b */
    printf("p now points to b:  *p=%d\n", *p);

    p = &a;
    printf("p[0]=%d\n", p[0]);  /* 下标等价于 *(p+0) */


    /* ---- 5.10 char 类型与字符常量 ---- */
    printf("\n=== 9. char ===\n");
    ch = 'A';
    printf("ch='A' -> %d  %c\n", ch, ch);
    ch = ch + 1;                /* 字符可以参与算术 */
    printf("ch+1   -> %c\n", ch);


    /* ---- 5.11 全局变量 ---- */
    printf("\n=== 10. global variables ===\n");
    g_counter = 0;
    g_total = 0;
    i = 0;
    while (i < 5) {
        g_counter = g_counter + 1;
        g_total = g_total + i;
        i++;
    }
    printf("g_counter=%d  g_total=%d\n", g_counter, g_total);


    /* ---- 5.12 #define 宏展开 ---- */
    printf("\n=== 11. define macro ===\n");
    a = N;                      /* 展开成 10 */
    b = N * 2;                  /* 展开成 10 * 2 */
    printf("N=%d  N*2=%d  SIZE=%d\n", a, b, SIZE);


    /* ---- 5.13 字符串转义 ---- */
    printf("\n=== 12. string escapes ===\n");
    /* 支持 \n 和八进制转义 \ooo; 不支持 \t \r 等 */
    printf("line1\nline2\n");
    printf("octal 101 = \101\n");   /* \101 就是 'A' */


    /* ---- 5.14 嵌套语句块 ---- */
    printf("\n=== 13. nested block ===\n");
    a = 1;
    {
        int inner;              /* 块内声明的变量, 放在块开头 */
        inner = a + 1;
        printf("inner block: inner=%d\n", inner);
    }


    printf("\ndone.\n");
    return 0;
}


/* =====================================================================
 *  实测确认: 以下语法"不支持", 请改用替代写法
 * =====================================================================
 *
 *  控制流
 *    for (i=0; i<n; i++)          用 while 代替
 *    do { } while ( )             用 while + 标志变量代替
 *    switch / case / default      用 if / else if 链代替
 *    continue                     用 if 包住剩余语句代替
 *
 *  运算符
 *    a ? b : c  三目条件          用 if / else 赋值代替
 *    += -= *= /= 等复合赋值        写成 a = a + b
 *
 *  数据类型
 *    long / short / unsigned      只有 int 和 char
 *    struct / union / enum        不支持
 *    数组 int a[10]               用指针 + 自己算偏移代替
 *    float / double               不支持, 全部用整数运算
 *
 *  声明
 *    int g = 5;                   全局变量不能初始化, 在函数里赋值
 *    int f(int x)                 参数不能写类型, 要写 int f(x)
 *    省略返回类型 main(){}         必须写 int main() 或 void main()
 *    中途声明变量                  必须先集中写在块开头
 *
 *  预处理
 *    #include                     用 int printf(); 这类声明代替
 *    #ifdef / #ifndef / #if       不支持
 *    #define F(x) 带参数的宏        不支持
 *
 *  其他
 *    sizeof                       不支持
 *    函数指针                     只部分支持
 *
 * ---------------------------------------------------------------------
 *  小技巧
 * ---------------------------------------------------------------------
 *  1. 需要字符串处理时, 可以用 libc 的 strcmp / strlen:
 *         int strcmp();
 *         int strlen();
 *
 *  2. 需要动态内存时:
 *         int malloc();
 *         int free();
 *         int *q;
 *         q = (int *)malloc(40);
 *
 *  3. 想确认某个语法是否支持, 直接编译一次看日志:
 *         ./tc yourfile.c 2>trace.txt
 *     日志会打印每个 token 和生成的字节数, 出错时会指出偏移和阶段.
 * ===================================================================== */
