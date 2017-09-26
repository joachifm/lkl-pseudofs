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
cc -DCONFIG_AUTO_LKL_POSIX_HOST -llkl -o $out $src
''
