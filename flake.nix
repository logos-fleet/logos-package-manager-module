{
  description = "Logos Package Manager Module - Plugin manager for the Logos system";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # `tests.mockCLibs = ["logos_pm"]` builds the unit tests against the
    # hand-maintained mirror in tests/stubs/package_manager_lib.h, never
    # linking the real library — only `nix build` catches a mismatch.
    logos-package-manager.url = "github:logos-co/logos-package-manager";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        logos_pm = {
          input = inputs.logos-package-manager;
          packages = {
            default = "lib";
            portable = "lib-portable";
          };
          # THE SAME LIBRARY, FOR A PHONE. `generate` stages a BUILD-platform
          # image of logos_pm into lib/, and nothing in logos-module-builder can
          # recompile it -- it comes from its own flake -- so the mobile Bare
          # build asks here, per target.
          #
          # `legacyPackages.<buildSystem>.mobile.<target>.lib`: a CROSS
          # derivation's `system` is its BUILD platform, so the archives cannot
          # live under `packages.<target>`. All three mobile targets are there,
          # and all three are STATIC archives with lgx folded in -- iOS because
          # it loads no dynamic library of its own, Android because logos-nix's
          # DT_NEEDED gate refuses an unbundled liblgx.so inside an APK.
          #
          # `or null` stays: a target with no build is refused BY NAME by
          # logos-module-builder, which is a better answer than a link error
          # forty lines deep.
          mobilePackages = { system, buildSystem, ... }:
            inputs.logos-package-manager.legacyPackages.${buildSystem}.mobile.${system}.lib
              or null;
        };
      };
      tests = {
        dir = ./tests;
        # Same key as nix.external_libraries[].name — documents intent; go_static filtering is N/A here.
        mockCLibs = [ "logos_pm" ];
      };
    };
}
