{
  description = "Emacs with Skia graphics backend";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            # Pre-built Skia from Nix
            skia

            # Skia runtime deps
            fontconfig
            freetype
            libGL
            libGLU
            libepoxy # GL function loading (used by GTK3 and for Skia GL backend)
            xorg.libX11
            xorg.libXext
            harfbuzz
            icu
            libjpeg
            libpng
            libwebp
            zlib
            expat

            # Emacs build deps
            autoconf
            automake
            texinfo
            pkg-config
            gtk3
            glib
            gdk-pixbuf
   #         cairo
            pango
            librsvg
            giflib
            libtiff
            gnutls
            ncurses
            ncurses.dev
            jansson
            sqlite
            libxml2
            tree-sitter
            webkitgtk_4_1

            # For debugging
            gdb
            clang-tools
          ];

          shellHook = ''
            # Use Nix Skia package - headers are in include/skia/
            export SKIA_DIR="${pkgs.skia}"
            export SKIA_LIBS="-L${pkgs.skia}/lib -lskia"
            export SKIA_CFLAGS="-I${pkgs.skia}/include/skia"
            echo "Skia development environment ready"
            echo "SKIA_DIR=$SKIA_DIR"
            echo "SKIA_LIBS=$SKIA_LIBS"
            echo "SKIA_CFLAGS=$SKIA_CFLAGS"
          '';
        };
      }
    );
}
