/**
 * Tests for signature policy and verification on PackageManagerImpl.
 */
#include <logos_test.h>
#include "package_manager_impl.h"
#include <package_manager_lib.h>
#include <lgx.h>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>

/**
 * Helper to get the preferred platform variant (matches what the library expects).
 */
static std::string currentVariant() {
    std::vector<std::string> variants = PackageManagerLib::platformVariantsToTry();
    return variants.empty() ? "unknown" : variants.front();
}

/**
 * Helper: create a minimal unsigned .lgx package in a temp directory.
 * Returns path to the .lgx file as std::string.
 */
static std::string createUnsignedPackage(const QString& dir, const QString& name) {
    QString lgxPath = dir + "/" + name + ".lgx";
    QString contentDir = dir + "/" + name + "_content";
    QDir().mkpath(contentDir);

    // Create a fake library file
#if defined(__APPLE__)
    QString libName = name + "_plugin.dylib";
#elif defined(_WIN32)
    QString libName = name + "_plugin.dll";
#else
    QString libName = name + "_plugin.so";
#endif
    {
        QFile f(contentDir + "/" + libName);
        f.open(QIODevice::WriteOnly);
        f.write("fake library content");
    }

    // Create a manifest.json
    {
        QFile f(contentDir + "/manifest.json");
        f.open(QIODevice::WriteOnly);
        f.write(QString("{"
            "\"name\":\"%1\","
            "\"version\":\"1.0.0\","
            "\"type\":\"core\","
            "\"description\":\"Test\","
            "\"category\":\"test\""
            "}").arg(name).toUtf8());
    }

    // Create LGX package
    std::string lgxStd = lgxPath.toStdString();
    std::string nameStd = name.toStdString();
    lgx_result_t res = lgx_create(lgxStd.c_str(), nameStd.c_str());
    if (!res.success) return {};

    lgx_package_t pkg = lgx_load(lgxStd.c_str());
    if (!pkg) return {};

    lgx_set_version(pkg, "1.0.0");
    lgx_set_description(pkg, "Test package");

    std::string variant = currentVariant();
    res = lgx_add_variant(pkg, variant.c_str(),
                          contentDir.toStdString().c_str(),
                          libName.toStdString().c_str());
    if (!res.success) { lgx_free_package(pkg); return {}; }

    res = lgx_save(pkg, lgxStd.c_str());
    lgx_free_package(pkg);

    return res.success ? lgxStd : std::string();
}

/**
 * Helper: generate a keypair. Returns path to .jwk file as std::string.
 */
static std::string generateKey(const QString& keysDir, const QString& keyName) {
    lgx_result_t res = lgx_keygen(keyName.toStdString().c_str(), keysDir.toStdString().c_str());
    if (!res.success) return {};
    return (keysDir + "/" + keyName + ".jwk").toStdString();
}

/**
 * Helper: read DID from a .did file.
 */
static std::string readDid(const QString& keysDir, const QString& keyName) {
    QFile f(keysDir + "/" + keyName + ".did");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed().toStdString();
}

/**
 * Helper: sign a package with a key.
 */
static bool signPackage(const std::string& lgxPath, const std::string& keyPath,
                         const std::string& signerName = {}, const std::string& signerUrl = {}) {
    lgx_result_t res = lgx_sign(
        lgxPath.c_str(),
        keyPath.c_str(),
        signerName.empty() ? nullptr : signerName.c_str(),
        signerUrl.empty() ? nullptr : signerUrl.c_str()
    );
    return res.success;
}


/**
 * Helper: one string field out of a LogosMap, defaulting to empty.
 */
static std::string str(const LogosMap& m, const char* key) {
    return m.value(key, std::string());
}

// =============================================================================
// Signature policy configuration
// =============================================================================

LOGOS_TEST(set_signature_policy_none) {
    PackageManagerImpl impl;

    // Should not crash or warn
    impl.setSignaturePolicy("none");
    impl.setSignaturePolicy("NONE");
    impl.setSignaturePolicy("None");
}

LOGOS_TEST(set_signature_policy_warn) {
    PackageManagerImpl impl;

    impl.setSignaturePolicy("warn");
}

LOGOS_TEST(set_signature_policy_require) {
    PackageManagerImpl impl;

    impl.setSignaturePolicy("require");
}

