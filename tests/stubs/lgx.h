/**
 * Minimal lgx.h for unit tests only — only symbols referenced by package_manager_impl.cpp.
 * Integration tests use the real header from ../lib.
 */
#ifndef LGX_H
#define LGX_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool success;
    const char* error;
} lgx_result_t;

typedef struct {
    const char* name;
    const char* did;
    const char* display_name;
    const char* url;
    const char* added_at;
} lgx_keyring_key_t;

typedef struct {
    lgx_keyring_key_t* keys;
    size_t count;
} lgx_keyring_list_t;

/* Opaque package handle — used by inspectPackage(). Real layout lives in
 * ../lib; unit tests never invoke these functions (they're only referenced
 * from code paths that aren't exercised here), so a forward decl is enough. */
typedef struct lgx_package_opaque* lgx_package_t;

lgx_result_t lgx_keyring_add(const char* keyring_dir,
                             const char* name,
                             const char* did,
                             const char* display_name,
                             const char* url);

lgx_result_t lgx_keyring_remove(const char* keyring_dir, const char* name);

lgx_keyring_list_t lgx_keyring_list(const char* keyring_dir);

void lgx_free_keyring_list(lgx_keyring_list_t list);

/* Package loading / inspection — only declarations needed for unit-test
 * compilation; the real implementations are provided by the lgx library
 * at link time (integration tests) or stubbed out when unexercised. */
lgx_package_t lgx_load(const char* path);
void          lgx_free_package(lgx_package_t pkg);
const char*   lgx_get_last_error(void);
const char*   lgx_get_name(lgx_package_t pkg);
const char*   lgx_get_version(lgx_package_t pkg);
const char*   lgx_get_description(lgx_package_t pkg);
const char*   lgx_get_manifest_json(lgx_package_t pkg);
const char**  lgx_get_variants(lgx_package_t pkg);
void          lgx_free_string_array(const char** array);

/* Variant vocabulary — every live spelling of one variant's ARCHITECTURE half,
 * canonical caller spelling first. variantAvailability() consults it so a
 * published package using a legacy architecture spelling still resolves. The
 * unit-test mock is the identity (no aliasing); the aliasing itself is asserted
 * in the integration suite, against the real library. */
const char**  lgx_variant_spellings(const char* variant);

#ifdef __cplusplus
}
#endif

#endif /* LGX_H */
