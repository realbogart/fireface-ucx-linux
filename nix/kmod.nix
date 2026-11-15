# The snd-fireface-ucx kernel module, built for one kernel:
#   config.boot.kernelPackages.callPackage ./nix/kmod.nix { }
{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:

let
  kdir = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";
in
stdenv.mkDerivation {
  pname = "snd-fireface-ucx";
  version = "0.1-${kernel.version}";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../include
      ../kmod/Kbuild
      (lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") ../kmod)
    ];
  };

  hardeningDisable = [ "pic" ];
  nativeBuildInputs = kernel.moduleBuildDependencies;
  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    kdir
  ];
  # M= needs the absolute path of the unpacked source
  preBuild = ''
    makeFlagsArray+=("M=$PWD/kmod")
  '';
  buildFlags = [ "modules" ];
  installFlags = [ "INSTALL_MOD_PATH=${placeholder "out"}" ];
  installTargets = [ "modules_install" ];

  meta = {
    description = "ALSA driver for the RME Fireface UCX in RME driver mode (USB 2a39:3fc9)";
    license = with lib.licenses; [
      gpl2Only
      mit
    ];
    platforms = lib.platforms.linux;
  };
}
