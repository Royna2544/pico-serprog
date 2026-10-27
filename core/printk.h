#ifndef PRINTK_H
#define PRINTK_H

#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARN 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

#define pr_emerg(...) printk(LOG_EMERG, __VA_ARGS__)
#define pr_info(...) printk(LOG_INFO, __VA_ARGS__)
#define pr_warn(...) printk(LOG_WARN, __VA_ARGS__)
#define pr_err(...) printk(LOG_ERR, __VA_ARGS__)
#define pr_debug(...) printk(LOG_DEBUG, __VA_ARGS__)

extern void printk(int level, const char *fmt, ...);

#endif // PRINTK_H