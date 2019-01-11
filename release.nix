{ nixpkgs ? import <nixpkgs>{} }:
with nixpkgs;

{
  build = pkgs.callPackage ./lkl/tools/default.nix {};
}
