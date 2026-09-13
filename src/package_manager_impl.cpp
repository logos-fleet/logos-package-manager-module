#include "package_manager_impl.h"
#include <package_manager_lib.h>
#include <lgx.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>

// ---------------------------------------------------------------------------
// Struct → LogosMap / LogosList conversion helpers
// ---------------------------------------------------------------------------
//
// Wire format is identical to what PackageManagerLib / the lgpm CLI emit via
// package_manager_json.cpp's nlohmann ADL hooks. We re-hand-roll here (rather
// than reuse those hooks) so the unit tests can compile against the stub
// header in tests/stubs/package_manager_lib.h without pulling in the lib's
// JSON module.
//
// Keep these in sync with package_manager_json.cpp's to_json definitions.
// ---------------------------------------------------------------------------

namespace {

LogosMap toLogosMap(const Hashes& h)
{
    LogosMap m = LogosMap::object();
    m["root"] = h.root;
    return m;
}

LogosMap toLogosMap(const InstalledPackage& p)
{
    LogosMap m = LogosMap::object();
    m["name"]         = p.name;
    m["displayName"]  = p.displayName;
    m["version"]      = p.version;
    m["description"]  = p.description;
    m["type"]         = p.type;
    m["category"]     = p.category;
    m["author"]       = p.author;
    m["license"]      = p.license;
    m["icon"]         = p.icon;
    m["manifestVersion"] = p.manifestVersion;
    m["view"]         = p.view;

    LogosList deps = LogosList::array();
    for (const auto& d : p.dependencies) deps.push_back(d);
    m["dependencies"] = deps;

    // A SEPARATE key rather than a widened `dependencies`, which stays an
    // array of plain name STRINGS: basecamp's PluginLoader reads that list as
    // `dep.toString()` and skips empties, so an object-form entry would
    // stringify to empty and the dependency would silently never load. Absent
    // when every entry is a bare name, so an unconstrained package crosses
    // byte-identically. Mirrors package_manager_json.cpp's
    // `dependencyConstraints`. These are pins on OTHERS; `signerDid` below is
    // what this package claims about itself.
    if (!p.dependencyConstraints.empty()) {
        LogosList constraints = LogosList::array();
        for (const auto& d : p.dependencyConstraints) {
            LogosMap c = LogosMap::object();
            c["name"] = d.name;
            if (d.version) c["version"] = *d.version;
            if (d.signer)  c["signer"]  = *d.signer;
            constraints.push_back(c);
        }
        m["dependencyConstraints"] = constraints;
    }

    // The DID the installed manifest.sig names, emitted only once that
    // signature verified under the key the DID itself carries. That is
    // self-consistency, not identity; only `requiredSigner` settles identity.
    // Absent when no usable signature is installed, so an unsigned or embedded
    // package crosses byte-identically; an empty string would be ambiguous.
    // Deliberately the same key name as in the installPluginFile /
    // inspectPackage / verifySignature responses, but those come from
    // lgx_verify_signature, which fills signer_did BEFORE checking, so they
    // need a companion signatureStatus key and this one does not.
    if (p.signerDid) m["signerDid"] = *p.signerDid;

    m["hashes"]       = toLogosMap(p.hashes);
    m["installType"]  = std::string(installTypeToString(p.installType));
    m["installDir"]   = p.installDir;
    m["mainFilePath"] = p.mainFilePath;
    return m;
}

LogosList toLogosList(const std::vector<InstalledPackage>& v)
{
    LogosList out = LogosList::array();
    for (const auto& p : v) out.push_back(toLogosMap(p));
    return out;
}

// Flat per-node projection — just the node's own fields, no `children`.
// Shared between the flat list APIs (resolveFlatDependencies /
// resolveFlatDependents) and the tree APIs (where each recursive step is
// "this node's fields plus its children").
LogosMap toFlatLogosMap(const DependencyTreeNode& n)
{
    LogosMap m = LogosMap::object();
    m["name"]   = n.name;
    m["status"] = std::string(dependencyStatusToString(n.status));
    // Every status except NotInstalled and Cycle resolved to a package that IS
    // installed, so it carries the fields Installed does — "needs ^2.0.0, have
    // 1.0.0" is only actionable with both numbers. A predicate, not an
    // `Installed || VersionMismatch` chain: such a chain silently starts
    // blanking the version on each status appended after it was written.
    if (nodeResolvedToAnInstalledPackage(n.status)) {
        m["version"]     = n.version;
        m["installType"] = std::string(installTypeToString(n.installType));
    } else {
        m["version"]     = "";
        m["installType"] = "";
    }
    // Emitted only when set, like the constraints below, so an unmarked tree
    // crosses byte-identically. A not_installed node carrying it is NOT a
    // broken install — a missing-dependency marker must skip it.
    if (n.optional) m["optional"] = true;
    // The constraint the parent edge declared; absent for an unconstrained
    // edge, so a bare-name tree crosses byte-identically to before.
    // `requiredSigner` is judged by verifying the installed signature under the
    // PIN's own key, not by comparing it to `signerDid`.
    if (n.requiredVersion) m["requiredVersion"] = *n.requiredVersion;
    if (n.requiredSigner)  m["requiredSigner"]  = *n.requiredSigner;
    // What the installed package's own signature says about itself; absent
    // when none is installed, which is what makes a `signer_unknown` row
    // legible without a second call. A `signer_mismatch` row carries both, and
    // the two differing is the normal shape of that row.
    if (n.signerDid)  m["signerDid"]  = *n.signerDid;
    return m;
}

LogosMap toFlatLogosMap(const DependentTreeNode& n)
{
    LogosMap m = LogosMap::object();
    m["name"]        = n.name;
    m["version"]     = n.version;
    m["type"]        = n.type;
    m["installType"] = std::string(installTypeToString(n.installType));
    m["installDir"]  = n.installDir;
    return m;
}

// Depth-clipped tree serialisation — root always emitted with its own
// fields; `maxDepth` bounds how far we recurse into `children`. Used by
// resolveDependencies/resolveDependents: maxDepth=1 for !recursive (root
// + direct children with empty children arrays), maxDepth=INT_MAX for
// the full tree.
template <typename Node>
LogosMap toLogosTreeMap(const Node& n, int maxDepth)
{
    LogosMap m = toFlatLogosMap(n);
    LogosList children = LogosList::array();
    if (maxDepth > 0) {
        for (const auto& c : n.children)
            children.push_back(toLogosTreeMap(c, maxDepth - 1));
    }
    m["children"] = children;
    return m;
}

template <typename Node>
LogosList toFlatLogosList(const std::vector<Node>& v)
{
    LogosList out = LogosList::array();
    for (const auto& n : v) out.push_back(toFlatLogosMap(n));
    return out;
}

} // namespace

