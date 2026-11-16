# otccn
The non-obfuscated Tiny C Compiler. Come from https://bellard.org/otcc/ Copyright (c) 2002 Fabrice Bellard

I made some changes. It can be running successfully under Linux and windows. April 5，2022

This code must be compiled into 32-bit software.

cygwin x86 and linux i386:

compile:
gcc otccn_u.c -o otccn_u -ldl
example:
./otccn_u otccex.c 12

windows tinycc 0.9.27 win32

compile:
tcc otccn_w    

example:
otccn_w otccex.c 12


# otccelfn
otccelfn是作者原版elf后端版
# otccpen
otccpen是workbuddy修改后的exe后端版。
# otccinit
tccinit是Fabrice Bellad在开发完整C编译器的初版，也就是otcc的非混淆C代码的清晰版。适合研究学习。本版本用32位gcc直接编译即可运行，windows/linux都可运行。



