{
  description = "Linux driver and tools for the RME Fireface UCX in RME driver mode";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (
        pkgs:
        let
          # Fallback headers; the shell hook prefers the booted kernel's own (see KDIR there)
          kernel = pkgs.linuxPackages.kernel;
        in
        {
          default = pkgs.mkShell {
            packages =
              with pkgs;
              [
                # Capturing and inspecting the USB protocol (usbmon + tshark)
                wireshark-cli
                usbutils

                # Linux driver development
                gnumake
                pkg-config
                libusb1 # ucxctl
                python3 # tools/*.py
                alsa-lib
                alsa-utils
                jansson # JSON for ucxcfg
                gdb
                clang-tools # clangd
                bear # compile_commands.json for kernel modules
              ]
              # pahole etc. for out-of-tree module builds (BTF generation)
              ++ kernel.moduleBuildDependencies;

            KDIR = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";

            shellHook = ''
              # The running kernel: moduleBuildDependencies puts a uname first in PATH
              # that reports the nixpkgs kernel instead
              running_kernel=$(cat /proc/sys/kernel/osrelease)

              # On NixOS, build modules against the booted kernel's own headers (its dev output)
              booted_kernel_drv=$(nix-store -qd "$(dirname "$(readlink -f /run/booted-system/kernel 2>/dev/null)")" 2>/dev/null)
              if [ -n "$booted_kernel_drv" ] && [ "$booted_kernel_drv" != unknown-deriver ] &&
                 booted_kernel_dev=$(nix build --no-link --print-out-paths "$booted_kernel_drv^dev" 2>/dev/null); then
                export KDIR="$booted_kernel_dev/lib/modules/$running_kernel/build"
              elif [ "$running_kernel" != "${kernel.modDirVersion}" ]; then
                echo "warning: running kernel $running_kernel != headers ${kernel.modDirVersion} in \$KDIR;" \
                  "modules built here won't load until those match (see 'kernel' in flake.nix)." >&2
              fi
            '';
          };
        }
      );

      # `nix build .#ucxcfg`: the configuration tools (the kernel module is nix/kmod.nix)
      packages = forAllSystems (pkgs: rec {
        ucxcfg = pkgs.callPackage ./nix/ucxcfg.nix { };
        default = ucxcfg;
      });

      # The kernel module, ucxcfg/ucxtui and the udev rule and user unit
      nixosModules.default = import ./nix/module.nix;

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