PackageManagerImpl::PackageManagerImpl()
    : m_lib(nullptr)
{
    m_lib = new PackageManagerLib();
}

PackageManagerImpl::~PackageManagerImpl()
{
    // Signal any running worker thread to exit, then join it before tearing
    // down state it might still reference (m_pendingAction, read when the
    // worker emits its cancellation event via the typed event methods). The
    // lock is taken briefly to publish m_ackShutdown and bump m_ackGeneration
    // atomically; notify + join happen outside the lock so the worker can
    // re-acquire and exit its wait_for.
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_ackShutdown = true;
        ++m_ackGeneration;
    }
    m_ackCv.notify_all();
    if (m_ackThread.joinable()) m_ackThread.join();

    delete m_lib;
    m_lib = nullptr;
}

LogosMap PackageManagerImpl::installPlugin(const std::string& pluginPath, bool skipIfNotNewerVersion)
{
    // "This build installs nothing at run time" is a configuration a shell is
    // allowed to have, and it is the one answer the library cannot hold: an
    // empty install list there means "this host's own variant". So it is
    // refused here, before any bytes are read, and the reason names the BUILD
    // rather than the package -- a user who pressed an install control is
    // entitled to know which of the two said no.
    if (m_installableVariants && m_installableVariants->empty()) {
        LogosMap refusal;
        refusal["name"] = std::filesystem::path(pluginPath).stem().string();
        refusal["path"] = std::string();
        refusal["isCoreModule"] = false;
        refusal["error"] = std::string("this build installs no package at run time");
        refusal["signatureStatus"] = std::string("unsigned");
        return refusal;
    }

    std::string errorMsg;
    std::string installedPluginPath;
    bool isCoreModule = false;
    std::string result = m_lib->installPluginFile(
        pluginPath, errorMsg, skipIfNotNewerVersion,
        &installedPluginPath, &isCoreModule
    );

    // The library reports success by returning a non-empty install location.
    // installedPluginPath is a REPORTING detail, not the success signal: a
    // QML-only ui_qml package ("main": {}) has no backend library, so it used
    // to come back empty from a perfectly good install. Gating on it here had
    // two consequences — uiPluginFileInstalled never fired (so Basecamp only
    // discovered the plugin after a restart) and response["path"] was empty,
    // which logos-package-manager-ui reads as failure and renders as a red
    // RETRY. Patched libraries always fill installedPluginPath in; the `result`
    // fallback keeps this correct against an older one.
    bool success = !result.empty();
    const std::string reportedPath = installedPluginPath.empty() ? result : installedPluginPath;

    if (success) {
        if (isCoreModule) {
            corePluginFileInstalled(reportedPath);
        } else {
            uiPluginFileInstalled(reportedPath);
        }
    }

    // Get signature info for the response
    auto sigResult = m_lib->verifyPackageSignature(pluginPath);

    std::string stem = std::filesystem::path(pluginPath).stem().string();

    LogosMap response;
    response["name"] = stem;
    response["path"] = success ? reportedPath : std::string();
    response["isCoreModule"] = isCoreModule;
    if (!success) {
        response["error"] = errorMsg;
    }

    // Add signature info
    if (sigResult.is_signed) {
        bool valid = sigResult.signature_valid && sigResult.package_valid;
        response["signatureStatus"] = valid ? std::string("signed") : std::string("invalid");
        response["signerDid"] = sigResult.signer_did;
        if (!sigResult.signer_name.empty())
            response["signerName"] = sigResult.signer_name;
        if (!sigResult.signer_url.empty())
            response["signerUrl"] = sigResult.signer_url;
        if (!sigResult.trusted_as.empty())
            response["trustedAs"] = sigResult.trusted_as;
    } else if (!sigResult.error.empty()) {
        response["signatureStatus"] = std::string("error");
        response["signatureError"] = sigResult.error;
    } else {
        response["signatureStatus"] = std::string("unsigned");
    }

    return response;
}

LogosMap PackageManagerImpl::inspectPackage(const std::string& lgxPath)
{
    LogosMap result;

    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        result["error"] = std::string("Failed to load LGX package: ")
                        + (lgx_get_last_error() ? lgx_get_last_error() : "unknown");
        return result;
    }

    const char* rawName    = lgx_get_name(pkg);
    const char* rawVersion = lgx_get_version(pkg);
    const char* rawDesc    = lgx_get_description(pkg);
    const char* rawManifest = lgx_get_manifest_json(pkg);

    std::string pkgName    = rawName    ? rawName    : "";
    std::string pkgVersion = rawVersion ? rawVersion : "";

    result["name"]    = pkgName;
    result["version"] = pkgVersion;
    result["description"] = rawDesc ? std::string(rawDesc) : "";

    // Extract type, category, and root content hash from the embedded
    // manifest. The root hash (Merkle tree root over the package content,
    // `manifest.hashes.root`) is the same identifier PMU renders when
    // browsing the online catalog — surfacing it here lets the install
    // confirmation dialog show a stable per-release fingerprint.
    if (rawManifest) {
        try {
            auto doc = LogosMap::parse(rawManifest);
            result["type"]     = doc.value("type", "");
            result["category"] = doc.value("category", "");
            if (doc.contains("hashes") && doc["hashes"].is_object()) {
                result["rootHash"] = doc["hashes"].value("root", "");
            } else {
                result["rootHash"] = "";
            }
        } catch (...) {
            result["type"]     = "";
            result["category"] = "";
            result["rootHash"] = "";
        }
    }

    // Available platform variants.
    const char** variants = lgx_get_variants(pkg);
    LogosList variantList = LogosList::array();
    if (variants) {
        for (int i = 0; variants[i]; ++i)
            variantList.push_back(std::string(variants[i]));
        lgx_free_string_array(variants);
    }
    result["variants"] = variantList;

    lgx_free_package(pkg);

    // Signature verification — standalone, no install side effects.
    auto sig = m_lib->verifyPackageSignature(lgxPath);
    if (sig.is_signed) {
        bool valid = sig.signature_valid && sig.package_valid;
        result["signatureStatus"] = valid ? std::string("signed")
                                          : std::string("invalid");
        result["signerDid"]  = sig.signer_did;
        result["signerName"] = sig.signer_name;
    } else if (!sig.error.empty()) {
        result["signatureStatus"] = std::string("error");
    } else {
        result["signatureStatus"] = std::string("unsigned");
    }

    // Check if this package is already installed.
    bool isAlreadyInstalled = false;
    std::string installedVersion;
    std::string installedHash;
    std::vector<InstalledPackage> scan = m_lib->getInstalledPackages();
    for (const auto& entry : scan) {
        if (entry.name == pkgName) {
            isAlreadyInstalled = true;
            installedVersion = entry.version;
            // Passthrough from the installed manifest.json; same field PMU
            // reads in the online catalog (`manifest.hashes.root`).
            installedHash = entry.hashes.root;
            break;
        }
    }
    result["isAlreadyInstalled"] = isAlreadyInstalled;
    result["installedVersion"]   = installedVersion;
    result["installedHash"]      = installedHash;

    // If already installed, compute reverse dependents so the dialog can
    // show what would be affected by an upgrade.
    if (isAlreadyInstalled) {
        auto deps = installedDependentsNames(pkgName);
        LogosList depList = LogosList::array();
        for (const auto& d : deps) depList.push_back(d);
        result["installedDependents"] = depList;
    }

    return result;
}

