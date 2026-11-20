{
  description = "wsland managed by flake";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs, }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ];

    in {
      packages = forAllSystems(system:
        let pkgs = import nixpkgs {
            inherit system;
        };

        ws_freerdp = pkgs.stdenv.mkDerivation {
            pname = "freerdp";
            version = "3.32.2";

            src = pkgs.fetchFromGitHub {
              owner = "FreeRDP";
              repo = "FreeRDP";
              tag = "3.32.2";
              hash = "sha256-qLZLD0t/mwxcwdigRZ7I1L7ZhDplW5RNUH6R+x+Os7Y=";
            };

            nativeBuildInputs = with pkgs; [
              pkg-config cmake ninja
            ];

            buildInputs = with pkgs; [
              zlib openssl icu
            ];

            cmakeFlags = [
              "-G Ninja"
              "-DCMAKE_BUILD_TYPE=Debug"
              "-DCMAKE_INSTALL_LIBDIR=lib"
              "-DWITH_VERBOSE_WINPR_ASSERT=OFF"
              "-DWITH_DEBUG_ALL=OFF"
              "-DWITH_ICU=ON"
              "-DWITH_SERVER=ON"
              "-DCHANNEL_GFXREDIR=ON"
              "-DWITH_INTERNAL_RC4=ON"
              "-DWITH_INTERNAL_MD4=ON"
              "-DWITH_FFMPEG=OFF"
              "-DWITH_DSP_FFMPEG=OFF"
              "-DWITH_VIDEO_FFMPEG=OFF"
              "-DWITH_SWSCALE=OFF"
              "-DWITH_KRB5=OFF"
              "-DWITH_CLIENT=OFF"
              "-DWITH_CLIENT_COMMON=OFF"
              "-DWITH_CLIENT_CHANNELS=OFF"
              "-DWITH_CLIENT_INTERFACE=OFF"
              "-DWITH_LIBSYSTEMD=OFF"
              "-DWITH_WAYLAND=OFF"
              "-DWITH_X11=OFF"
              "-DWITH_PROXY=OFF"
              "-DWITH_SHADOW=OFF"
              "-DWITH_SAMPLE=OFF"
            ];
        };
      in {
        default = pkgs.stdenv.mkDerivation {
            name = "wsland";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config meson ninja
            ];

            buildInputs = with pkgs; [
              wayland wayland-protocols wayland-scanner pixman cairo inih
              libxkbcommon libxcb-wm openssl_legacy xwayland wlroots_0_20
            ] ++ [ws_freerdp];
          };
      });

      devShells = forAllSystems(system:
        let pkgs = import nixpkgs {
            inherit system;
        };

        ws_freerdp = pkgs.stdenv.mkDerivation {
            pname = "freerdp";
            version = "3.32.2";

            src = pkgs.fetchFromGitHub {
              owner = "FreeRDP";
              repo = "FreeRDP";
              tag = "3.32.2";
              hash = "sha256-qLZLD0t/mwxcwdigRZ7I1L7ZhDplW5RNUH6R+x+Os7Y=";
            };

            nativeBuildInputs = with pkgs; [
              pkg-config cmake ninja
            ];

            buildInputs = with pkgs; [
              zlib openssl icu
            ];

            cmakeFlags = [
              "-G Ninja"
              "-DCMAKE_BUILD_TYPE=Debug"
              "-DCMAKE_INSTALL_LIBDIR=lib"
              "-DWITH_VERBOSE_WINPR_ASSERT=OFF"
              "-DWITH_DEBUG_ALL=OFF"
              "-DWITH_ICU=ON"
              "-DWITH_SERVER=ON"
              "-DCHANNEL_GFXREDIR=ON"
              "-DWITH_INTERNAL_RC4=ON"
              "-DWITH_INTERNAL_MD4=ON"
              "-DWITH_FFMPEG=OFF"
              "-DWITH_DSP_FFMPEG=OFF"
              "-DWITH_VIDEO_FFMPEG=OFF"
              "-DWITH_SWSCALE=OFF"
              "-DWITH_KRB5=OFF"
              "-DWITH_CLIENT=OFF"
              "-DWITH_CLIENT_COMMON=OFF"
              "-DWITH_CLIENT_CHANNELS=OFF"
              "-DWITH_CLIENT_INTERFACE=OFF"
              "-DWITH_LIBSYSTEMD=OFF"
              "-DWITH_WAYLAND=OFF"
              "-DWITH_X11=OFF"
              "-DWITH_PROXY=OFF"
              "-DWITH_SHADOW=OFF"
              "-DWITH_SAMPLE=OFF"
            ];
        };
      in {
        default = pkgs.mkShell {
            packages = with pkgs; [
              pkg-config meson ninja 
              wayland wayland-protocols wayland-scanner pixman cairo inih
              libxkbcommon libxcb-wm openssl_legacy xwayland wlroots_0_20
            ] ++ [ws_freerdp];
        };
      });
    };
}
