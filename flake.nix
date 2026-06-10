{
  description = "ia-basecamp — sovereign archive follower & preservation (core module + view-only QML UI)";

  inputs = {
    # pinned to the April-era builder whose internal cpp-sdk (f7c855b) predates the
    # LogosTransportConfig ABI break — matches the AppImage's exported liblogos_core
    # (1-arg getClient); newer SDKs bad_alloc against the host's LogosAPI object
    logos-module-builder.url = "github:logos-co/logos-module-builder/29cecd23a2a6bd63d113340cc9773829681598a4";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
    nix-bundle-lgx.follows = "logos-module-builder/nix-bundle-lgx";
    # storage_module_api is VENDORED from stash (src/generated) — no codegen input;
    # the vendored copy is the exact code stash runs against this AppImage daily
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