LOGOS_TEST(set_keyring_directory) {
    PackageManagerImpl impl;

    impl.setKeyringDirectory("/custom/keyring");
}

// =============================================================================
// Verify unsigned package
// =============================================================================

LOGOS_TEST(verify_unsigned_package) {
    PackageManagerImpl impl;

    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    std::string lgxPath = createUnsignedPackage(tmpDir.path(), "verify_unsigned");
    LOGOS_ASSERT_FALSE(lgxPath.empty());

    LogosMap result = impl.verifyPackage(lgxPath);

    LOGOS_ASSERT_FALSE(result["isSigned"].get<bool>());
    LOGOS_ASSERT_FALSE(result["signatureValid"].get<bool>());
    LOGOS_ASSERT_TRUE(result["signerDid"].get<std::string>().empty());
}

// =============================================================================
// Verify signed package
// =============================================================================

LOGOS_TEST(verify_signed_package) {
    PackageManagerImpl impl;

    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    QString keysDir = tmpDir.path() + "/keys";
    QDir().mkpath(keysDir);

    std::string lgxPath = createUnsignedPackage(tmpDir.path(), "verify_signed");
    LOGOS_ASSERT_FALSE(lgxPath.empty());

    std::string keyPath = generateKey(keysDir, "testkey");
    LOGOS_ASSERT_FALSE(keyPath.empty());
    LOGOS_ASSERT_TRUE(signPackage(lgxPath, keyPath, "Test Signer", "https://example.com"));

    LogosMap result = impl.verifyPackage(lgxPath);

    LOGOS_ASSERT_TRUE(result["isSigned"].get<bool>());
    LOGOS_ASSERT_TRUE(result["signatureValid"].get<bool>());
    LOGOS_ASSERT_TRUE(result["packageValid"].get<bool>());
    LOGOS_ASSERT_EQ(result["signerName"].get<std::string>(), std::string("Test Signer"));
    LOGOS_ASSERT_EQ(result["signerUrl"].get<std::string>(), std::string("https://example.com"));
    // DID should start with did:jwk:
    LOGOS_ASSERT_TRUE(result["signerDid"].get<std::string>().substr(0, 8) == "did:jwk:");
    // Not in keyring, so not trusted
    LOGOS_ASSERT_TRUE(result["trustedAs"].get<std::string>().empty());
}

// =============================================================================
// Verify signed + trusted package
// =============================================================================

LOGOS_TEST(verify_signed_trusted_package) {
    PackageManagerImpl impl;

    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    QString keysDir = tmpDir.path() + "/keys";
    QString keyringDir = tmpDir.path() + "/keyring";
    QDir().mkpath(keysDir);
    QDir().mkpath(keyringDir);

    impl.setKeyringDirectory(keyringDir.toStdString());

    std::string lgxPath = createUnsignedPackage(tmpDir.path(), "verify_trusted");
    LOGOS_ASSERT_FALSE(lgxPath.empty());

    std::string keyPath = generateKey(keysDir, "trustkey");
    LOGOS_ASSERT_FALSE(keyPath.empty());
    LOGOS_ASSERT_TRUE(signPackage(lgxPath, keyPath));

    // Add key to keyring
    std::string did = readDid(keysDir, "trustkey");
    LOGOS_ASSERT_FALSE(did.empty());

    LogosMap addResult = impl.addTrustedKey("my-publisher", did, "Publisher Name", "https://pub.com");
    LOGOS_ASSERT_TRUE(addResult["success"].get<bool>());

    LogosMap result = impl.verifyPackage(lgxPath);

    LOGOS_ASSERT_TRUE(result["isSigned"].get<bool>());
    LOGOS_ASSERT_TRUE(result["signatureValid"].get<bool>());
    LOGOS_ASSERT_EQ(result["trustedAs"].get<std::string>(), std::string("my-publisher"));
}

// =============================================================================
// Install with signature policies
// =============================================================================

LOGOS_TEST(install_unsigned_with_policy_none_succeeds) {
    PackageManagerImpl impl;

    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    impl.setSignaturePolicy("none");
    impl.setUserModulesDirectory((tmpDir.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmpDir.path() + "/ui").toStdString());

    std::string lgxPath = createUnsignedPackage(tmpDir.path(), "install_none");
    LOGOS_ASSERT_FALSE(lgxPath.empty());

    LogosMap result = impl.installPlugin(lgxPath, false);
    LOGOS_ASSERT_FALSE(result.contains("error"));
}

