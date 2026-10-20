#include <haku/stdio.h>
#include <haku/string.h>
#include <haku/unistd.h>

#define PRINTF_CAPACITY 512u

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
} output_t;

static void emit(output_t *out, char ch) {
    if (out->used < out->capacity) out->data[out->used] = ch;
    out->used++;
}

static void emit_spaces(output_t *out, unsigned int count) {
    while (count--) emit(out, ' ');
}

static void emit_unsigned(output_t *out, unsigned int value,
                          unsigned int base, int upper, unsigned int width,
                          int zero_pad) {
    char digits[32];
    const char *table = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned int count = 0;
    do {
        digits[count++] = table[value % base];
        value /= base;
    } while (value);
    while (width > count) {
        emit(out, zero_pad ? '0' : ' ');
        width--;
    }
    while (count) emit(out, digits[--count]);
}

static int format_output(char *buffer, size_t capacity, const char *format,
                         unsigned int *args) {
    output_t out;
    const char *cursor;

    out.data = buffer;
    out.used = 0;
    out.capacity = capacity;

    for (cursor = format; *cursor; cursor++) {
        unsigned int width = 0;
        int zero_pad = 0;
        if (*cursor != '%') {
            emit(&out, *cursor);
            continue;
        }
        cursor++;
        if (*cursor == '%') {
            emit(&out, '%');
            continue;
        }
        if (*cursor == '0') {
            zero_pad = 1;
            cursor++;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            width = width * 10u + (unsigned int)(*cursor - '0');
            if (width > 200u) width = 200u;
            cursor++;
        }
        if (*cursor == 'l' || *cursor == 'h' || *cursor == 'z') cursor++;
        switch (*cursor) {
        case 'c': {
            char ch = (char)(*args++);
            if (width > 1u) emit_spaces(&out, width - 1u);
            emit(&out, ch);
            break;
        }
        case 's': {
            const char *text = (const char *)(*args++);
            size_t length;
            if (!text) text = "(null)";
            length = strlen(text);
            if (width > length) emit_spaces(&out, width - (unsigned int)length);
            while (*text) emit(&out, *text++);
            break;
        }
        case 'd':
        case 'i': {
            int value = (int)(*args++);
            unsigned int magnitude;
            if (value < 0) {
                emit(&out, '-');
                if (width) width--;
                magnitude = 0u - (unsigned int)value;
            } else {
                magnitude = (unsigned int)value;
            }
            emit_unsigned(&out, magnitude, 10u, 0, width, zero_pad);
            break;
        }
        case 'u':
            emit_unsigned(&out, *args++, 10u, 0, width, zero_pad);
            break;
        case 'x':
            emit_unsigned(&out, *args++, 16u, 0, width, zero_pad);
            break;
        case 'X':
            emit_unsigned(&out, *args++, 16u, 1, width, zero_pad);
            break;
        case 'p':
            emit(&out, '0');
            emit(&out, 'x');
            emit_unsigned(&out, *args++, 16u, 0, 8u, 1);
            break;
        case 0:
            emit(&out, '%');
            cursor--;
            break;
        default:
            emit(&out, '%');
            emit(&out, *cursor);
            break;
        }
    }
    return (int)(out.used < out.capacity ? out.used : out.capacity);
}

int putchar(int ch) {
    char byte = (char)ch;
    if (write(1, &byte, 1) != 1) return -1;
    return (unsigned char)byte;
}

int puts(const char *text) {
    size_t length = strlen(text);
    if (write(1, text, length) != (ssize_t)length) return -1;
    if (write(1, "\n", 1) != 1) return -1;
    return (int)(length + 1u);
}

int printf(const char *format, ...) {
    char buffer[PRINTF_CAPACITY];
    unsigned int *args = (unsigned int *)&format + 1;
    int length = format_output(buffer, sizeof(buffer), format, args);
    if (length > 0 && write(1, buffer, (size_t)length) != length) return -1;
    return length;
}
