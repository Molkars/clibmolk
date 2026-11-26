{
  description = "clibmolk: single-header C modules";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            gcc just bear
            python3          # clibmolk script and tools/
            gh               # just release
            jujutsu          # version control (jj git init --colocate)
            clang-tools      # clangd, clang-format
          ];
          env.CC = "gcc";
        };
      });

      # `nix run github:molkars/clibmolk -- download array.h v0.1.0`
      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = toString (pkgs.writeShellScript "clibmolk" ''
            exec ${pkgs.python3}/bin/python3 ${./clibmolk} "$@"
          '');
        };
      });
    };
}
