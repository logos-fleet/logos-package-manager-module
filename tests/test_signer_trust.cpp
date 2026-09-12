// The signer-trust prompt's COMPOSITION.
//
// A Store shell shows who signed a package before it installs it. The prompt
// needs the signer's display NAME, their DID and whether the local keyring
// vouches for them, plus a fourth fact the UI cannot derive on its own: whether
// this build would install the package AT ALL.
//
// `signerTrust()` is that one answer. It exists rather than the caller composing
// verifyPackage() + listTrustedKeys() + the policy because the composition IS
// the policy: a prompt showing a DID beside an Install button the installer
// would refuse is a prompt that lies, and one hiding the button the installer
// would accept is a feature nobody can reach.
//
// So this file's subject is the MAPPING — which (policy, is_signed,
// signature_valid, package_valid, trusted_as) tuple yields which verdict — and
// it drives it through the mocked verifier, which is the only way to enumerate
// the tuples. Real Ed25519 signatures, and the assertion that `installPlugin`
// refuses exactly what `installable: false` says it will, are in
// test_signature.cpp's integration half.
//
// The mapping mirrors PackageManagerLib::installPluginFile's gate, in the same
// order. If that gate changes and this does not, the two disagree — which is
// the failure this method was added to prevent, so the table below is the
// regression guard for it.

#include <logos_test.h>

#include "package_manager_impl.h"

#include <string>

namespace {

// The three booleans the verifier reports, named so a test reads as the
// situation it describes rather than as three bare `true`s.
struct Verdict {
    bool isSigned = false;
    bool signatureValid = false;
    bool packageValid = true;
};

void mockVerifier(LogosTestContext& t, const Verdict& v,
                  const char* did = "", const char* signerName = "",
                  const char* trustedAs = "") {
    t.mockCFunction("verifyPackageSignature_is_signed").returns(v.isSigned);
    t.mockCFunction("verifyPackageSignature_signature_valid").returns(v.signatureValid);
    t.mockCFunction("verifyPackageSignature_package_valid").returns(v.packageValid);
    t.mockCFunction("verifyPackageSignature_signer_did").returns(did);
    t.mockCFunction("verifyPackageSignature_signer_name").returns(signerName);
    t.mockCFunction("verifyPackageSignature_trusted_as").returns(trustedAs);
}

// Let signerTrust past the package load, which the mock refuses by default.
void mockLoadablePackage(LogosTestContext& t, const char* name, const char* version) {
    t.mockCFunction("lgx_load_ok").returns(true);
    t.mockCFunction("lgx_get_name").returns(name);
    t.mockCFunction("lgx_get_version").returns(version);
}

std::string str(const LogosMap& m, const char* key) { return m.value(key, std::string()); }

const Verdict kSignedAndValid{true, true, true};
const Verdict kUnsigned{false, false, true};
const Verdict kSignatureDoesNotVerify{true, false, true};
const Verdict kContentsDoNotMatch{true, true, false};

}  // namespace

// ── what the prompt shows ───────────────────────────────────────────────────

LOGOS_TEST(signerTrust_carries_the_signer_name_and_did) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.2.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc", "Acme Modules");

    PackageManagerImpl impl;
    const LogosMap out = impl.signerTrust("/tmp/counter_ui.lgx");

    LOGOS_ASSERT_EQ(str(out, "name"), std::string("counter_ui"));
    LOGOS_ASSERT_EQ(str(out, "version"), std::string("1.2.0"));
    LOGOS_ASSERT_EQ(str(out, "signatureStatus"), std::string("signed"));
    LOGOS_ASSERT_EQ(str(out, "signerName"), std::string("Acme Modules"));
    LOGOS_ASSERT_EQ(str(out, "signerDid"), std::string("did:jwk:abc"));
}

LOGOS_TEST(signed_is_not_trusted) {
    // The keyring is what vouches, and a name a signer chose for themselves is
    // not evidence. This is why the prompt shows a DID at all.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc", "Totally Legit Inc");

    PackageManagerImpl impl;
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_FALSE(out.value("trusted", true));
    LOGOS_ASSERT_TRUE(str(out, "trustedAs").empty());
}

LOGOS_TEST(a_keyring_match_is_reported_by_its_LOCAL_name) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc", "Acme Modules", "my-publisher");

    PackageManagerImpl impl;
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_TRUE(out.value("trusted", false));
    LOGOS_ASSERT_EQ(str(out, "trustedAs"), std::string("my-publisher"));
}