LogosList PackageManagerImpl::getInstalledPackages()
{
    return toLogosList(m_lib->getInstalledPackages());
}

LogosList PackageManagerImpl::getInstalledModules()
{
    return toLogosList(m_lib->getInstalledModules());
}

LogosList PackageManagerImpl::getInstalledUiPlugins()
{
    return toLogosList(m_lib->getInstalledUiPlugins());
}

LogosMap PackageManagerImpl::uninstallPackage(const std::string& packageName)
{
    return doUninstall(packageName);
}

LogosMap PackageManagerImpl::doUninstall(const std::string& packageName)
{
    // Inspect the package before removal so we know whether to emit a core or UI event.
    std::vector<InstalledPackage> scan = m_lib->getInstalledPackages();
    std::string moduleType;
    for (const auto& entry : scan) {
        if (entry.name == packageName) {
            moduleType = entry.type;
            break;
        }
    }

    UninstallResult r = m_lib->uninstallPackage(packageName);

    LogosMap response;
    response["success"] = r.success;
    if (!r.success) {
        response["error"] = r.errorMsg;
    } else {
        LogosList removed = LogosList::array();
        for (const auto& f : r.removedFiles) removed.push_back(f);
        response["removedFiles"] = removed;

        if (moduleType == "core") {
            corePluginUninstalled(packageName);
        } else {
            uiPluginUninstalled(packageName);
        }
    }
    return response;
}

LogosMap PackageManagerImpl::resolveDependencies(const std::string& packageName, bool recursive)
{
    // Unknown roots surface as nullopt from the library; keep an empty
    // object on the wire so callers can `.contains(...)` without branching.
    auto tree = m_lib->resolveDependencies(packageName);
    if (!tree) return LogosMap::object();
    // maxDepth=1 clips to root + direct children (children with empty
    // `children` arrays); INT_MAX walks the full tree.
    return toLogosTreeMap(*tree, recursive ? std::numeric_limits<int>::max() : 1);
}

LogosMap PackageManagerImpl::resolveDependents(const std::string& packageName, bool recursive)
{
    // Same shape treatment as resolveDependencies — the library returns a
    // tree, we either clip it at depth 1 or walk the full reverse subtree.
    auto tree = m_lib->resolveDependents(packageName);
    if (!tree) return LogosMap::object();
    return toLogosTreeMap(*tree, recursive ? std::numeric_limits<int>::max() : 1);
}

LogosList PackageManagerImpl::resolveFlatDependencies(const std::string& packageName, bool recursive)
{
    // Flat list of per-node maps (no `children`). recursive=false emits
    // only the root's direct children; recursive=true emits every
    // descendant, BFS-ordered and deduped by name (via DependencyTreeNode::flatten()).
    auto tree = m_lib->resolveDependencies(packageName);
    if (!tree) return LogosList::array();
    return recursive ? toFlatLogosList(tree->flatten())
                     : toFlatLogosList(tree->children);
}

LogosList PackageManagerImpl::resolveFlatDependents(const std::string& packageName, bool recursive)
{
    auto tree = m_lib->resolveDependents(packageName);
    if (!tree) return LogosList::array();
    return recursive ? toFlatLogosList(tree->flatten())
                     : toFlatLogosList(tree->children);
}

std::vector<std::string> PackageManagerImpl::getValidVariants()
{
    return PackageManagerLib::platformVariantsToTry();
}

// ---------------------------------------------------------------------------
// Per-variant availability
// ---------------------------------------------------------------------------
//
// Two lists in, one verdict out. No disk, no package load, no platform
// branching -- which is what makes "a native-only entry has no install control"
// a unit test rather than a device run.

namespace {

// The flavour suffix a non-portable CONSUMER build appends to its own variant
// name. It says nothing about the package, so it comes off both sides before
// anything is compared.
const char kDevSuffix[] = "-dev";

std::string withoutDevSuffix(const std::string& v)
{
    const size_t n = sizeof(kDevSuffix) - 1;
    return (v.size() > n && v.compare(v.size() - n, n, kDevSuffix) == 0)
         ? v.substr(0, v.size() - n)
         : v;
}

// Every live spelling of `variant`'s ARCHITECTURE half, from logos-package's
// vocabulary. The two catalog producers in this ecosystem disagree about how to
// spell one (darwin-amd64 vs darwin-x86_64) while one CONSUMES the other, so a
// consumer that compared verbatim would call an installable entry unavailable.
// The OS half is never aliased -- that is what keeps a Windows package from
// resolving as a macOS one.
std::vector<std::string> variantSpellings(const std::string& variant)
{
    std::vector<std::string> out;
    const char** s = lgx_variant_spellings(variant.c_str());
    if (!s) return { variant };
    for (int i = 0; s[i]; ++i) out.emplace_back(s[i]);
    lgx_free_string_array(s);
    if (out.empty()) out.push_back(variant);
    return out;
}

// What a user calls the platform a variant names. Unknown names are returned
// verbatim: a private target this vocabulary has never heard of is still
// somewhere the package runs, and dropping it would UNDER-report availability.
//
// "ios-sim" is tested before "ios" and is deliberately a different answer.
// Telling someone their package "runs on iOS" when only a simulator build
// exists sends them to look for it on a phone.
std::string platformLabel(const std::string& variant)
{
    struct Row { const char* prefix; const char* label; };
    static const Row kRows[] = {
        { "linux-",   "Linux" },
        { "darwin-",  "macOS" },
        { "windows-", "Windows" },
        { "android-", "Android" },
        { "ios-sim-", "the iOS simulator" },
        { "ios-",     "iOS" },
    };
    if (variant == "web") return "the Web container";
    for (const Row& r : kRows) {
        const size_t n = std::strlen(r.prefix);
        if (variant.size() > n && variant.compare(0, n, r.prefix) == 0) return r.label;
    }
    return variant;
}

// "A", "A and B", "A, B and C".
std::string joinLabels(const std::vector<std::string>& labels)
{
    std::string out;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) out += (i + 1 == labels.size()) ? " and " : ", ";
        out += labels[i];
    }
    return out;
}

}  // namespace