LOGOS_TEST(install_unsigned_with_policy_require_rejected) {
    PackageManagerImpl impl;

    QTemporaryDir tmpDir;
    LOGOS_ASSERT_TRUE(tmpDir.isValid());

    impl.setSignaturePolicy("require");
    impl.setKeyringDirectory((tmpDir.path() + "/keyring").toStdString());
    impl.setUserModulesDirectory((tmpDir.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmpDir.path() + "/ui").toStdString());

    std::string lgxPath = createUnsignedPackage(tmpDir.path(), "install_req");
    LOGOS_ASSERT_FALSE(lgxPath.empty());

    LogosMap result = impl.installPlugin(lgxPath, false);
    LOGOS_ASSERT_TRUE(result.contains("error"));
    LOGOS_ASSERT_TRUE(result["error"].get<std::string>().find("unsigned") != std::string::npos);
}

// =============================================================================
// The signer-trust prompt (issue #18) -- real lgx, real Ed25519
// =============================================================================
//
// A Store shell shows who signed a package before it installs it. That prompt
// needs the signer's display NAME, their DID and whether the local keyring
// vouches for them, plus a fourth fact the UI cannot derive: whether this build
// would install the package AT ALL.
//
// signerTrust() is that one answer, and the reason it exists rather than the
// caller composing verifyPackage() + listTrustedKeys() + the policy is that the
// composition IS the policy. A prompt showing a DID beside an Install button the
// installer would refuse is a prompt that lies; one hiding the button the
// installer would accept is a feature nobody can reach. Each test below asserts
// the verdict AND the install, so the two cannot drift apart.
//
// The composition itself is unit-tested against the mocked verifier in
// tests/test_signer_trust.cpp, which is the suite `nix build .#unit-tests` runs.
// These need real signatures: a mocked verifier cannot tell a valid signature
// from a stale one.

// ── what the prompt shows ───────────────────────────────────────────────────

LOGOS_TEST(signerTrust_shows_the_signer_name_and_did) {
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    QDir().mkpath(keysDir);

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_named");
    LOGOS_ASSERT_FALSE(lgx.empty());
    const std::string key = generateKey(keysDir, "publisher");
    LOGOS_ASSERT_FALSE(key.empty());
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "Acme Modules", "https://acme.example"));

    const LogosMap t = impl.signerTrust(lgx);

    LOGOS_ASSERT_EQ(str(t, "name"), std::string("trust_named"));
    LOGOS_ASSERT_EQ(str(t, "version"), std::string("1.0.0"));
    LOGOS_ASSERT_EQ(str(t, "signatureStatus"), std::string("signed"));
    LOGOS_ASSERT_EQ(str(t, "signerName"), std::string("Acme Modules"));
    LOGOS_ASSERT_EQ(str(t, "signerDid").substr(0, 8), std::string("did:jwk:"));
    // Signed is not trusted. The keyring is empty, and the prompt has to be able
    // to say "this publisher is not one you know" — which is the whole point of
    // showing a DID rather than a name a signer chose for themselves.
    LOGOS_ASSERT_FALSE(t.value("trusted", true));
    LOGOS_ASSERT_TRUE(str(t, "trustedAs").empty());
}

LOGOS_TEST(signerTrust_reports_a_keyring_match_by_its_local_name) {
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    const QString keyring = tmp.path() + "/keyring";
    QDir().mkpath(keysDir);
    QDir().mkpath(keyring);
    impl.setKeyringDirectory(keyring.toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_known");
    const std::string key = generateKey(keysDir, "known");
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "Known Publisher"));
    const std::string did = readDid(keysDir, "known");
    LOGOS_ASSERT_TRUE(impl.addTrustedKey("my-publisher", did, "Publisher", "https://p.example")
                          .value("success", false));

    const LogosMap t = impl.signerTrust(lgx);

    LOGOS_ASSERT_TRUE(t.value("trusted", false));
    LOGOS_ASSERT_EQ(str(t, "trustedAs"), std::string("my-publisher"));
    LOGOS_ASSERT_EQ(str(t, "signerDid"), did);
}

