{
  description = "wsland managed by flake";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

    wslg-applist.url = "github:qq1038765585/wslg-flake/main";
    wslg-freerdp.url = "github:qq1038765585/freerdp-flake/working";
  };

  outputs = { self, nixpkgs, wslg-applist, wslg-freerdp }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ];

    in {
      packages = forAllSystems(system: let 
        pkgs = import nixpkgs { inherit system; };

        wslg-freerdp-lib = wslg-freerdp.packages.${system}.default;
        wslg-applist-lib = wslg-applist.packages.${system}.wslg-applist;
      in {
        default = pkgs.stdenv.mkDerivation {
            name = "wsland";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config meson ninja
            ];

            buildInputs = with pkgs; [
              wslg-freerdp-lib wslg-applist-lib wayland wayland-protocols wayland-scanner pixman cairo
              libxcb libxcb-wm libxkbcommon libdrm xwayland openssl wlroots_0_19
            ];
          };
      });

      devShells = forAllSystems(system: let 
        pkgs = import nixpkgs { inherit system; };

        wslg-applist-lib = wslg-applist.packages.${system}.wslg-applist;
        wslg-freerdp-lib = wslg-freerdp.packages.${system}.default;
      in {
        default = pkgs.mkShell {
            packages = with pkgs; [
              pkg-config meson ninja
              wslg-freerdp-lib wslg-applist-lib wayland wayland-protocols wayland-scanner pixman cairo
              libxcb libxcb-wm libxkbcommon libdrm xwayland openssl wlroots_0_19
            ];

            LD_LIBRARY_PATH = "${wslg-freerdp-lib}/lib:${wslg-applist-lib}/lib";
            PKG_CONFIG_PATH = "${wslg-freerdp-lib}/lib/pkgconfig:${wslg-applist-lib}/lib/pkgconfig";
            NIX_CFLAGS_COMPILE = "-I${wslg-freerdp-lib}/include/freerdp2 -I${wslg-freerdp-lib}/include/winpr2";
        };
      });
    };
}
