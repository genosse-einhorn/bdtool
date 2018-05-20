// Copyright (c) 2018 Jonas Kümmerlin <jonas@kuemmerlin.eu>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct {
    size_t allocated_size;
    size_t used_size;
    char str[];
} strbuf;

static inline void strbuf_realloc(strbuf **buf, size_t newsize)
{
    if (newsize == 0)
        newsize = 1;

    strbuf *newbuf = realloc(*buf, sizeof(strbuf) + newsize);
    if (!newbuf) {
        perror("strbuf_realloc");
        abort();
    }

    newbuf->allocated_size = newsize;
    newbuf->str[newsize-1] = 0;

    if (!*buf) {
        // malloc case
        newbuf->used_size = 0;
        newbuf->str[0] = 0;
    }

    *buf = newbuf;
}

static inline strbuf *strbuf_alloc(void)
{
    strbuf *p = NULL;
    strbuf_realloc(&p, 32-sizeof(strbuf));

    return p;
}

static inline void strbuf_free(strbuf *buf)
{
    free(buf);
}

static inline void strbuf_ensure_space(strbuf **pbuf, size_t space)
{
    size_t newsize = (*pbuf)->allocated_size;
    while (newsize - (*pbuf)->used_size < space)
        newsize = newsize * 2 + sizeof(strbuf);

    if (newsize != (*pbuf)->allocated_size)
        strbuf_realloc(pbuf, newsize);
}

static inline void strbuf_append_len(strbuf **pbuf, const char *str, size_t len)
{
    strbuf_ensure_space(pbuf, len + 1);

    memcpy(&(*pbuf)->str[(*pbuf)->used_size], str, len);
    (*pbuf)->used_size += len;
    (*pbuf)->str[(*pbuf)->used_size] = 0;
}

static inline void strbuf_append(strbuf **pbuf, const char *str)
{
    strbuf_append_len(pbuf, str, strlen(str));
}

static inline void strbuf_append_vprintf(strbuf **pbuf, const char *format, va_list args)
{
    va_list args2;
    va_copy(args2, args);

    int size = vsnprintf(NULL, 0, format, args);
    if (size < 0) {
        perror("strbuf_append_vprintf: while calling vsnprintf");
        abort();
    }

    strbuf_ensure_space(pbuf, (size_t)size + 1);

    (*pbuf)->used_size += (size_t)vsnprintf(&(*pbuf)->str[(*pbuf)->used_size], (size_t)size + 1, format, args2);

    va_end(args2);
}

#ifdef __GNUC__
__attribute__ ((format (printf, 2, 3)))
#endif
static inline void strbuf_append_printf(strbuf **pbuf, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    strbuf_append_vprintf(pbuf, format, args);

    va_end(args);
}
