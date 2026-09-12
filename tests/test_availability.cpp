// Per-variant availability — what a catalog entry is allowed to offer HERE.
//
// A Store shell installs `web` variants and nothing else: a phone may not
// download native code. So a catalog full of desktop packages is mostly
// uninstallable on one, and the user is entitled to be told that in the entry
// rather than by an install that fails. "Available on macOS and Linux, not in
// this build" is an answer; a greyed-out button is not, and a button that throws
// is worse.
//
// The decision is a function of two lists and nothing else: the variants the
// package ships, and the variants this build can install at runtime. It reads
// no disk, loads no package, and is the same function on every platform — which
// is why it is unit-tested against a mocked library rather than against a device.
//
// `installableVariants` is NOT `validVariants`. The latter is what the loader
// accepts for a package already on disk (a Store shell's embedded set is native
// and arrives at build time); the former is what the user may ADD at runtime.
// On a desktop they nearly coincide; on a phone they do not overlap at all, and
// conflating them is exactly how a native-only entry acquires an install button.

#include <logos_test.h>

#include "package_manager_impl.h"

#include <string>

namespace {

// The catalog shape: the `variants` array the downloader publishes per package.
std::string variants(const std::string& jsonArray) { return jsonArray; }

std::string reasonOf(const LogosMap& m) { return m.value("reason", std::string()); }

}  // namespace

// ── the installable set ─────────────────────────────────────────────────────

LOGOS_TEST(installableVariants_defaults_to_what_the_loader_accepts) {
    auto t = LogosTestContext("package_manager");
    t.mockCFunction("platformVariantsToTry_first").returns("darwin-arm64");

    PackageManagerImpl impl;

    const std::vector<std::string> v = impl.getInstallableVariants();
    LOGOS_ASSERT_EQ(static_cast<int>(v.size()), 1);
    LOGOS_ASSERT_EQ(v[0], std::string("darwin-arm64"));
}

LOGOS_TEST(a_store_shell_declares_web_and_only_web) {
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;

    impl.setInstallableVariants({"web"});

    const std::vector<std::string> v = impl.getInstallableVariants();
    LOGOS_ASSERT_EQ(static_cast<int>(v.size()), 1);
    LOGOS_ASSERT_EQ(v[0], std::string("web"));
}

LOGOS_TEST(an_empty_declaration_means_nothing_may_be_installed) {
    // A shell that installs nothing at runtime is a legitimate configuration,
    // and it must not silently fall back to the loader's native variant.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;

    impl.setInstallableVariants({});

    LOGOS_ASSERT_TRUE(impl.getInstallableVariants().empty());
    const LogosMap a = impl.variantAvailability(variants(R"(["web"])"));
    LOGOS_ASSERT_FALSE(a.value("available", true));
}

// ── available ───────────────────────────────────────────────────────────────

LOGOS_TEST(a_web_variant_is_available_on_a_store_shell) {
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a = impl.variantAvailability(variants(R"(["web","ios-arm64"])"));

    LOGOS_ASSERT_TRUE(a.value("available", false));
    // The variant the install would use — the Shell needs it to name the
    // container, and a caller that only got a bool would have to re-derive it.
    LOGOS_ASSERT_EQ(a.value("variant", std::string()), std::string("web"));
    LOGOS_ASSERT_TRUE(a.value("availableOn", LogosList::array()).empty());
}

LOGOS_TEST(the_first_matching_installable_variant_wins) {
    // The declared order is a preference, not a set: a host that lists
    // "web" before its native variant means "prefer the container".
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web", "darwin-arm64"});

    const LogosMap a = impl.variantAvailability(variants(R"(["darwin-arm64","web"])"));

    LOGOS_ASSERT_TRUE(a.value("available", false));
    LOGOS_ASSERT_EQ(a.value("variant", std::string()), std::string("web"));
}

LOGOS_TEST(a_dev_flavoured_build_accepts_the_plain_variant) {
    // A non-portable build appends "-dev" to its own variant name. That is a
    // property of the CONSUMER's build, not of the package, so a package
    // shipping "darwin-arm64" is installable on a "darwin-arm64-dev" build.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"darwin-arm64-dev"});

    const LogosMap a = impl.variantAvailability(variants(R"(["darwin-arm64"])"));

    LOGOS_ASSERT_TRUE(a.value("available", false));
    LOGOS_ASSERT_EQ(a.value("variant", std::string()), std::string("darwin-arm64"));
}

// ── unavailable, with the reason ────────────────────────────────────────────

LOGOS_TEST(a_native_only_package_is_unavailable_and_says_where_it_runs) {
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a =
        impl.variantAvailability(variants(R"(["darwin-arm64","linux-x86_64"])"));

    LOGOS_ASSERT_FALSE(a.value("available", true));
    LOGOS_ASSERT_EQ(a.value("variant", std::string()), std::string(""));
    // The machine-readable half: what it DOES ship, so a caller can render its
    // own phrasing or filter on it.
    LOGOS_ASSERT_EQ(a.value("availableOn", LogosList::array()).size(), std::size_t(2));
    // The human half, which is what the entry shows.
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "macOS");
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "Linux");
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "not in this build");
}

