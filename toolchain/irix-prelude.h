/* irix-prelude.h - included first by irix-cc. IRIX's headers make va_list
 * a char * unless _VA_LIST_ says it's there (gcc's MIPS va_list is one);
 * clang's is a void *: so it's clang's own, everywhere, from the start. */
#ifndef _VA_LIST_
#define _VA_LIST_
typedef __builtin_va_list va_list;
#endif