void PackageManagerImpl::setInstallableVariants(const std::vector<std::string>& variants)
{
    m_installableVariants = variants;
    // AND THE LIBRARY, because otherwise the annotation and the install would
    // answer differently: a row would read "installable here as the 'web'
    // variant" and installPluginFile would go looking for this host's NATIVE
    // one and refuse the package it had just advertised. One declaration, both
    // halves -- which is the whole reason availability is answered by this
    // module rather than by the shell.
    //
    // An explicitly EMPTY declaration is NOT forwarded: the library reads an
    // empty install list as "this host's own", which is the right default for
    // every caller that never declared anything and the opposite of what an
    // empty declaration here means. That case is refused in installPlugin.
    if (!variants.empty())
        m_lib->setInstallVariants(variants);
}

std::vector<std::string> PackageManagerImpl::getInstallableVariants()
{
    // An explicitly EMPTY declaration must not fall back to the loader's native
    // variant: "this build installs nothing at runtime" is a configuration a
    // shell is allowed to have, and silently granting it the desktop's answer is
    // how a phone grows an install button it cannot honour.
    if (m_installableVariants) return *m_installableVariants;
    return getValidVariants();
}

LogosMap PackageManagerImpl::variantAvailability(const std::string& variantsJson)
{
    LogosMap out;
    out["available"]   = false;
    out["variant"]     = "";
    out["availableOn"] = LogosList::array();

    const LogosMap parsed =
        LogosMap::parse(variantsJson, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        // Distinct from an empty list. "Ships nothing" and "we could not tell"
        // are different facts, and the second must not render as the first.
        out["reason"] = "the package's variant list could not be read";
        return out;
    }

    std::vector<std::string> shipped;
    for (const auto& v : parsed)
        if (v.is_string()) shipped.push_back(v.get<std::string>());

    if (shipped.empty()) {
        out["reason"] = "this package declares no variant, so there is nowhere it runs";
        return out;
    }

    // Preference order is the HOST's declaration order, not the package's: a
    // shell that lists "web" before its native variant means "prefer the
    // container", and iterating the package first would silently invert that.
    for (const std::string& accepted : getInstallableVariants()) {
        for (const std::string& spelling : variantSpellings(withoutDevSuffix(accepted))) {
            const auto hit = std::find_if(shipped.begin(), shipped.end(),
                [&spelling](const std::string& candidate) {
                    return withoutDevSuffix(candidate) == spelling;
                });
            if (hit == shipped.end()) continue;
            out["available"] = true;
            out["variant"]   = *hit;
            out["reason"]    = "installable here as the '" + *hit + "' variant";
            return out;
        }
    }

    LogosList elsewhere = LogosList::array();
    std::vector<std::string> labels;
    for (const std::string& v : shipped) {
        elsewhere.push_back(v);
        const std::string label = platformLabel(v);
        // Two Windows architectures are one sentence about Windows.
        if (std::find(labels.begin(), labels.end(), label) == labels.end())
            labels.push_back(label);
    }
    out["availableOn"] = elsewhere;
    out["reason"] = "available on " + joinLabels(labels) + ", not in this build";
    return out;
}

LogosList PackageManagerImpl::catalogAvailability(const std::string& catalogJson)
{
    const LogosMap parsed =
        LogosMap::parse(catalogJson, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) return LogosList::array();

    LogosList out = LogosList::array();
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;
        // ANNOTATES: the entry passes through whole. An entry that lost its
        // report link on the way through would take the report affordance with
        // it, and a `variants` key dropped here would make a second pass lie.
        LogosMap annotated = entry;
        const std::string variants = entry.contains("variants") && entry["variants"].is_array()
                                   ? entry["variants"].dump()
                                   : std::string("not-an-array");
        annotated["availability"] = variantAvailability(variants);
        out.push_back(annotated);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The signer-trust prompt
// ---------------------------------------------------------------------------

LogosMap PackageManagerImpl::signerTrust(const std::string& lgxPath)
{
    LogosMap out;
    out["name"] = "";
    out["version"] = "";
    out["signatureStatus"] = "error";
    out["signerName"] = "";
    out["signerDid"] = "";
    out["signerUrl"] = "";
    out["trusted"] = false;
    out["trustedAs"] = "";
    out["policy"] = m_signaturePolicy;
    out["installable"] = false;

    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        const char* err = lgx_get_last_error();
        out["error"]  = std::string("Failed to load LGX package: ")
                      + (err ? err : "unknown");
        out["reason"] = "this file could not be read as a package";
        return out;
    }
    const char* rawName    = lgx_get_name(pkg);
    const char* rawVersion = lgx_get_version(pkg);
    out["name"]    = rawName    ? std::string(rawName)    : std::string();
    out["version"] = rawVersion ? std::string(rawVersion) : std::string();
    lgx_free_package(pkg);

    const auto sig = m_lib->verifyPackageSignature(lgxPath);
    const bool valid = sig.signature_valid && sig.package_valid;
    if (sig.is_signed) {
        out["signatureStatus"] = valid ? std::string("signed") : std::string("invalid");
        out["signerName"] = sig.signer_name;
        out["signerDid"]  = sig.signer_did;
        out["signerUrl"]  = sig.signer_url;
        out["trustedAs"]  = sig.trusted_as;
        // TRUSTED means the local keyring vouches for the DID *and* the
        // signature actually verified. A keyring hit over bytes that do not
        // match what was signed is not a trusted package, and reporting it as
        // one would put a known publisher's name on somebody else's code.
        out["trusted"] = valid && !sig.trusted_as.empty();
    } else if (!sig.error.empty()) {
        out["signatureStatus"] = "error";
        out["error"] = sig.error;
    } else {
        out["signatureStatus"] = "unsigned";
    }

    // The verdict. Mirrors PackageManagerLib::installPluginFile's gate exactly,
    // in the same order -- this method's only reason to exist is that the prompt
    // and the installer must not be able to disagree.
    if (m_signaturePolicy == "none") {
        out["installable"] = true;
        out["reason"] = "this build does not check signatures";
    } else if (sig.is_signed && !sig.signature_valid) {
        out["reason"] = "the signature on this package does not verify";
    } else if (!sig.package_valid) {
        out["reason"] = "this package's contents are not what was signed";
    } else if (!sig.is_signed && m_signaturePolicy == "require") {
        out["reason"] = "this package is unsigned and this build requires a signature";
    } else if (sig.is_signed && sig.trusted_as.empty() && m_signaturePolicy == "require") {
        out["reason"] = "signed by a key your keyring does not vouch for";
    } else {
        out["installable"] = true;
        out["reason"] = sig.is_signed
            ? (sig.trusted_as.empty()
                   ? "signed, by a publisher you have not marked as trusted"
                   : "signed by '" + sig.trusted_as + "', a publisher you trust")
            : "unsigned; this build installs it with a warning";
    }
    return out;
}

