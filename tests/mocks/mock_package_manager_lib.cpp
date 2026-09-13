// Mock PackageManagerLib for package_manager_module unit tests (link-time substitution).
//
// Everything the impl reads back from the library is registered via the
// setMock*() helpers in mock_package_manager_lib.h. Registries are file-static
// because each struct type carries std::string / std::vector members and
// can't go through LogosCMockStore's memcpy-based return slot.
//
// State reset: LogosTestContext constructor calls LogosCMockStore::reset(),
// which zeroes every recorded call. We hook into that by checking a sentinel
// call count on entry to every mock — count == 0 means the store was just
// reset, so we also clear our struct registries. Tests can then set up
// registries AFTER constructing LogosTestContext with the usual pattern.

#include <logos_clib_mock.h>
#include <package_manager_lib.h>

#include "mock_package_manager_lib.h"

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// File-static registries populated by setMock*()
// ---------------------------------------------------------------------------

std::vector<InstalledPackage>     s_installedPackages;
std::vector<InstalledPackage>     s_installedModules;
std::vector<InstalledPackage>     s_installedUiPlugins;
std::optional<DependencyTreeNode> s_dependencyTree;
std::optional<DependentTreeNode>  s_dependentTree;

// Sentinel key recorded on the first mock interaction per-test. When
// LogosCMockStore::reset() zeroes all call counts (inside LogosTestContext
// ctor), the sentinel disappears too — the next mock call sees count == 0,
// wipes the struct registries, and re-arms the sentinel.
constexpr const char* kResetSentinel = "__pm_mock_reset_sentinel__";

void ensureFreshStateForTest() {
    auto& store = LogosCMockStore::instance();
    if (store.callCount(kResetSentinel) == 0) {
        s_installedPackages.clear();
        s_installedModules.clear();
        s_installedUiPlugins.clear();
        s_dependencyTree.reset();
        s_dependentTree.reset();
        store.recordCall(kResetSentinel);
    }
}

std::string mockDupCStr(const char* key, const char* fallback) {
    const char* ret = LOGOS_CMOCK_RETURN_STRING(key);
    if (ret && ret[0])
        return std::string(ret);
    return fallback ? std::string(fallback) : std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// Setters (exposed via mock_package_manager_lib.h)
// ---------------------------------------------------------------------------

void setMockInstalledPackages(std::vector<InstalledPackage> v) {
    ensureFreshStateForTest();
    s_installedPackages = std::move(v);
}

void setMockInstalledModules(std::vector<InstalledPackage> v) {
    ensureFreshStateForTest();
    s_installedModules = std::move(v);
}

void setMockInstalledUiPlugins(std::vector<InstalledPackage> v) {
    ensureFreshStateForTest();
    s_installedUiPlugins = std::move(v);
}

void setMockDependencyTree(std::optional<DependencyTreeNode> tree) {
    ensureFreshStateForTest();
    s_dependencyTree = std::move(tree);
}

void setMockDependentTree(std::optional<DependentTreeNode> tree) {
    ensureFreshStateForTest();
    s_dependentTree = std::move(tree);
}

// ---------------------------------------------------------------------------
// PackageManagerLib method impls
// ---------------------------------------------------------------------------

PackageManagerLib::PackageManagerLib() {
    LOGOS_CMOCK_RECORD("PackageManagerLib_ctor");
    ensureFreshStateForTest();
}

PackageManagerLib::~PackageManagerLib() {
    LOGOS_CMOCK_RECORD("PackageManagerLib_dtor");
}

void PackageManagerLib::setEmbeddedModulesDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("setEmbeddedModulesDirectory");
    (void)dir;
}

void PackageManagerLib::addEmbeddedModulesDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("addEmbeddedModulesDirectory");
    (void)dir;
}

void PackageManagerLib::setEmbeddedUiPluginsDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("setEmbeddedUiPluginsDirectory");
    (void)dir;
}

void PackageManagerLib::addEmbeddedUiPluginsDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("addEmbeddedUiPluginsDirectory");
    (void)dir;
}

void PackageManagerLib::setUserModulesDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("setUserModulesDirectory");
    (void)dir;
}

void PackageManagerLib::setUserUiPluginsDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("setUserUiPluginsDirectory");
    (void)dir;
}

void PackageManagerLib::setInstallVariants(const std::vector<std::string>& variants) {
    LOGOS_CMOCK_RECORD("setInstallVariants");
    (void)variants;
}