// ── the verdict, and the install that must agree with it ────────────────────

LOGOS_TEST(an_unsigned_package_cannot_be_installed_under_require) {
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    impl.setSignaturePolicy("require");
    impl.setKeyringDirectory((tmp.path() + "/keyring").toStdString());
    impl.setUserModulesDirectory((tmp.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmp.path() + "/ui").toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_unsigned");
    LOGOS_ASSERT_FALSE(lgx.empty());

    const LogosMap t = impl.signerTrust(lgx);
    LOGOS_ASSERT_EQ(str(t, "signatureStatus"), std::string("unsigned"));
    LOGOS_ASSERT_FALSE(t.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(t, "reason"), "unsigned");
    // There is no signer to show, and no prompt to show it in.
    LOGOS_ASSERT_TRUE(str(t, "signerDid").empty());

    // And the verdict is not advice: the installer refuses the same package.
    LOGOS_ASSERT_TRUE(impl.installPlugin(lgx, false).contains("error"));
}

LOGOS_TEST(a_package_whose_signature_no_longer_matches_cannot_be_installed) {
    // The "mismatched" case, and the one an attacker actually has: a real
    // publisher's signature over content that is not what they signed. The
    // signature covers a Merkle root over the package, so changing the package
    // after signing leaves both halves present and disagreeing.
    //
    // Built the way the API makes it reachable: sign, then add a variant and
    // save. lgx re-emits manifest.sig verbatim while the hashes it commits to
    // are recomputed, which is exactly the shape.
    //
    // Asserted at WARN, the DESKTOP DEFAULT -- a content mismatch is not a trust
    // preference, so the permissive-but-checking policy must refuse it too.
    // (NONE does not check at all; that is the cross-install bundler's path and
    // is a declared opt-out rather than an oversight.)
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    QDir().mkpath(keysDir);
    impl.setSignaturePolicy("warn");
    impl.setKeyringDirectory((tmp.path() + "/keyring").toStdString());
    impl.setUserModulesDirectory((tmp.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmp.path() + "/ui").toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_mismatch");
    LOGOS_ASSERT_FALSE(lgx.empty());
    const std::string key = generateKey(keysDir, "victim");
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "Victim Publisher"));
    // It verifies before the change -- otherwise the assertion below would pass
    // for the wrong reason.
    LOGOS_ASSERT_EQ(str(impl.signerTrust(lgx), "signatureStatus"), std::string("signed"));

    // Add a second variant's worth of content under the signed root.
    {
        const QString extra = tmp.path() + "/extra_content";
        QDir().mkpath(extra);
        QFile f(extra + "/other_plugin.so");
        f.open(QIODevice::WriteOnly);
        f.write("different library content");
        f.close();
        lgx_package_t pkg = lgx_load(lgx.c_str());
        LOGOS_ASSERT_TRUE(pkg != nullptr);
        LOGOS_ASSERT_TRUE(lgx_add_variant(pkg, "linux-x86_64",
                                          extra.toStdString().c_str(),
                                          "other_plugin.so").success);
        LOGOS_ASSERT_TRUE(lgx_save(pkg, lgx.c_str()).success);
        lgx_free_package(pkg);
    }

    const LogosMap t = impl.signerTrust(lgx);
    LOGOS_ASSERT_EQ(str(t, "signatureStatus"), std::string("invalid"));
    LOGOS_ASSERT_FALSE(t.value("installable", true));
    LOGOS_ASSERT_FALSE(t.value("trusted", true));

    LOGOS_ASSERT_TRUE(impl.installPlugin(lgx, false).contains("error"));
}

LOGOS_TEST(a_signed_package_is_installable_under_warn_even_untrusted) {
    // The desktop default. The prompt still shows the DID and still says the
    // publisher is unknown; it just does not veto.
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    QDir().mkpath(keysDir);
    impl.setSignaturePolicy("warn");
    impl.setKeyringDirectory((tmp.path() + "/keyring").toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_warn");
    const std::string key = generateKey(keysDir, "stranger");
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "A Stranger"));

    const LogosMap t = impl.signerTrust(lgx);

    LOGOS_ASSERT_EQ(str(t, "signatureStatus"), std::string("signed"));
    LOGOS_ASSERT_FALSE(t.value("trusted", true));
    LOGOS_ASSERT_TRUE(t.value("installable", false));
}

