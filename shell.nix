{ pkgs ? import <nixpkgs> {} }:

let
  raylib_deps = with pkgs; [
    libGL
    xorg.libX11 xorg.libX11.dev
    xorg.libXcursor xorg.libXi xorg.libXinerama
    xorg.libXrandr xorg.libXext xorg.libXfixes
  ];
in
pkgs.mkShell {
  nativeBuildInputs = with pkgs; [ cmake gnumake gcc pkg-config opencode ];

  # NOTE: noto-fonts-cjk-sans provides .ttc files raylib can read.
    buildInputs = (with pkgs; [ raylib noto-fonts-cjk-sans zstd libwebp ]) ++ raylib_deps;
    shellHook = ''
    export PKG_CONFIG_PATH="${pkgs.raylib}/lib/pkgconfig:$PKG_CONFIG_PATH"
    export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath (
      [ pkgs.raylib ] ++ raylib_deps
    )}:$LD_LIBRARY_PATH"

    # Point the app at the Noto CJK font inside the store.
    export STUDYQUEST_FONT="${pkgs.noto-fonts-cjk-sans}/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"

    echo "raylib: $(pkg-config --modversion raylib 2>/dev/null || echo NOT FOUND)"
    echo "font:   $STUDYQUEST_FONT"
  '';
}