void PackageManagerImpl::setEmbeddedModulesDirectory(const std::string& dir)
{
    m_lib->setEmbeddedModulesDirectory(dir);
}

void PackageManagerImpl::addEmbeddedModulesDirectory(const std::string& dir)
{
    m_lib->addEmbeddedModulesDirectory(dir);
}

void PackageManagerImpl::setEmbeddedUiPluginsDirectory(const std::string& dir)
{
    m_lib->setEmbeddedUiPluginsDirectory(dir);
}

void PackageManagerImpl::addEmbeddedUiPluginsDirectory(const std::string& dir)
{
    m_lib->addEmbeddedUiPluginsDirectory(dir);
}

void PackageManagerImpl::setUserModulesDirectory(const std::string& dir)
{
    m_lib->setUserModulesDirectory(dir);
}

void PackageManagerImpl::setUserUiPluginsDirectory(const std::string& dir)
{
    m_lib->setUserUiPluginsDirectory(dir);
}

void PackageManagerImpl::setSignaturePolicy(const std::string& policy)
{
    std::string p = policy;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    static const std::map<std::string, SignaturePolicy> kPolicies{
        {"none",    SignaturePolicy::NONE},
        {"warn",    SignaturePolicy::WARN},
        {"require", SignaturePolicy::REQUIRE},
    };
    const auto known = kPolicies.find(p);
    if (known == kPolicies.end()) {
        std::cerr << "PackageManagerImpl::setSignaturePolicy: invalid policy '"
                  << policy << "' - expected one of: none, warn, require\n";
        return;
    }

    m_lib->setSignaturePolicy(known->second);
    // Mirrored into m_signaturePolicy as well as pushed into the lib, because
    // signerTrust() has to reproduce the installer's decision and the lib's own
    // getter is absent from the unit tests' stub header.
    m_signaturePolicy = p;
}

void PackageManagerImpl::setKeyringDirectory(const std::string& dir)
{
    m_lib->setKeyringDirectory(dir);
}

LogosMap PackageManagerImpl::verifyPackage(const std::string& lgxPath)
{
    auto result = m_lib->verifyPackageSignature(lgxPath);

    LogosMap response;
    response["isSigned"] = result.is_signed;
    response["signatureValid"] = result.signature_valid;
    response["packageValid"] = result.package_valid;
    response["signerDid"] = result.signer_did;
    response["signerName"] = result.signer_name;
    response["signerUrl"] = result.signer_url;
    response["trustedAs"] = result.trusted_as;
    if (!result.error.empty())
        response["error"] = result.error;
    return response;
}

LogosMap PackageManagerImpl::addTrustedKey(const std::string& name, const std::string& did,
                                            const std::string& displayName, const std::string& url)
{
    std::string keyringDir = m_lib->keyringDirectory();
    const char* keyringDirPtr = keyringDir.empty() ? nullptr : keyringDir.c_str();

    lgx_result_t res = lgx_keyring_add(
        keyringDirPtr,
        name.c_str(),
        did.c_str(),
        displayName.empty() ? nullptr : displayName.c_str(),
        url.empty() ? nullptr : url.c_str()
    );

    LogosMap response;
    response["success"] = static_cast<bool>(res.success);
    if (!res.success && res.error)
        response["error"] = std::string(res.error);
    return response;
}

LogosMap PackageManagerImpl::removeTrustedKey(const std::string& name)
{
    std::string keyringDir = m_lib->keyringDirectory();
    const char* keyringDirPtr = keyringDir.empty() ? nullptr : keyringDir.c_str();

    lgx_result_t res = lgx_keyring_remove(
        keyringDirPtr,
        name.c_str()
    );

    LogosMap response;
    response["success"] = static_cast<bool>(res.success);
    if (!res.success && res.error)
        response["error"] = std::string(res.error);
    return response;
}

LogosList PackageManagerImpl::listTrustedKeys()
{
    std::string keyringDir = m_lib->keyringDirectory();
    const char* keyringDirPtr = keyringDir.empty() ? nullptr : keyringDir.c_str();

    lgx_keyring_list_t list = lgx_keyring_list(keyringDirPtr);

    LogosList result = LogosList::array();
    for (size_t i = 0; i < list.count; ++i) {
        LogosMap entry;
        if (list.keys[i].name)         entry["name"]        = std::string(list.keys[i].name);
        if (list.keys[i].did)          entry["did"]         = std::string(list.keys[i].did);
        if (list.keys[i].display_name) entry["displayName"] = std::string(list.keys[i].display_name);
        if (list.keys[i].url)          entry["url"]         = std::string(list.keys[i].url);
        if (list.keys[i].added_at)     entry["addedAt"]     = std::string(list.keys[i].added_at);
        result.push_back(entry);
    }

    lgx_free_keyring_list(list);
    return result;
}

// ---------------------------------------------------------------------------
// Gated uninstall / upgrade flow
// ---------------------------------------------------------------------------

const char* PackageManagerImpl::opName(PendingOp op)
{
    switch (op) {
        case PendingOp::Uninstall:      return "uninstall";
        case PendingOp::Upgrade:        return "upgrade";
        case PendingOp::Install:        return "install";
        case PendingOp::MultiUninstall: return "multi-uninstall";
        case PendingOp::None:           return "none";
    }
    return "none";
}

// Human-readable description of the pending action for cross-op blocking
// error messages. Single-name ops include the package name; multi includes
// the batch size (showing names[0] alone would be misleading for a batch).
// Caller must hold m_stateMutex.
std::string PackageManagerImpl::pendingDescriptionLocked() const
{
    std::string desc = std::string("Another ") + opName(m_pendingAction.op);
    if (m_pendingAction.op == PendingOp::MultiUninstall) {
        desc += " is in progress (batch of "
              + std::to_string(m_pendingAction.names.size()) + " packages)";
    } else {
        desc += " is in progress for '" + m_pendingAction.name + "'";
    }
    return desc;
}

bool PackageManagerImpl::isEmbedded(const std::string& packageName) const
{
    std::vector<InstalledPackage> scan = m_lib->getInstalledPackages();
    for (const auto& entry : scan) {
        if (entry.name == packageName)
            return entry.installType == InstallType::Embedded;
    }
    return false;
}

std::vector<std::string> PackageManagerImpl::installedDependentsNames(const std::string& packageName) const
{
    std::vector<std::string> names;
    auto tree = m_lib->resolveDependents(packageName);
    if (!tree) return names;
    auto flat = tree->flatten();
    names.reserve(flat.size());
    for (const auto& d : flat) {
        if (!d.name.empty()) names.push_back(d.name);
    }
    return names;
}

