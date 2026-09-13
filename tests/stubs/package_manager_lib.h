#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Stub for unit tests — aligns with logos-package-manager PackageManagerLib public API
// used by PackageManagerImpl.

enum class SignaturePolicy {
    NONE,
    WARN,
    REQUIRE
};

enum class InstallType {
    Embedded,
    User,
};

enum class DependencyStatus {
    Installed,
    NotInstalled,
    Cycle,
    // Installed at a version the depending manifest's semver range rejects.
    // Appended, never inserted — the real enum's numeric values are ABI across
    // libpackage_manager_lib.
    VersionMismatch,
    // Installed, but its manifest.sig does not verify under the key the edge's
    // `signer` DID carries. Identity, not trust — see the real header.
    SignerMismatch,
    // The edge pins a `signer` and no usable signature is installed to check
    // it against. Absence of evidence, as its own status.
    SignerUnknown,
};

struct SignatureVerificationResult {
    bool is_signed = false;
    bool signature_valid = false;
    bool package_valid = false;
    std::string signer_did;
    std::string signer_name;
    std::string signer_url;
    std::string trusted_as;
    std::string error;
};

struct UninstallResult {
    bool success = false;
    std::string errorMsg;
    std::vector<std::string> removedFiles;
};

// Mirrors the real lib's Hashes struct. Only `root` is read today but keeping
// the nested shape matches manifest.json and leaves room for additions.
struct Hashes {
    std::string root;
};

// Mirrors PackageDependency in package_manager_lib.h — one `dependencies[]`
// entry: a plain name, or an object carrying a semver range and/or signer DID.
struct PackageDependency {
    std::string name;
    std::optional<std::string> version;
    std::optional<std::string> signer;

    PackageDependency() = default;
    PackageDependency(std::string n) : name(std::move(n)) {}   // NOLINT: intended implicit
    PackageDependency(const char* n) : name(n) {}              // NOLINT: intended implicit

    bool isSimple() const { return !version.has_value() && !signer.has_value(); }
};

// Mirrors InstalledPackage in package_manager_lib.h.
struct InstalledPackage {
    std::string name;
    std::string displayName;
    std::string version;
    std::string description;
    std::string type;
    std::string category;
    std::string author;
    std::string license;
    std::string icon;
    std::string view;
    std::string manifestVersion;
    std::vector<std::string> dependencies;
    // The subset of those entries declaring a range and/or a signer; empty
    // when every dependency is a bare name.
    std::vector<PackageDependency> dependencyConstraints;
    // The DID the installed manifest.sig names, once verified under the key
    // that DID carries; nullopt when no usable signature is installed
    // (embedded, unsigned, or a signature that does not verify).
    std::optional<std::string> signerDid;
    Hashes hashes;
    InstallType installType = InstallType::User;
    std::string installDir;
    std::string mainFilePath;
};

// Mirrors DependencyTreeNode in package_manager_lib.h.
struct DependencyTreeNode {
    std::string name;
    DependencyStatus status = DependencyStatus::NotInstalled;
    std::string version;
    InstallType installType = InstallType::User;
    // The constraint the PARENT declared on this edge; absent when it named
    // the dependency without one, and always absent on the root.
    std::optional<std::string> requiredVersion;
    std::optional<std::string> requiredSigner;
    // Was the edge reaching this node declared optional? Per-edge like the two
    // above, false on the root, and inherited by a required child of an
    // optional edge. An absent optional dependency is not a broken install.
    bool optional = false;
    // What the installed package's own signature says about itself; the
    // verdict comes from verifying, not from comparing it to requiredSigner.
    std::optional<std::string> signerDid;
    std::vector<DependencyTreeNode> children;

    // Matches the real lib — descendants-only BFS, name-deduped, children
    // cleared on returned copies. Implementation lives in the mock .cpp so
    // the stub header stays declaration-only.
    std::vector<DependencyTreeNode> flatten() const;
};

// Mirrors DependentTreeNode in package_manager_lib.h.
struct DependentTreeNode {
    std::string name;
    std::string version;
    std::string type;
    InstallType installType = InstallType::User;
    std::string installDir;
    std::vector<DependentTreeNode> children;

    std::vector<DependentTreeNode> flatten() const;
};

const char* installTypeToString(InstallType t);
const char* dependencyStatusToString(DependencyStatus s);

// Mirrors the real header: true for everything except NotInstalled and Cycle.
// Decides whether `version` / `installType` carry meaning on a serialised node.
// A predicate rather than an `||` chain, which silently omits each new status.
bool nodeResolvedToAnInstalledPackage(DependencyStatus s);

class PackageManagerLib {
public:
    PackageManagerLib();
    ~PackageManagerLib();

    void setEmbeddedModulesDirectory(const std::string& dir);
    void addEmbeddedModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void addEmbeddedUiPluginsDirectory(const std::string& dir);
    void setUserModulesDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);
    void setInstallVariants(const std::vector<std::string>& variants);

    std::string installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                  bool skipIfNotNewerVersion = false,
                                  std::string* installedPluginPath = nullptr,
                                  bool* isCoreModule = nullptr);

    std::vector<InstalledPackage> getInstalledPackages();
    std::vector<InstalledPackage> getInstalledModules();
    std::vector<InstalledPackage> getInstalledUiPlugins();

    static std::vector<std::string> platformVariantsToTry();

    void setSignaturePolicy(SignaturePolicy policy);
    void setKeyringDirectory(const std::string& dir);
    std::string keyringDirectory();

    SignatureVerificationResult verifyPackageSignature(const std::string& lgxPath);

    UninstallResult uninstallPackage(const std::string& packageName);

    std::optional<DependencyTreeNode> resolveDependencies(const std::string& packageName);
    std::optional<DependentTreeNode>  resolveDependents(const std::string& packageName);
};