LOGOS_TEST(a_keyring_hit_over_content_that_does_not_match_is_NOT_trusted) {
    // The sharp one. `trusted_as` is filled from the DID the signature CLAIMS,
    // and logos-package populates that before checking anything — so a keyring
    // hit alone would put a known publisher's name on somebody else's code.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kContentsDoNotMatch, "did:jwk:abc", "Acme Modules", "my-publisher");

    PackageManagerImpl impl;
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_EQ(str(out, "signatureStatus"), std::string("invalid"));
    LOGOS_ASSERT_FALSE(out.value("trusted", true));
}

LOGOS_TEST(signerTrust_on_a_file_that_is_not_a_package) {
    auto t = LogosTestContext("package_manager");  // lgx_load refuses by default
    PackageManagerImpl impl;

    const LogosMap out = impl.signerTrust("/nonexistent/nope.lgx");

    LOGOS_ASSERT_FALSE(out.value("installable", true));
    LOGOS_ASSERT_FALSE(str(out, "error").empty());
}

// ── the verdict: the installer's gate, reproduced ───────────────────────────

LOGOS_TEST(under_require_an_unsigned_package_is_not_installable) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kUnsigned);

    PackageManagerImpl impl;
    impl.setSignaturePolicy("require");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_EQ(str(out, "signatureStatus"), std::string("unsigned"));
    LOGOS_ASSERT_FALSE(out.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "unsigned");
    LOGOS_ASSERT_TRUE(str(out, "signerDid").empty());
}

LOGOS_TEST(under_require_a_signature_from_an_unknown_publisher_is_not_installable) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc", "Unknown Publisher");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("require");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_FALSE(out.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "keyring");
    // The name and DID are still there: this is the prompt that OFFERS to add
    // them, so hiding them would remove the only way forward.
    LOGOS_ASSERT_EQ(str(out, "signerName"), std::string("Unknown Publisher"));
    LOGOS_ASSERT_FALSE(str(out, "signerDid").empty());
}

LOGOS_TEST(under_require_a_trusted_signature_is_installable) {
    // The positive control. Without it every refusal here would pass against a
    // signerTrust that answered "not installable" unconditionally.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc", "Acme Modules", "my-publisher");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("require");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_TRUE(out.value("installable", false));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "my-publisher");
}

LOGOS_TEST(a_signature_that_does_not_verify_is_refused_under_warn_too) {
    // WARN is the desktop default, and a broken signature is not a trust
    // PREFERENCE — the bytes are not the ones the signer signed.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignatureDoesNotVerify, "did:jwk:abc", "Victim Publisher");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("warn");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_EQ(str(out, "signatureStatus"), std::string("invalid"));
    LOGOS_ASSERT_FALSE(out.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "does not verify");
}

LOGOS_TEST(content_that_does_not_match_is_refused_under_warn_too) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kContentsDoNotMatch, "did:jwk:abc");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("warn");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_FALSE(out.value("installable", true));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "not what was signed");
}

LOGOS_TEST(under_warn_an_unsigned_package_is_installable_and_says_why) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kUnsigned);

    PackageManagerImpl impl;
    impl.setSignaturePolicy("warn");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_TRUE(out.value("installable", false));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "warning");
}

LOGOS_TEST(warn_is_the_default_policy_when_the_host_declares_none) {
    // The lib's default, mirrored. A module that defaulted to "none" would
    // report every broken signature as installable.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignatureDoesNotVerify, "did:jwk:abc");

    PackageManagerImpl impl;  // no setSignaturePolicy call
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_EQ(str(out, "policy"), std::string("warn"));
    LOGOS_ASSERT_FALSE(out.value("installable", true));
}

LOGOS_TEST(under_none_nothing_is_checked_and_the_answer_says_so) {
    // The declared opt-out: the nix cross-install bundler runs this way. It must
    // be visible in the answer rather than looking like a clean bill of health.
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignatureDoesNotVerify, "did:jwk:abc");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("none");
    const LogosMap out = impl.signerTrust("/tmp/x.lgx");

    LOGOS_ASSERT_TRUE(out.value("installable", false));
    LOGOS_ASSERT_CONTAINS(str(out, "reason"), "does not check signatures");
}

LOGOS_TEST(an_invalid_policy_word_does_not_change_the_policy) {
    auto t = LogosTestContext("package_manager");
    mockLoadablePackage(t, "counter_ui", "1.0.0");
    mockVerifier(t, kSignedAndValid, "did:jwk:abc");

    PackageManagerImpl impl;
    impl.setSignaturePolicy("require");
    impl.setSignaturePolicy("permissive");  // rejected, warns on stderr

    LOGOS_ASSERT_EQ(str(impl.signerTrust("/tmp/x.lgx"), "policy"), std::string("require"));
}
