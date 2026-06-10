{
  description = "ia-basecamp — sovereign archive follower & preservation (ui_qml module with C++ backend)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
    nix-bundle-lgx.follows = "logos-module-builder/nix-bundle-lgx";
    # typed-SDK codegen for the preserve path (flake-inputs-for-module-codegen):
    # without this input the StorageModule wrapper is silently not generated
    storage_module.url = "github:logos-co/logos-storage-module";
    storage_module.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
