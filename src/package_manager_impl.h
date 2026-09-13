#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdint>
#include <optional>
#include <logos_json.h>
#include <logos_module_context.h>  // LogosModuleContext base; provides `logos_events`

class PackageManagerLib;

class PackageManagerImpl : public LogosModuleContext {
public:
    PackageManagerImpl();
    ~PackageManagerImpl();

    PackageManagerImpl(const PackageManagerImpl&) = delete;
    PackageManagerImpl& operator=(const PackageManagerImpl&) = delete;

    // Install from local LGX file — returns LogosMap {name, path, error, isCoreModule, ...}
    LogosMap installPlugin(const std::string& pluginPath, bool skipIfNotNewerVersion);

    // Inspect an LGX file without installing. Returns package metadata plus
    // already-installed status and dependents so callers can show a confirmation
    // dialog before committing. Shape:
    //   { name, version, type, description, category,
    //     signatureStatus ("signed"|"unsigned"|"invalid"|"error"),
    //     signerDid?, signerName?,
    //     isAlreadyInstalled, installedVersion?,
    //     installedDependents? }
    LogosMap inspectPackage(const std::string& lgxPath);

    // Directory configuration — embedded (multiple, read-only)
    void setEmbeddedModulesDirectory(const std::string& dir);
    void addEmbeddedModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void addEmbeddedUiPluginsDirectory(const std::string& dir);

