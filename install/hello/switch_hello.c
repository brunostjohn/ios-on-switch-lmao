// SPDX-License-Identifier: GPL-3.0-or-later
#include <stddef.h>
#include <stdio.h>

int switch_hello_greeting(char *buffer, size_t size);

int switch_hello_greeting(char *buffer, size_t size)
{
    return snprintf(buffer, size, "hello from C on a %d-bit target", (int)(sizeof(void *) * 8));
}