LOGOS_TEST(the_reason_names_each_platform_once_however_many_variants_it_has) {
    // Two Windows architectures are one sentence about Windows.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a =
        impl.variantAvailability(variants(R"(["windows-x86_64","windows-arm64"])"));

    LOGOS_ASSERT_FALSE(a.value("available", true));
    const std::string reason = reasonOf(a);
    LOGOS_ASSERT_CONTAINS(reason, "Windows");
    LOGOS_ASSERT_EQ(reason.find("Windows", reason.find("Windows") + 1), std::string::npos);
}

LOGOS_TEST(a_package_with_no_variants_at_all_says_so) {
    // Distinct from "runs elsewhere": there is nowhere it runs, so pointing the
    // user at another platform would be a lie.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a = impl.variantAvailability(variants(R"([])"));

    LOGOS_ASSERT_FALSE(a.value("available", true));
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "no variant");
    LOGOS_ASSERT_TRUE(reasonOf(a).find("not in this build") == std::string::npos);
}

LOGOS_TEST(unparseable_variant_input_is_refused_rather_than_read_as_empty) {
    // An empty variant list and a broken one are different facts, and the
    // second must not render as "this package ships nothing".
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a = impl.variantAvailability("{not json");

    LOGOS_ASSERT_FALSE(a.value("available", true));
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "could not be read");
}

LOGOS_TEST(the_mobile_platforms_are_named_the_way_a_user_names_them) {
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    LOGOS_ASSERT_CONTAINS(
        reasonOf(impl.variantAvailability(variants(R"(["android-arm64"])"))), "Android");
    LOGOS_ASSERT_CONTAINS(
        reasonOf(impl.variantAvailability(variants(R"(["ios-arm64"])"))), "iOS");
    // The simulator is not iOS: telling a user their package "runs on iOS" when
    // only a simulator build exists sends them to look for it on a phone.
    LOGOS_ASSERT_CONTAINS(
        reasonOf(impl.variantAvailability(variants(R"(["ios-sim-arm64"])"))), "simulator");
}

LOGOS_TEST(an_unknown_variant_name_is_reported_verbatim) {
    // A private target this vocabulary has never heard of is still somewhere the
    // package runs. Dropping it would under-report availability.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosMap a = impl.variantAvailability(variants(R"(["freebsd-riscv64"])"));

    LOGOS_ASSERT_FALSE(a.value("available", true));
    LOGOS_ASSERT_CONTAINS(reasonOf(a), "freebsd-riscv64");
}

// ── the whole catalog at once ───────────────────────────────────────────────

LOGOS_TEST(catalogAvailability_annotates_every_entry_in_place) {
    // The Shell has a list, not one package. Annotating it in the module means
    // the availability rule has exactly one implementation and the QML has none.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const std::string catalog = R"([
      { "name": "counter_ui",   "variants": ["web"] },
      { "name": "desktop_only", "variants": ["darwin-arm64","linux-x86_64"] }
    ])";

    const LogosList out = impl.catalogAvailability(catalog);

    LOGOS_ASSERT_EQ(out.size(), std::size_t(2));
    LOGOS_ASSERT_EQ(out[0].value("name", std::string()), std::string("counter_ui"));
    LOGOS_ASSERT_TRUE(out[0]["availability"].value("available", false));
    LOGOS_ASSERT_EQ(out[1].value("name", std::string()), std::string("desktop_only"));
    LOGOS_ASSERT_FALSE(out[1]["availability"].value("available", true));
    LOGOS_ASSERT_CONTAINS(out[1]["availability"].value("reason", std::string()), "macOS");
}

LOGOS_TEST(catalogAvailability_keeps_every_other_field_the_entry_carried) {
    // It ANNOTATES. An entry that lost its report link on the way through would
    // take the report affordance with it.
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosList out = impl.catalogAvailability(
        R"([{ "name": "counter_ui", "variants": ["web"], "reportUrl": "https://x/report" }])");

    LOGOS_ASSERT_EQ(out.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(out[0].value("reportUrl", std::string()), std::string("https://x/report"));
}

LOGOS_TEST(catalogAvailability_on_an_entry_with_no_variants_key) {
    // A catalog older than the variants field must not read as "available".
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    const LogosList out = impl.catalogAvailability(R"([{ "name": "mystery" }])");

    LOGOS_ASSERT_EQ(out.size(), std::size_t(1));
    LOGOS_ASSERT_FALSE(out[0]["availability"].value("available", true));
}

LOGOS_TEST(catalogAvailability_refuses_input_that_is_not_a_list) {
    auto t = LogosTestContext("package_manager");
    PackageManagerImpl impl;
    impl.setInstallableVariants({"web"});

    LOGOS_ASSERT_TRUE(impl.catalogAvailability("{not json").empty());
    LOGOS_ASSERT_TRUE(impl.catalogAvailability(R"({"name":"x"})").empty());
}
