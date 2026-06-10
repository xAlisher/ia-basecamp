{
  description = "ia-basecamp — sovereign archive follower & preservation (ui_qml module with C++ backend)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
    nix-bundle-lgx.follows = "logos-module-builder/nix-bundle-lgx";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
