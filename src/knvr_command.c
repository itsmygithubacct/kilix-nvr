#include "knvr_command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void knvr_command_bundled(const char *name, char *out, size_t size)
{
    char self[1024];
    ssize_t length;

    if (name == NULL || out == NULL || size == 0u) {
        return;
    }
    (void)snprintf(out, size, "%s", name);
    length = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (length <= 0) {
        return;
    }
    self[length] = '\0';
    {
        char *slash = strrchr(self, '/');
        char beside[1200];
        char in_tools[1200];

        if (slash == NULL) {
            return;
        }
        *slash = '\0';
        /* Installed beside the binary, then the checkout's tools/. */
        (void)snprintf(beside, sizeof(beside), "%s/%s", self, name);
        (void)snprintf(in_tools, sizeof(in_tools), "%s/../tools/%s", self,
                       name);
        if (access(beside, X_OK) == 0 && strlen(beside) < size) {
            (void)snprintf(out, size, "%s", beside);
        } else if (access(in_tools, X_OK) == 0 && strlen(in_tools) < size) {
            (void)snprintf(out, size, "%s", in_tools);
        }
    }
}
