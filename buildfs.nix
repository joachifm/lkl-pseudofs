with (import ./nixpkgs);
with lib;

runCommandCC "buildfs" { buildInputs = [ lkl ]; } ''
cc -llkl -L${lkl.lib}/lib -o $out ${./buildfs.c}
''
