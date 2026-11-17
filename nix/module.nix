# NixOS module: builds snd-fireface-ucx for the configured kernel and installs
# it (udev loads it when the UCX is plugged in: the module has a USB alias),
# with ucxcfg and ucxtui, and applies each user's ~/.config/ucx/config.json
# whenever the unit appears (udev/70-snd-fireface-ucx.rules starts the user
# unit below; it does nothing for a user without that file), and tells
# PipeWire the unit's latency (wireplumber/51-fireface-ucx-latency.conf).
{ config, pkgs, ... }:
let
  ucxcfg = pkgs.callPackage ./ucxcfg.nix { pipewire = config.services.pipewire.package; };
in
{
  boot.extraModulePackages = [ (config.boot.kernelPackages.callPackage ./kmod.nix { }) ];

  environment.systemPackages = [ ucxcfg ];

  services.udev.packages = [ ucxcfg ];

  # Install the single-speed latency correction and the 96 kHz duplex profile.
  services.pipewire.wireplumber.configPackages = [ ucxcfg ];

  # Same as systemd/ucxcfg-apply.service.in, which explains it
  systemd.user.services.ucxcfg-apply = {
    description = "Apply your Fireface UCX config (~/.config/ucx/config.json)";
    wantedBy = [ "default.target" ];
    unitConfig = {
      ConditionPathExists = "%E/ucx/config.json";
      StartLimitIntervalSec = 60;
      StartLimitBurst = 4;
    };
    serviceConfig = {
      Type = "oneshot";
      ExecStartPre = "${pkgs.coreutils}/bin/sleep 2";
      ExecStart = "${ucxcfg}/bin/ucxcfg apply --push %E/ucx/config.json";
      SuccessExitStatus = 3;
      Restart = "on-failure";
      RestartSec = 3;
    };
  };
}
