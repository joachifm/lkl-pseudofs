with (import ../nixpkgs);
with lib;

runCommandCC "buildfs"
{ buildInputs = [ lkl ];
  src = ./buildfs.c;
}
''
cc -llkl -o $out $src
''
