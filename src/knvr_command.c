#include "knvr_command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool knvr_command_from_env(
    const char *variable, char *storage, size_t storage_size,
    const char **argv, size_t capacity)
{
    const char *value;
    size_t count = 0u;
    char *at;

    if (variable == NULL || storage == NULL || argv == NULL ||
        storage_size == 0u || capacity < 2u) {
        return false;
    }
    value = getenv(variable);
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (strlen(value) >= storage_size) {
        return false;
    }
    (void)snprintf(storage, storage_size, "%s", value);

    at = storage;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') {
            at++;
        }
        if (*at == '\0') {
            break;
        }
        if (count + 1u >= capacity) {
            /* Too many words: refuse the whole thing rather than run the
             * first few, which would be a different command wearing the
             * same name. */
            return false;
        }
        argv[count++] = at;
        while (*at != '\0' && *at != ' ' && *at != '\t') {
            at++;
        }
        if (*at != '\0') {
            *at++ = '\0';
        }
    }
    if (count == 0u) {
        return false;
    }
    argv[count] = NULL;
    return true;
}
