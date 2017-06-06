let
  nixexprs = builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs-channels/archive/nixpkgs-unstable.tar.gz";
  };
in
with (import nixexprs { config = { }; });

runCommandCC "buildfs"
{ buildInputs = [ lkl ];
  src = ./buildfs.c;
}
''
cc -llkl -o $out $src
''
