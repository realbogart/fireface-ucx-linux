# ucxcfg and ucxtui, the configuration tools for the Fireface UCX, with their library:
#   pkgs.callPackage ./nix/ucxcfg.nix { }
# It talks to the snd-fireface-ucx kernel module (nix/kmod.nix, nix/module.nix).
{
  lib,
  stdenv,
  pkg-config,
  alsa-lib,
  jansson,
}:

stdenv.mkDerivation {
  pname = "ucxcfg";
  version = "0.1";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../Makefile
      ../include
      ../lib
      ../tui
      ../tools/ucxcfg.c
      ../tests/config.sh
      ../tests/proto.c
      ../tests/doc.c
      ../examples
      ../udev
      ../systemd
      ../wireplumber
    ];
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    alsa-lib
    jansson
  ];

  buildFlags = [
    "ucxcfg"
    "ucxtui"
  ];
  doCheck = true;
  checkTarget = "check";
  installTargets = [ "install-ucxcfg" ];
  installFlags = [ "PREFIX=${placeholder "out"}" ];

  meta = {
    description = "Apply a declarative JSON configuration to an RME Fireface UCX (snd-fireface-ucx), from a file or a terminal editor";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "ucxcfg";
  };
}