// ---------------------------------------------------------------------------
// Pure-C++ ack timer — std::thread + std::condition_variable replacing QTimer.
// See detailed protocol comment in the header.
// ---------------------------------------------------------------------------

void PackageManagerImpl::startAckTimerLocked(std::unique_lock<std::mutex>& lock)
{
    // Precondition: caller holds m_stateMutex via `lock`.

    // Bump the generation and wake any previously-running worker. If one is
    // still waiting on the CV, it'll re-acquire the mutex, see its captured
    // generation is stale, and bail.
    ++m_ackGeneration;
    m_ackCv.notify_all();

    // Join the previous worker (if any) before replacing m_ackThread —
    // assigning to a joinable std::thread is undefined behaviour. Release
    // the lock during join so the worker can proceed past its wait_for;
    // otherwise we'd deadlock (we hold the lock the worker needs).
    if (m_ackThread.joinable()) {
        lock.unlock();
        m_ackThread.join();
        lock.lock();
    }

    const uint64_t gen = m_ackGeneration;
    m_ackThread = std::thread([this, gen]() { ackTimerWorker(gen); });
}

void PackageManagerImpl::stopAckTimerLocked()
{
    // Caller holds m_stateMutex. Bump the generation and notify so any
    // running worker wakes up and exits silently. Do NOT join here: the
    // slot calling us is likely running on the module thread and the
    // worker might be mid-wait needing the mutex we hold. The worker
    // will exit on its own; the next startAckTimerLocked (or the
    // destructor) reaps the std::thread handle.
    ++m_ackGeneration;
    m_ackCv.notify_all();
}

void PackageManagerImpl::ackTimerWorker(uint64_t myGeneration)
{
    std::unique_lock<std::mutex> lock(m_stateMutex);

    // wait_for returns true when the predicate is satisfied, false on
    // timeout. Predicate: "stop waiting" — either the process is shutting
    // down or our generation is stale (a newer request / ack / cancel
    // has superseded us).
    bool cancelled = m_ackCv.wait_for(
        lock,
        std::chrono::milliseconds(m_ackTimeoutMs),
        [this, myGeneration]() {
            return m_ackShutdown || m_ackGeneration != myGeneration;
        }
    );
    if (cancelled) return;

    // Full timeout with no cancellation — but recheck state now that we
    // hold the lock. An ack or a different state change could have
    // landed between the last CV check and here (unlikely, but cheap to
    // verify).
    if (m_ackShutdown) return;
    if (m_ackGeneration != myGeneration) return;
    if (m_pendingAction.op == PendingOp::None || m_pendingAction.acked) return;

    // Claim the pending action so slot-side code sees a clean slate.
    PendingAction pa = m_pendingAction;
    m_pendingAction = {};

    const std::string reason = "no listener acknowledged within "
                             + std::to_string(m_ackTimeoutMs) + "ms";

    // Release the lock before emitting — event emission marshals through a
    // Qt signal; a listener synchronously calling back into this impl
    // (e.g. a headless runtime that calls uninstallPackage on cancel
    // notification) would otherwise re-enter the mutex and deadlock.
    lock.unlock();
    emitCancellation(pa, reason);
}

void PackageManagerImpl::emitCancellation(const PendingAction& pa, const std::string& reason)
{
    LogosMap payload;
    payload["reason"] = reason;
    if (pa.op == PendingOp::Upgrade) {
        payload["name"] = pa.name;
        payload["releaseTag"] = pa.releaseTag;
        upgradeCancelled(payload.dump());
    } else if (pa.op == PendingOp::Uninstall) {
        payload["name"] = pa.name;
        uninstallCancelled(payload.dump());
    } else if (pa.op == PendingOp::Install) {
        payload["name"] = pa.name;
        payload["releaseTag"] = pa.releaseTag;
        payload["repositoryUrl"] = pa.repositoryUrl;
        installCancelled(payload.dump());
    } else if (pa.op == PendingOp::MultiUninstall) {
        LogosList names = LogosList::array();
        for (const auto& n : pa.names) names.push_back(n);
        payload["names"] = names;
        multiUninstallCancelled(payload.dump());
    }
}

LogosMap PackageManagerImpl::requestUninstall(const std::string& packageName)
{
    LogosMap response;
    // Empty packageName would be persisted into m_pendingAction.name and then
    // broadcast via beforeUninstall(payload{name:""}), causing listeners to
    // open a dialog titled "Uninstall ''?" with no dependents. Reject early
    // with a distinct error so callers can surface a sane toast and callers
    // that ARE the GUI can avoid showing a stray dialog.
    if (packageName.empty()) {
        response["success"] = false;
        response["error"] = "Package name cannot be empty";
        return response;
    }

    std::unique_lock<std::mutex> lock(m_stateMutex);

    if (m_pendingAction.op != PendingOp::None) {
        response["success"] = false;
        response["error"] = pendingDescriptionLocked();
        return response;
    }

    if (isEmbedded(packageName)) {
        response["success"] = false;
        response["error"] = "Cannot uninstall embedded module '" + packageName + "'";
        return response;
    }

    m_pendingAction = {};
    m_pendingAction.op = PendingOp::Uninstall;
    m_pendingAction.name = packageName;
    m_pendingAction.acked = false;

    // Build the event payload while we still hold the lock (so m_lib reads
    // don't race against a concurrent slot). The event emission itself is
    // deferred until after the unlock — see the reentrancy note in ackTimerWorker.
    LogosMap payload;
    payload["name"] = packageName;
    LogosList deps = LogosList::array();
    for (const auto& d : installedDependentsNames(packageName))
        deps.push_back(d);
    payload["installedDependents"] = deps;

    // Start the ack timer (this may briefly release + re-acquire `lock`
    // while joining a previous worker).
    startAckTimerLocked(lock);

    lock.unlock();
    beforeUninstall(payload.dump());

    response["success"] = true;
    return response;
}

// Parse the initiator-supplied depChanges JSON (array of change records) and
// attach it to a gated-flow event payload under "depChanges". The module never
// interprets it — it's opaque display data for the host's confirmation dialog —
// so a malformed / empty string simply yields an empty array rather than an
// error (the dialog then shows "no other packages need to change").
static void attachDepChanges(LogosMap& payload, const std::string& depChanges)
{
    LogosList changes = LogosList::array();
    if (!depChanges.empty()) {
        try {
            LogosMap parsed = LogosMap::parse(depChanges);
            if (parsed.is_array())
                changes = std::move(parsed);
        } catch (...) {
            // leave `changes` empty on any parse failure
        }
    }
    payload["depChanges"] = changes;
}

