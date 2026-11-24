#include <stddef.h>

/* Minimal freestanding C library primitives required by the pinned FatFs code. */
void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;
    for (size_t i = 0u; i < length; ++i)
        dst[i] = src[i];
    return destination;
}

void *memset(void *destination, int value, size_t length)
{
    unsigned char *dst = (unsigned char *)destination;
    for (size_t i = 0u; i < length; ++i)
        dst[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t length)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (size_t i = 0u; i < length; ++i) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *string)
{
    size_t length = 0u;
    while (string[length] != '\0')
        ++length;
    return length;
}

char *strchr(const char *string, int character)
{
    const char wanted = (char)character;
    for (;;) {
        if (*string == wanted)
            return (char *)string;
        if (*string == '\0')
            return 0;
        ++string;
    }
}
