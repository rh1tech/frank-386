#ifndef __NATIVE_DOS_CTYPE_H__
#define __NATIVE_DOS_CTYPE_H__

static inline int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');

    return c;
}

#endif /* __NATIVE_DOS_CTYPE_H__ */
