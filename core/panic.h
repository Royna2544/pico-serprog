#pragma once

extern void __attribute__((noreturn, format(printf, 1, 2)))
do_panic(const char *fmt, ...);
