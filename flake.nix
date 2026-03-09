{
  description = "wsland managed by flake";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

    wslg-app.url = "path:/home/pp/workspace/wslg";
    wslg-freerdp.url = "path:/home/pp/workspace/wslg/vendor/FreeRDP";
  };

  outputs = { self, nixpkgs, wslg-app, wslg-freerdp }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ];

    in {
      packages = forAllSystems(system: let 
        pkgs = import nixpkgs { inherit system; };

        wslgApplistLib = wslg-app.packages.${system}.wslg-applist;
        wslgFreerdpLib = wslg-freerdp.packages.${system}.default;

      in {
        default = pkgs.stdenv.mkDerivation {
            name = "wsland";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config meson ninja
            ];

            buildInputs = with pkgs; [
              wslgFreerdpLib wslgApplistLib wayland wayland-protocols wayland-scanner pixman cairo
              libxcb libxcb-wm libxkbcommon libdrm xwayland openssl
            ];
          };
      });

      devShells = forAllSystems(system: let 
        pkgs = import nixpkgs { inherit system; };

        wslgApplistLib = wslg-app.packages.${system}.wslg-applist;
        wslgFreerdpLib = wslg-freerdp.packages.${system}.default;
      in {
        default = pkgs.mkShell {
            packages = with pkgs; [
              pkg-config meson ninja
              wslgFreerdpLib wslgApplistLib wayland wayland-protocols wayland-scanner pixman cairo
              libxcb libxcb-wm libxkbcommon libdrm xwayland openssl
            ];

            LD_LIBRARY_PATH = "${wslgFreerdpLib}/lib";
            PKG_CONFIG_PATH = "${wslgFreerdpLib}/lib/pkgconfig";
            NIX_CFLAGS_COMPILE = "-I${wslgFreerdpLib}/include/freerdp2 -I${wslgFreerdpLib}/include/winpr2";
        };
      });
    };
}