LogosMap PackageManagerImpl::requestUpgrade(const std::string& packageName,
                                             const std::string& releaseTag,
                                             int64_t mode,
                                             const std::string& depChanges)
{
    LogosMap response;
    // Same rationale as requestUninstall: empty name has to be rejected
    // before we set pending state, otherwise beforeUpgrade(name="") leads
    // listeners into an empty-title dialog.
    if (packageName.empty()) {
        response["success"] = false;
        response["error"] = "Package name cannot be empty";
        return response;
    }

    std::unique_lock<std::mutex> lock(m_stateMutex);

    if (m_pendingAction.op != PendingOp::None) {
        response["success"] = false;
        response["error"] = pendingDescriptionLocked();
        return response;
    }

    if (isEmbedded(packageName)) {
        response["success"] = false;
        response["error"] = "Cannot upgrade embedded module '" + packageName + "'";
        return response;
    }

    m_pendingAction = {};
    m_pendingAction.op = PendingOp::Upgrade;
    m_pendingAction.name = packageName;
    m_pendingAction.releaseTag = releaseTag;
    m_pendingAction.mode = mode;
    m_pendingAction.acked = false;

    LogosMap payload;
    payload["name"] = packageName;
    payload["releaseTag"] = releaseTag;
    payload["mode"] = mode;
    LogosList deps = LogosList::array();
    for (const auto& d : installedDependentsNames(packageName))
        deps.push_back(d);
    payload["installedDependents"] = deps;
    attachDepChanges(payload, depChanges);

    startAckTimerLocked(lock);

    lock.unlock();
    beforeUpgrade(payload.dump());

    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::requestInstall(const std::string& packageName,
                                             const std::string& releaseTag,
                                             const std::string& repositoryUrl,
                                             const std::string& depChanges)
{
    LogosMap response;
    if (packageName.empty()) {
        response["success"] = false;
        response["error"] = "Package name cannot be empty";
        return response;
    }

    std::unique_lock<std::mutex> lock(m_stateMutex);

    if (m_pendingAction.op != PendingOp::None) {
        response["success"] = false;
        response["error"] = pendingDescriptionLocked();
        return response;
    }

    m_pendingAction = {};
    m_pendingAction.op = PendingOp::Install;
    m_pendingAction.name = packageName;
    m_pendingAction.releaseTag = releaseTag;
    m_pendingAction.repositoryUrl = repositoryUrl;
    m_pendingAction.acked = false;

    LogosMap payload;
    payload["name"] = packageName;
    payload["releaseTag"] = releaseTag;
    payload["repositoryUrl"] = repositoryUrl;
    attachDepChanges(payload, depChanges);

    startAckTimerLocked(lock);

    lock.unlock();
    beforeInstall(payload.dump());

    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::ackPendingAction(const std::string& packageName)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    LogosMap response;

    bool match = false;
    if (m_pendingAction.op == PendingOp::MultiUninstall) {
        match = std::find(m_pendingAction.names.begin(),
                          m_pendingAction.names.end(),
                          packageName) != m_pendingAction.names.end();
    } else if (m_pendingAction.op != PendingOp::None) {
        match = (m_pendingAction.name == packageName);
    }

    if (!match) {
        response["success"] = false;
        response["error"] = "No matching pending action to ack for '" + packageName + "'";
        return response;
    }
    // Idempotent — re-acking an already-acked request is a no-op.
    m_pendingAction.acked = true;
    stopAckTimerLocked();
    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::confirmUninstall(const std::string& packageName)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Uninstall || m_pendingAction.name != packageName) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending uninstall for '" + packageName + "'";
            return response;
        }
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending uninstall for '" + packageName + "' has not been acknowledged";
            return response;
        }
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    // Lock released before doUninstall — it emits corePluginUninstalled /
    // uiPluginUninstalled, and listeners may synchronously call back into
    // this impl (the whole point of the event is to trigger cleanup).
    return doUninstall(packageName);
}

LogosMap PackageManagerImpl::cancelUninstall(const std::string& packageName)
{
    PendingAction pa;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Uninstall || m_pendingAction.name != packageName) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending uninstall for '" + packageName + "'";
            return response;
        }
        // Symmetric with confirmUninstall: the gated protocol requires the
        // owning listener to ack before driving the decision either way.
        // An un-acked pending state is owned by the ack-reception timer;
        // letting cancel short-circuit it would bypass the protocol and
        // suppress the "no listener acknowledged" timeout event that
        // initiators otherwise rely on.
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending uninstall for '" + packageName + "' has not been acknowledged";
            return response;
        }
        pa = m_pendingAction;
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    // Uniform cancellation notification — same event the ack-timeout path emits.
    // Initiators (PMU) subscribe once and handle every cancellation consistently.
    emitCancellation(pa, "user cancelled");
    LogosMap response;
    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::confirmUpgrade(const std::string& packageName,
                                             const std::string& releaseTag)
{
    int64_t mode = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Upgrade
            || m_pendingAction.name != packageName
            || m_pendingAction.releaseTag != releaseTag) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending upgrade for '" + packageName + "'";
            return response;
        }
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending upgrade for '" + packageName + "' has not been acknowledged";
            return response;
        }
        mode = m_pendingAction.mode;
        m_pendingAction = {};
        stopAckTimerLocked();
    }

    LogosMap uninstallResult = doUninstall(packageName);

    // On successful uninstall, tell PMU to drive the download+install step
    // for the new version. The impl layer has no LogosAPI access (it only
    // communicates outward via the typed events), so we can't call
    // package_downloader directly. Instead we emit upgradeUninstallDone
    // with the pinned releaseTag — PMU subscribes to this event and reuses
    // its existing download+install chain (downloadPackageAsync →
    // installOnePackage). The user sees the row flip to "Installing" while
    // the download runs, then to "Installed" (or "Failed") when it finishes.
    bool ok = uninstallResult.value("success", false);
    if (ok) {
        LogosMap payload;
        payload["name"] = packageName;
        payload["releaseTag"] = releaseTag;
        payload["mode"] = mode;
        upgradeUninstallDone(payload.dump());
    }

    return uninstallResult;
}

LogosMap PackageManagerImpl::cancelUpgrade(const std::string& packageName,
                                            const std::string& releaseTag)
{
    PendingAction pa;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Upgrade
            || m_pendingAction.name != packageName
            || m_pendingAction.releaseTag != releaseTag) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending upgrade for '" + packageName + "'";
            return response;
        }
        // See cancelUninstall for why cancel also requires prior ack.
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending upgrade for '" + packageName + "' has not been acknowledged";
            return response;
        }
        pa = m_pendingAction;
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    emitCancellation(pa, "user cancelled");
    LogosMap response;
    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::resetPendingAction()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_pendingAction = {};
    stopAckTimerLocked();
    LogosMap response;
    response["success"] = true;
    return response;
}

