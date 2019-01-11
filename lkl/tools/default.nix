{ lib, runCommandCC, lkl }:

runCommandCC "buildfs-0.0"
{ buildInputs = [ lkl ];
  src = lib.cleanSource ./.;
  preferLocalBuild = true;
  allowSubstitutes = false;
}
''
cc -o lkl-buildfs $src/buildfs.c -llkl -pthread -lrt
install -Dt $out/bin lkl-buildfs
''