    // Directory configuration — user (single, writable)
    void setUserModulesDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);

    // Scanning — each returns LogosList (JSON array with all manifest fields
    // + installDir + mainFilePath + installType ("embedded"|"user"))
    LogosList getInstalledPackages();
    LogosList getInstalledModules();
    LogosList getInstalledUiPlugins();

    // Uninstall a user-installed package. Refuses embedded packages.
    // Returns { success: bool, error?: string, removedFiles?: [string] }.
    // On success also emits "corePluginUninstalled" or "uiPluginUninstalled".
    //
    // This is the ungated path — it performs the uninstall immediately.
    // Headless callers (lgpm, scripts, logoscore-driven automation) use this
    // directly. GUI callers should prefer requestUninstall below, which gates
    // the destructive work behind a listener-driven confirmation dialog.
    LogosMap uninstallPackage(const std::string& packageName);

    // Forward dependency walk for an installed package. Returns a tree of
    // { name, status, version, installType, children: [...] } rooted at the
    // queried package. Stops at NotInstalled/Cycle nodes. `recursive=false`
    // walks only one level deep (children have empty `children` arrays);
    // `recursive=true` walks the full tree.
    LogosMap resolveDependencies(const std::string& packageName, bool recursive);

    // Reverse dependency walk — same tree shape as resolveDependencies but
    // for the inverse edge. Returns a tree of { name, version, type,
    // installType, installDir, children: [...] } rooted at the queried
    // package. `recursive=false` walks only depth-1 (direct reverse
    // neighbours, with empty `children`); `recursive=true` walks the full
    // reverse subtree.
    LogosMap resolveDependents(const std::string& packageName, bool recursive);

    // Flat projections of the two walks above. Each returns a LogosList of
    // per-node maps (same fields as the tree version minus `children`).
    // `recursive=false` emits only direct neighbours; `recursive=true`
    // emits every descendant, BFS-ordered and deduplicated by name.
    LogosList resolveFlatDependencies(const std::string& packageName, bool recursive);
    LogosList resolveFlatDependents(const std::string& packageName, bool recursive);

    // Platform variants this build accepts (e.g. ["darwin-arm64-dev"] or ["darwin-arm64"])
    std::vector<std::string> getValidVariants();

    // ── per-variant availability ────────────────────────────────────────────
    //
    // What a catalog entry is allowed to OFFER here. A Store shell installs
    // `web` variants and nothing else — a phone may not download native code —
    // so most of a catalog is uninstallable on one, and the user is entitled to
    // be told that in the entry rather than by an install that fails.
    //
    // `installableVariants` is NOT `validVariants`. The latter is what the
    // loader accepts for a package already on disk (a Store shell's embedded set
    // is native and arrived at build time); the former is what the user may ADD
    // at runtime. On a desktop they nearly coincide; on a phone they do not
    // overlap at all, and conflating them is how a native-only entry acquires an
    // install button.
    //
    // Declared by the host, in PREFERENCE order — a host listing "web" before
    // its native variant means "prefer the container". Unset, it defaults to
    // getValidVariants(); an explicitly EMPTY list means nothing may be
    // installed at runtime, which is a legitimate configuration and must not
    // fall back.
    void setInstallableVariants(const std::vector<std::string>& variants);
    std::vector<std::string> getInstallableVariants();

    // Can this build install a package that ships `variantsJson` (the `variants`
    // array the catalog publishes per package)? Returns
    //   { available, variant, availableOn: [...], reason }
    // `variant` is the one the install would use, empty when unavailable.
    // `availableOn` is the machine-readable half — what it DOES ship — and
    // `reason` is the sentence the entry shows ("available on macOS and Linux,
    // not in this build").
    //
    // Matching goes through logos-package's variant vocabulary, so a legacy
    // architecture spelling in a published package still resolves. The OS half
    // is never aliased.
    LogosMap variantAvailability(const std::string& variantsJson);

    // The same decision over a whole catalog: every entry passes through with an
    // added `availability` key. One implementation of the rule, and none in QML.
    LogosList catalogAvailability(const std::string& catalogJson);

    // ── the signer-trust prompt ─────────────────────────────────────────────
    //
    // Everything a "who signed this?" prompt must show, plus the one thing the
    // UI cannot derive: whether this build would install the package at all.
    //
    // One call rather than verifyPackage() + listTrustedKeys() + the policy,
    // because that composition IS the policy: a prompt showing a DID beside an
    // Install button the installer would refuse is a prompt that lies, and one
    // hiding the button the installer would accept is a feature nobody can
    // reach. `installable` and `installPlugin` answer the same question.
    //
    // Returns { name, version, signatureStatus, signerName, signerDid,
    //           signerUrl, trusted, trustedAs, policy, installable, reason,
    //           error? }.
    LogosMap signerTrust(const std::string& lgxPath);

    // Signature policy configuration
    void setSignaturePolicy(const std::string& policy);
    void setKeyringDirectory(const std::string& dir);

    // Standalone signature verification — returns {isSigned, signatureValid, packageValid, signerDid, ...}
    LogosMap verifyPackage(const std::string& lgxPath);

    // Keyring management — add/remove/list trusted signing keys
    LogosMap addTrustedKey(const std::string& name, const std::string& did,
                           const std::string& displayName, const std::string& url);
    LogosMap removeTrustedKey(const std::string& name);
    LogosList listTrustedKeys();

    // ----------------------------------------------------------------
    // Gated uninstall / upgrade flow with two-phase listener ack.
    // ----------------------------------------------------------------
    //
    // Protocol (GUI-only — headless callers must use the ungated slots above):
    //
    //   1. Caller invokes requestUninstall(name) / requestUpgrade(name, tag, mode).
    //      Returns LogosResult-shaped LogosMap { success, error? } synchronously.
    //      On success, sets pending state, emits "beforeUninstall" / "beforeUpgrade",
    //      and starts a 3s ack-reception timer.
    //
    //   2. Any listener handling the event MUST immediately call
    //      ackPendingAction(name). This cancels the ack timer and tells the module
    //      "I'm driving the dialog — wait indefinitely for my decision."
    //
    //   3a. If the ack timer fires with no ack, the module clears pending state
    //       and emits "uninstallCancelled" / "upgradeCancelled" with a timeout
    //       reason. Destructive work never runs without an owning listener.
    //
    //   3b. Once acked, confirmUninstall / confirmUpgrade performs the work;
    //       cancelUninstall / cancelUpgrade aborts and emits the cancellation
    //       event with reason "user cancelled". Confirm and cancel both
    //       require a prior ack — an un-acked pending state is owned by the
    //       ack-reception timer, and bypassing it would short-circuit the
    //       "no listener acknowledged" timeout path that initiators rely on.
    //       Calling confirm/cancel before ack returns { success: false,
    //       error: "Pending <op> for '<name>' has not been acknowledged" }.
    //
    // Only one gated flow can be pending globally (across packages and ops).
    // A second requestXxx while one is pending returns { success: false, error: ... }.
    LogosMap requestUninstall(const std::string& packageName);
    // `depChanges` is an optional JSON array of the transitive dependency
    // changes the initiator resolved for this operation (each entry shaped
    // { name, action, fromVersion, toVersion, repository }). It is echoed into
    // the beforeUpgrade / beforeInstall payload so the host's confirmation
    // dialog can list exactly what else will change. The module parses it only
    // to re-embed it as a JSON array in the payload (via attachDepChanges) and
    // never interprets or acts on its contents — it is opaque display data.
    // Empty string = the initiator resolved no changes (or didn't compute any).
    LogosMap requestUpgrade(const std::string& packageName, const std::string& releaseTag,
                            int64_t mode, const std::string& depChanges);

    // Fresh-install gate — the confirmation-only sibling of requestUpgrade.
    // Gates a not-yet-installed package behind the same listener-ack protocol
    // so the host shows one confirmation dialog before anything is downloaded.
    // confirmInstall emits "installApproved" (nothing is uninstalled first);
    // cancelInstall / the ack timeout emit "installCancelled".
    LogosMap requestInstall(const std::string& packageName, const std::string& releaseTag,
                            const std::string& repositoryUrl, const std::string& depChanges);
    LogosMap confirmInstall(const std::string& packageName);
    LogosMap cancelInstall(const std::string& packageName);

    LogosMap ackPendingAction(const std::string& packageName);

    LogosMap confirmUninstall(const std::string& packageName);
    LogosMap cancelUninstall(const std::string& packageName);
    LogosMap confirmUpgrade(const std::string& packageName, const std::string& releaseTag);
    LogosMap cancelUpgrade(const std::string& packageName, const std::string& releaseTag);

    LogosMap requestMultiUninstall(const std::vector<std::string>& packageNamesIn);
    LogosMap confirmMultiUninstall(const std::vector<std::string>& packageNamesIn);
    LogosMap cancelMultiUninstall(const std::vector<std::string>& packageNamesIn);

    // Belt-and-braces: clears any pending state. Called by Basecamp at startup
    // so a crash mid-dialog in a previous session doesn't block new requests.
    LogosMap resetPendingAction();

    // Test-only hook — override the ack-reception timeout so timeout-path
    // tests complete in milliseconds instead of the production 3-second
    // default. Must be called before any request*() on this instance (i.e.
    // while no ack-timer worker is running). Not thread-safe against a
    // concurrent worker.
    void setAckTimeoutMsForTest(int ms) { m_ackTimeoutMs = ms; }

    // Events this module emits to listeners (other modules / the host).
    // Declared Qt-`signals:`-style; the codegen supplies the bodies in
    // `package_manager_events.cpp`. Call them like ordinary methods —
    // they no-op when no listener is wired (e.g. a headless runtime with
    // no subscribers). The install/uninstall events carry a single path or
    // package name; the gated-flow events carry a JSON payload string (see
    // emit sites).