// ---------------------------------------------------------------------------
// Multi-package gated uninstall
// ---------------------------------------------------------------------------
//
// Same protocol as requestUninstall — single pending slot, single ack, single
// confirm/cancel — extended to gate a batch of N packages. The destructive
// loop in confirmMultiUninstall calls doUninstall(name) per package, which
// emits per-package corePluginUninstalled / uiPluginUninstalled as today

namespace {

// Dedupe while preserving first-occurrence order. Used at the boundary of
// every multi-uninstall entry point so duplicate names in the caller's list
// can never cause a double-uninstall or a confirm/cancel mismatch.
static std::vector<std::string> dedupeNamesPreserveOrder(
    const std::vector<std::string>& in)
{
    std::vector<std::string> out;
    out.reserve(in.size());
    std::set<std::string> seen;
    for (const auto& n : in) {
        if (seen.insert(n).second) out.push_back(n);
    }
    return out;
}

} // namespace

LogosMap PackageManagerImpl::confirmInstall(const std::string& packageName)
{
    // A fresh install removes nothing first — unlike confirmUpgrade there is no
    // doUninstall step. Validate, capture the echo fields, and clear the gate in
    // ONE critical section so a concurrent cancel / reset / ack-timeout can't swap
    // the pending action out between the check and the capture (which would make
    // installApproved carry an empty/wrong payload). Emit outside the lock —
    // listeners may call back in synchronously.
    LogosMap payload;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Install || m_pendingAction.name != packageName) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending install for '" + packageName + "'";
            return response;
        }
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending install for '" + packageName + "' has not been acknowledged";
            return response;
        }
        payload["name"] = m_pendingAction.name;
        payload["releaseTag"] = m_pendingAction.releaseTag;
        payload["repositoryUrl"] = m_pendingAction.repositoryUrl;
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    installApproved(payload.dump());

    LogosMap response;
    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::cancelInstall(const std::string& packageName)
{
    PendingAction pa;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::Install || m_pendingAction.name != packageName) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending install for '" + packageName + "'";
            return response;
        }
        // See cancelUninstall for why cancel also requires prior ack.
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending install for '" + packageName + "' has not been acknowledged";
            return response;
        }
        pa = m_pendingAction;
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    emitCancellation(pa, "user cancelled");
    LogosMap response;
    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::requestMultiUninstall(const std::vector<std::string>& packageNamesIn)
{
    LogosMap response;

    if (packageNamesIn.empty()) {
        response["success"] = false;
        response["error"] = "Package list cannot be empty";
        return response;
    }

    for (const auto& n : packageNamesIn) {
        if (n.empty()) {
            response["success"] = false;
            response["error"] = "Package names cannot be empty";
            return response;
        }
    }

    // Dedupe immediately so every downstream check (embedded scan, dependents
    // union, m_pendingAction.names storage, beforeMultiUninstall payload)
    // operates on the canonical set.
    const std::vector<std::string> packageNames =
        dedupeNamesPreserveOrder(packageNamesIn);

    std::unique_lock<std::mutex> lock(m_stateMutex);

    if (m_pendingAction.op != PendingOp::None) {
        response["success"] = false;
        response["error"] = pendingDescriptionLocked();
        return response;
    }

    std::vector<std::string> embedded;
    for (const auto& n : packageNames) {
        if (isEmbedded(n)) embedded.push_back(n);
    }
    if (!embedded.empty()) {
        std::string msg = "Cannot uninstall embedded modules:";
        for (const auto& n : embedded) msg += " '" + n + "'";
        response["success"] = false;
        response["error"] = msg;
        return response;
    }

    m_pendingAction = {};
    m_pendingAction.op = PendingOp::MultiUninstall;
    m_pendingAction.names = packageNames;
    m_pendingAction.acked = false;
    // m_pendingAction.name intentionally left empty — ack matches against
    // m_pendingAction.names directly; the cross-op blocking error message
    // uses pendingDescriptionLocked() which handles the multi case.

    std::set<std::string> batchSet(packageNames.begin(), packageNames.end());
    std::vector<std::string> dedupedDeps;
    std::set<std::string> seen;
    for (const auto& n : packageNames) {
        for (const auto& d : installedDependentsNames(n)) {
            if (batchSet.count(d)) continue;
            if (seen.insert(d).second) dedupedDeps.push_back(d);
        }
    }

    LogosMap payload;
    LogosList namesArr = LogosList::array();
    for (const auto& n : packageNames) namesArr.push_back(n);
    payload["names"] = namesArr;
    LogosList depsArr = LogosList::array();
    for (const auto& d : dedupedDeps) depsArr.push_back(d);
    payload["installedDependents"] = depsArr;

    startAckTimerLocked(lock);

    lock.unlock();
    beforeMultiUninstall(payload.dump());

    response["success"] = true;
    return response;
}

LogosMap PackageManagerImpl::confirmMultiUninstall(const std::vector<std::string>& packageNamesIn)
{
    // Dedupe so callers can pass either the original or deduped form — the
    // pending state always holds the deduped list (see requestMultiUninstall).
    const std::vector<std::string> packageNames =
        dedupeNamesPreserveOrder(packageNamesIn);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::MultiUninstall || m_pendingAction.names != packageNames) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending multi-uninstall";
            return response;
        }
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending multi-uninstall has not been acknowledged";
            return response;
        }
        m_pendingAction = {};
        stopAckTimerLocked();
    }

    LogosList results = LogosList::array();
    bool allOk = true;
    for (const auto& n : packageNames) {
        LogosMap one = doUninstall(n);
        bool ok = one.value("success", false);
        if (!ok) allOk = false;
        LogosMap entry;
        entry["name"] = n;
        entry["success"] = ok;
        if (one.contains("error"))        entry["error"] = one["error"];
        if (one.contains("removedFiles")) entry["removedFiles"] = one["removedFiles"];
        results.push_back(entry);
    }

    LogosMap response;
    response["success"] = allOk;
    response["results"] = results;
    return response;
}

LogosMap PackageManagerImpl::cancelMultiUninstall(const std::vector<std::string>& packageNamesIn)
{
    // Dedupe so callers can pass either the original or deduped form — see
    // confirmMultiUninstall for rationale.
    const std::vector<std::string> packageNames =
        dedupeNamesPreserveOrder(packageNamesIn);

    PendingAction pa;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_pendingAction.op != PendingOp::MultiUninstall || m_pendingAction.names != packageNames) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "No matching pending multi-uninstall";
            return response;
        }
        if (!m_pendingAction.acked) {
            LogosMap response;
            response["success"] = false;
            response["error"] = "Pending multi-uninstall has not been acknowledged";
            return response;
        }
        pa = m_pendingAction;
        m_pendingAction = {};
        stopAckTimerLocked();
    }
    emitCancellation(pa, "user cancelled");
    LogosMap response;
    response["success"] = true;
    return response;
}