LOGOS_TEST(an_untrusted_signer_is_refused_under_require) {
    // REQUIRE is the Store shell's setting: a valid signature by a publisher the
    // keyring does not know is not enough.
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    QDir().mkpath(keysDir);
    impl.setSignaturePolicy("require");
    impl.setKeyringDirectory((tmp.path() + "/keyring").toStdString());
    impl.setUserModulesDirectory((tmp.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmp.path() + "/ui").toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_unknown_signer");
    const std::string key = generateKey(keysDir, "unknown");
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "Unknown Publisher"));

    const LogosMap t = impl.signerTrust(lgx);

    LOGOS_ASSERT_EQ(str(t, "signatureStatus"), std::string("signed"));
    LOGOS_ASSERT_FALSE(t.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(t, "reason"), "keyring");
    // The name and DID are still there: this is the prompt that offers to add
    // them to the keyring.
    LOGOS_ASSERT_EQ(str(t, "signerName"), std::string("Unknown Publisher"));
    LOGOS_ASSERT_FALSE(str(t, "signerDid").empty());

    LOGOS_ASSERT_TRUE(impl.installPlugin(lgx, false).contains("error"));
}

LOGOS_TEST(a_trusted_signer_is_installable_under_require) {
    // The positive control. Without it every refusal above would pass against a
    // signerTrust that answered "not installable" unconditionally.
    PackageManagerImpl impl;
    QTemporaryDir tmp;
    LOGOS_ASSERT_TRUE(tmp.isValid());
    const QString keysDir = tmp.path() + "/keys";
    const QString keyring = tmp.path() + "/keyring";
    QDir().mkpath(keysDir);
    QDir().mkpath(keyring);
    impl.setSignaturePolicy("require");
    impl.setKeyringDirectory(keyring.toStdString());
    impl.setUserModulesDirectory((tmp.path() + "/modules").toStdString());
    impl.setUserUiPluginsDirectory((tmp.path() + "/ui").toStdString());

    const std::string lgx = createUnsignedPackage(tmp.path(), "trust_ok");
    const std::string key = generateKey(keysDir, "good");
    LOGOS_ASSERT_TRUE(signPackage(lgx, key, "Good Publisher"));
    impl.addTrustedKey("good-publisher", readDid(keysDir, "good"), "Good", "https://good.example");

    const LogosMap t = impl.signerTrust(lgx);

    LOGOS_ASSERT_TRUE(t.value("trusted", false));
    LOGOS_ASSERT_TRUE(t.value("installable", false));
    LOGOS_ASSERT_FALSE(impl.installPlugin(lgx, false).contains("error"));
}

LOGOS_TEST(signerTrust_on_a_path_that_is_not_a_package) {
    PackageManagerImpl impl;

    const LogosMap t = impl.signerTrust("/nonexistent/nope.lgx");

    LOGOS_ASSERT_FALSE(t.value("installable", true));
    LOGOS_ASSERT_FALSE(str(t, "error").empty());
}

// ── availability, against the real variant vocabulary ───────────────────────

LOGOS_TEST(availability_matches_a_legacy_architecture_spelling) {
    // The two catalog producers in this ecosystem disagree about how to spell an
    // architecture (darwin-amd64 vs darwin-x86_64), and logos-package owns the
    // reconciliation. Availability has to consult it, or an entry a build CAN
    // install reads as unavailable.
    PackageManagerImpl impl;
    impl.setInstallableVariants({"darwin-x86_64"});

    const LogosMap a = impl.variantAvailability(R"(["darwin-amd64"])");

    LOGOS_ASSERT_TRUE(a.value("available", false));
    LOGOS_ASSERT_EQ(str(a, "variant"), std::string("darwin-amd64"));
}

LOGOS_TEST(availability_never_aliases_the_os_half) {
    // Which is what stops a Windows package installing as a macOS one.
    PackageManagerImpl impl;
    impl.setInstallableVariants({"darwin-x86_64"});

    LOGOS_ASSERT_FALSE(impl.variantAvailability(R"(["windows-x86_64"])").value("available", true));
    LOGOS_ASSERT_FALSE(impl.variantAvailability(R"(["linux-x86_64"])").value("available", true));
}
