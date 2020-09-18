#include "ubiq/platform/internal/algorithm.h"

#include <string.h>

static
struct ubiq_platform_algorithm *
ubiq_platform_algorithms = NULL;
static
size_t
ubiq_platform_algorithms_n = 0;

/*
 * maps the openssl ciphers to the numeric id's that are
 * used in the ubiq headers to identify them.
 */
void
ubiq_platform_algorithm_init(
    void)
{
    const struct ubiq_platform_algorithm algos[] = {
        { .cipher = EVP_aes_256_gcm(), .taglen = 16 },
        { .cipher = EVP_aes_128_gcm(), .taglen = 16 },
    };

    ubiq_platform_algorithms = malloc(sizeof(algos));
    ubiq_platform_algorithms_n = sizeof(algos) / sizeof(*algos);

    for (unsigned int i = 0; i < ubiq_platform_algorithms_n; i++) {
        ubiq_platform_algorithms[i] = algos[i];
        ubiq_platform_algorithms[i].id = i;
    }
}

void
ubiq_platform_algorithm_exit(
    void)
{
    free(ubiq_platform_algorithms);
}

const struct ubiq_platform_algorithm *
ubiq_platform_algorithm_get_byid(
    const unsigned int i)
{
    if (i < ubiq_platform_algorithms_n) {
        return &ubiq_platform_algorithms[i];
    }

    return NULL;
}

const struct ubiq_platform_algorithm *
ubiq_platform_algorithm_get_bycipher(
    const EVP_CIPHER * const c)
{
    for (unsigned int i = 0; i < ubiq_platform_algorithms_n; i++) {
        if (ubiq_platform_algorithms[i].cipher == c) {
            return &ubiq_platform_algorithms[i];
        }
    }

    return NULL;
}