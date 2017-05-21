with (import ./nixpkgs);
with lib;

runCommandCC "buildfs"
{ nativeBuildInputs = [ pkgconfig ];
  buildInputs = [ lkl ];
  src = ./buildfs.c;
}
''
cc $(pkg-config --cflags --libs lkl) -o $out $src
''