std::string PackageManagerLib::installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                                 bool skipIfNotNewerVersion,
                                                 std::string* installedPluginPath,
                                                 bool* isCoreModule) {
    LOGOS_CMOCK_RECORD("installPluginFile");
    if (skipIfNotNewerVersion) {
        LOGOS_CMOCK_RECORD("installPluginFile_skipIfNotNewer_true");
    } else {
        LOGOS_CMOCK_RECORD("installPluginFile_skipIfNotNewer_false");
    }
    (void)pluginPath;

    errorMsg = mockDupCStr("installPluginFile_error", "");

    if (installedPluginPath) {
        *installedPluginPath = mockDupCStr("installPluginFile_installedPath", "");
    }
    if (isCoreModule) {
        *isCoreModule = LOGOS_CMOCK_RETURN(bool, "installPluginFile_isCore");
    }

    return mockDupCStr("installPluginFile_result", "");
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledPackages() {
    LOGOS_CMOCK_RECORD("getInstalledPackages");
    ensureFreshStateForTest();
    return s_installedPackages;
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledModules() {
    LOGOS_CMOCK_RECORD("getInstalledModules");
    ensureFreshStateForTest();
    return s_installedModules;
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledUiPlugins() {
    LOGOS_CMOCK_RECORD("getInstalledUiPlugins");
    ensureFreshStateForTest();
    return s_installedUiPlugins;
}

std::vector<std::string> PackageManagerLib::platformVariantsToTry() {
    LOGOS_CMOCK_RECORD("platformVariantsToTry");
    const char* v = LOGOS_CMOCK_RETURN_STRING("platformVariantsToTry_first");
    if (v && v[0]) {
        return {std::string(v)};
    }
    return {"mock-variant"};
}

void PackageManagerLib::setSignaturePolicy(SignaturePolicy policy) {
    LOGOS_CMOCK_RECORD("setSignaturePolicy");
    (void)policy;
}

void PackageManagerLib::setKeyringDirectory(const std::string& dir) {
    LOGOS_CMOCK_RECORD("setKeyringDirectory");
    (void)dir;
}

std::string PackageManagerLib::keyringDirectory() {
    LOGOS_CMOCK_RECORD("keyringDirectory");
    return mockDupCStr("keyringDirectory", "");
}

UninstallResult PackageManagerLib::uninstallPackage(const std::string& packageName) {
    LOGOS_CMOCK_RECORD("uninstallPackage");
    (void)packageName;
    UninstallResult r;
    r.success = LOGOS_CMOCK_RETURN(bool, "uninstallPackage_success");
    const char* err = LOGOS_CMOCK_RETURN_STRING("uninstallPackage_error");
    if (err && err[0]) {
        r.errorMsg = err;
    }
    const char* removed = LOGOS_CMOCK_RETURN_STRING("uninstallPackage_removed");
    if (removed && removed[0]) {
        // Comma-separated list for convenience
        std::string s(removed);
        std::string::size_type pos = 0, next;
        while ((next = s.find(',', pos)) != std::string::npos) {
            r.removedFiles.emplace_back(s.substr(pos, next - pos));
            pos = next + 1;
        }
        r.removedFiles.emplace_back(s.substr(pos));
    }
    return r;
}

std::optional<DependencyTreeNode> PackageManagerLib::resolveDependencies(const std::string& packageName) {
    LOGOS_CMOCK_RECORD("resolveDependencies");
    (void)packageName;
    ensureFreshStateForTest();
    return s_dependencyTree;
}

std::optional<DependentTreeNode> PackageManagerLib::resolveDependents(const std::string& packageName) {
    LOGOS_CMOCK_RECORD("resolveDependents");
    (void)packageName;
    ensureFreshStateForTest();
    return s_dependentTree;
}

// ---------------------------------------------------------------------------
// flatten() — hand-rolled copies of the real-lib implementations. The stub
// header mirrors the real header's declarations, so the tree structs expose
// the same member function; the mock provides bodies so link-time tests can
// exercise the same BFS-dedup behaviour without linking the real lib.
// ---------------------------------------------------------------------------

namespace {
// Mirrors edgeVerdictSeverity in the real library. Higher = judged more
// harshly by the edge that reached it; -1 for the statuses no edge decides.
int mockEdgeVerdictSeverity(DependencyStatus s) {
    switch (s) {
        case DependencyStatus::Installed:       return 0;
        case DependencyStatus::SignerUnknown:   return 1;
        case DependencyStatus::VersionMismatch: return 2;
        case DependencyStatus::SignerMismatch:  return 3;
        case DependencyStatus::NotInstalled:
        case DependencyStatus::Cycle:           return -1;
    }
    return -1;
}
} // namespace

std::vector<DependencyTreeNode> DependencyTreeNode::flatten() const {
    std::vector<DependencyTreeNode> out;
    std::unordered_map<std::string, std::size_t> indexByName;
    std::deque<const DependencyTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependencyTreeNode* n = queue.front();
        queue.pop_front();
        auto [slot, inserted] = indexByName.emplace(n->name, out.size());
        if (!inserted) {
            // The real library PROMOTES: one row per package, but a later edge
            // that judged it more harshly than the recorded one wins, carrying
            // its own constraint. Dedup-and-stop would show a satisfied row for
            // a package the real resolver rejects.
            DependencyTreeNode& recorded = out[slot->second];
            const int recordedRank  = mockEdgeVerdictSeverity(recorded.status);
            const int candidateRank = mockEdgeVerdictSeverity(n->status);
            if (recordedRank >= 0 && candidateRank > recordedRank) {
                recorded.status          = n->status;
                recorded.requiredVersion = n->requiredVersion;
                recorded.requiredSigner  = n->requiredSigner;
            }
            // Collapses conservatively and independently of the ranking above:
            // one required edge settles it, however many optional edges also
            // reach the package.
            recorded.optional = recorded.optional && n->optional;
            continue;
        }
        DependencyTreeNode copy;
        copy.name            = n->name;
        copy.status          = n->status;
        copy.version         = n->version;
        copy.installType     = n->installType;
        copy.requiredVersion = n->requiredVersion;
        copy.requiredSigner  = n->requiredSigner;
        copy.optional        = n->optional;
        copy.signerDid  = n->signerDid;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

std::vector<DependentTreeNode> DependentTreeNode::flatten() const {
    std::vector<DependentTreeNode> out;
    std::unordered_set<std::string> seen;
    std::deque<const DependentTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependentTreeNode* n = queue.front();
        queue.pop_front();
        if (!seen.insert(n->name).second) continue;
        DependentTreeNode copy;
        copy.name        = n->name;
        copy.version     = n->version;
        copy.type        = n->type;
        copy.installType = n->installType;
        copy.installDir  = n->installDir;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

SignatureVerificationResult PackageManagerLib::verifyPackageSignature(const std::string& lgxPath) {
    LOGOS_CMOCK_RECORD("verifyPackageSignature");
    (void)lgxPath;
    SignatureVerificationResult r;
    r.is_signed = LOGOS_CMOCK_RETURN(bool, "verifyPackageSignature_is_signed");
    r.signature_valid = LOGOS_CMOCK_RETURN(bool, "verifyPackageSignature_signature_valid");
    r.package_valid = LOGOS_CMOCK_RETURN(bool, "verifyPackageSignature_package_valid");
    const char* did = LOGOS_CMOCK_RETURN_STRING("verifyPackageSignature_signer_did");
    if (did && did[0]) {
        r.signer_did = did;
    }
    const char* name = LOGOS_CMOCK_RETURN_STRING("verifyPackageSignature_signer_name");
    if (name && name[0]) {
        r.signer_name = name;
    }
    const char* url = LOGOS_CMOCK_RETURN_STRING("verifyPackageSignature_signer_url");
    if (url && url[0]) {
        r.signer_url = url;
    }
    const char* trusted = LOGOS_CMOCK_RETURN_STRING("verifyPackageSignature_trusted_as");
    if (trusted && trusted[0]) {
        r.trusted_as = trusted;
    }
    const char* err = LOGOS_CMOCK_RETURN_STRING("verifyPackageSignature_error");
    if (err && err[0]) {
        r.error = err;
    }
    return r;
}

// installTypeToString / dependencyStatusToString are declared in
// package_manager_lib.h but referenced at link time by any JSON helper that
// serialises InstalledPackage / DependencyTreeNode. The real library supplies
// them; in unit tests they're not exercised because PackageManagerImpl builds
// LogosMap directly from the structs without touching them, but providing a
// stub keeps the mock self-contained should a future test need it.
const char* installTypeToString(InstallType t) {
    switch (t) {
        case InstallType::Embedded: return "embedded";
        case InstallType::User:     return "user";
    }
    return "user";
}

const char* dependencyStatusToString(DependencyStatus s) {
    switch (s) {
        case DependencyStatus::Installed:       return "installed";
        case DependencyStatus::NotInstalled:    return "not_installed";
        case DependencyStatus::Cycle:           return "cycle";
        case DependencyStatus::VersionMismatch: return "version_mismatch";
        case DependencyStatus::SignerMismatch:  return "signer_mismatch";
        case DependencyStatus::SignerUnknown:   return "signer_unknown";
    }
    return "not_installed";
}

bool nodeResolvedToAnInstalledPackage(DependencyStatus s) {
    return s != DependencyStatus::NotInstalled && s != DependencyStatus::Cycle;
}