logos_events:
    void corePluginFileInstalled(const std::string& path);
    void uiPluginFileInstalled(const std::string& path);
    void corePluginUninstalled(const std::string& packageName);
    void uiPluginUninstalled(const std::string& packageName);
    void beforeUninstall(const std::string& payload);
    void beforeUpgrade(const std::string& payload);
    void beforeInstall(const std::string& payload);
    void beforeMultiUninstall(const std::string& payload);
    void uninstallCancelled(const std::string& payload);
    void upgradeCancelled(const std::string& payload);
    void installCancelled(const std::string& payload);
    void multiUninstallCancelled(const std::string& payload);
    void upgradeUninstallDone(const std::string& payload);
    // Fresh-install gate approval. Unlike upgrade (which uninstalls the old
    // version in-module and signals upgradeUninstallDone), a fresh install has
    // nothing to remove first: confirmInstall simply emits this so the
    // initiator (PMU) runs its download + install chain for the approved
    // package. Payload: { name, releaseTag, repositoryUrl }.
    void installApproved(const std::string& payload);

private:
    enum class PendingOp { None, Uninstall, Upgrade, Install, MultiUninstall };

    struct PendingAction {
        PendingOp   op = PendingOp::None;
        std::string name;             // Uninstall / Upgrade / Install; empty for MultiUninstall (which uses `names`).
        std::vector<std::string> names; // MultiUninstall only — full deduped batch
        std::string releaseTag;        // upgrade / install only
        std::string repositoryUrl;     // install only — echoed back in installApproved
        int64_t     mode = 0;          // upgrade only (UpgradeMode enum as int)
        bool        acked = false;
    };

    // Production ack-reception timeout. Overridable per-instance via
    // setAckTimeoutMsForTest so timeout-path tests don't need to wait
    // 3 real seconds each.
    int m_ackTimeoutMs = 3000;
    static const char* opName(PendingOp op);
    std::string pendingDescriptionLocked() const;

    // ----------------------------------------------------------------
    // Pure-C++ ack timer.
    // ----------------------------------------------------------------
    //
    // When a gated operation (requestUninstall / requestUpgrade) is initiated,
    // a worker thread is spawned that waits on m_ackCv for up to m_ackTimeoutMs.
    // Any concurrent state change — ackPendingAction, confirmXxx, cancelXxx,
    // resetPendingAction, a new requestXxx, or the destructor — bumps
    // m_ackGeneration and notifies the CV. The worker wakes up, observes that
    // its captured generation no longer matches the current one (or m_ackShutdown
    // is set), and exits silently. Only a worker whose captured generation still
    // matches after a full timeout proceeds to emit the cancellation event.
    //
    // Threading rules:
    //   - startAckTimerLocked / stopAckTimerLocked must be called with
    //     m_stateMutex held.
    //   - startAckTimerLocked temporarily releases the lock while joining the
    //     previous worker thread (if any), to avoid a deadlock where the
    //     worker can't exit its wait_for because we're holding the lock.
    //   - stopAckTimerLocked never joins — it only signals. The worker exits
    //     on its own; the next startAckTimerLocked (or the destructor) joins
    //     the now-finished thread.
    //   - The destructor sets m_ackShutdown, notifies the CV, and joins the
    //     worker before destroying any other state.
    //
    // Everything else on this class (m_lib access, file scanning, typed
    // event emission from user code) runs serially on the module thread via
    // the glue layer's queued connection, so no additional locking is needed
    // beyond m_stateMutex guarding the pending-action state.
    void startAckTimerLocked(std::unique_lock<std::mutex>& lock);
    void stopAckTimerLocked();
    void ackTimerWorker(uint64_t myGeneration);

    // Helpers used by the gated-flow slots.
    bool isEmbedded(const std::string& packageName) const;
    std::vector<std::string> installedDependentsNames(const std::string& packageName) const;
    LogosMap doUninstall(const std::string& packageName);
    void emitCancellation(const PendingAction& pa, const std::string& reason);

    PackageManagerLib* m_lib;

    // Unset until the host declares one; getInstallableVariants() then answers
    // from getValidVariants(). An explicit (possibly empty) declaration is
    // stored here and takes over — which is why this is an optional and not a
    // vector whose emptiness would be ambiguous.
    std::optional<std::vector<std::string>> m_installableVariants;

    // The policy as this module last set it, lower-cased. Mirrors what was
    // pushed into the lib so signerTrust() can reproduce the installer's
    // decision; the lib's own getter is absent from the unit tests' stub header,
    // and a prompt that disagreed with the installer is the one failure that
    // method exists to prevent. "warn" is the lib's default.
    std::string m_signaturePolicy = "warn";

    // Guards m_pendingAction and the ack-timer generation/shutdown flags.
    mutable std::mutex      m_stateMutex;
    std::condition_variable m_ackCv;
    std::thread             m_ackThread;
    uint64_t                m_ackGeneration = 0;
    bool                    m_ackShutdown   = false;

    PendingAction m_pendingAction;
};
