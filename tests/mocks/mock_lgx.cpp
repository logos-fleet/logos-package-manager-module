// Minimal lgx C API mock for unit tests (package_manager_impl keyring calls).

#include <logos_clib_mock.h>
#include <lgx.h>

#include <cstdlib>
#include <cstring>

extern "C" {

lgx_result_t lgx_keyring_add(const char* keyring_dir,
                             const char* name,
                             const char* did,
                             const char* display_name,
                             const char* url) {
    LOGOS_CMOCK_RECORD("lgx_keyring_add");
    (void)keyring_dir;
    (void)name;
    (void)did;
    (void)display_name;
    (void)url;
    lgx_result_t r;
    r.success = true;
    r.error = nullptr;
    return r;
}

lgx_result_t lgx_keyring_remove(const char* keyring_dir, const char* name) {
    LOGOS_CMOCK_RECORD("lgx_keyring_remove");
    (void)keyring_dir;
    (void)name;
    lgx_result_t r;
    r.success = true;
    r.error = nullptr;
    return r;
}

lgx_keyring_list_t lgx_keyring_list(const char* keyring_dir) {
    LOGOS_CMOCK_RECORD("lgx_keyring_list");
    (void)keyring_dir;
    lgx_keyring_list_t list;
    list.keys = nullptr;
    list.count = 0;
    return list;
}

void lgx_free_keyring_list(lgx_keyring_list_t list) {
    LOGOS_CMOCK_RECORD("lgx_free_keyring_list");
    (void)list;
}

// ---------------------------------------------------------------------------
// Package loading / inspection — unit tests never invoke inspectPackage, but
// the symbols must exist at link time because package_manager_impl.cpp
// references them unconditionally. Stubs return benign zeroes/nulls.
// ---------------------------------------------------------------------------

lgx_package_t lgx_load(const char* path) {
    LOGOS_CMOCK_RECORD("lgx_load");
    (void)path;
    // nullptr by default, which is what "unit tests never load a package" meant
    // before signerTrust() existed. A test that needs to get PAST the load opts
    // in with lgx_load_ok and supplies lgx_get_name / lgx_get_version; the
    // handle itself is never dereferenced by the mock.
    if (LOGOS_CMOCK_RETURN(bool, "lgx_load_ok"))
        return reinterpret_cast<lgx_package_t>(1);
    return nullptr;
}

void lgx_free_package(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_free_package");
    (void)pkg;
}

const char* lgx_get_last_error(void) {
    LOGOS_CMOCK_RECORD("lgx_get_last_error");
    return nullptr;
}

const char* lgx_get_name(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_get_name");
    (void)pkg;
    return LOGOS_CMOCK_RETURN_STRING("lgx_get_name");
}

const char* lgx_get_version(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_get_version");
    (void)pkg;
    return LOGOS_CMOCK_RETURN_STRING("lgx_get_version");
}

const char* lgx_get_description(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_get_description");
    (void)pkg;
    return nullptr;
}

const char* lgx_get_manifest_json(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_get_manifest_json");
    (void)pkg;
    return nullptr;
}

const char** lgx_get_variants(lgx_package_t pkg) {
    LOGOS_CMOCK_RECORD("lgx_get_variants");
    (void)pkg;
    return nullptr;
}

// Identity: the input alone, no architecture aliases. Aliasing is the real
// library's table and is asserted against it in the integration suite; a mock
// that invented aliases would make the unit tests agree with a fiction.
const char** lgx_variant_spellings(const char* variant) {
    LOGOS_CMOCK_RECORD("lgx_variant_spellings");
    const char** out = static_cast<const char**>(std::malloc(2 * sizeof(char*)));
    if (!out) return nullptr;
    out[0] = variant ? ::strdup(variant) : nullptr;
    out[1] = nullptr;
    return out;
}

void lgx_free_string_array(const char** array) {
    LOGOS_CMOCK_RECORD("lgx_free_string_array");
    (void)array;
}

} // extern "C"
