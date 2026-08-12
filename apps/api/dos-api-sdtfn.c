#include "conio.h"
#include "string.h"
#include "stdlib.h"
#include "dos-api.h"

uint8_t inp(uint16_t port)
{
    CPU *cpu = get_PC()->cpu;
    return cpu->cb.io_read8(cpu->cb.io, port);
}

uint16_t inpw(uint16_t port)
{
    CPU *cpu = get_PC()->cpu;
    return cpu->cb.io_read16(cpu->cb.io, port);
}

void outp(uint16_t port, uint8_t value)
{
    CPU *cpu = get_PC()->cpu;
    cpu->cb.io_write8(cpu->cb.io, port, value);
}

void outpw(uint16_t port, uint16_t value)
{
    CPU *cpu = get_PC()->cpu;
    cpu->cb.io_write16(cpu->cb.io, port, value);
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n--)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && *src)
    {
        *dst++ = *src++;
        --n;
    }
    while (n--)
        *dst++ = '\0';
    return ret;
}

int strcmpi(const char *a, const char *b)
{
    for (;;)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

void strupr(char *s)
{
    while (*s)
    {
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - ('a' - 'A'));
        ++s;
    }
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
    return 0;
}

int atoi(const char *s)
{
    int sign = 1;
    int value = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        ++s;

    if (*s == '-' || *s == '+')
    {
        if (*s == '-')
            sign = -1;
        ++s;
    }

    while (*s >= '0' && *s <= '9')
    {
        value = value * 10 + (*s - '0');
        ++s;
    }

    return sign * value;
}
