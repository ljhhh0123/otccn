# otccn
The non-obfuscated Tiny C Compiler. Come from https://bellard.org/otcc/ Copyright (c) 2002 Fabrice Bellard

I made some changes. It can be compiled successfully under Linux and windows. April 5，2022

This code must be compiled into 32-bit software.

cygwin x86 and linux i386:

compile:
gcc otccn_u.c -o otccn_u -ldl
example:
./otccn_u otccex.c 12

windows 
#tinycc 0.9.27 win32

compile:
tcc otccn_w    

example:
otccn_w otccex.c 12

