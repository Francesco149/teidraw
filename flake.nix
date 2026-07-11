{
  description = "teidraw — MIT-licensed infinite-canvas whiteboard with tldraw-grade feel (imgui; D3D11 on Windows, SDL3 on Linux)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # ── Windows cross-toolchain ───────────────────────────────────────
        # teidraw is a 64-bit Windows PE cross-compiled with mingw-w64 and run
        # natively on the Windows host via WSLInterop (no WSLg tax). Same
        # proven pattern as the sibling slopstudio editor.
        mingw = pkgs.pkgsCross.mingwW64.buildPackages;

        # ── Dear ImGui, pinned ≥1.92 ──────────────────────────────────────
        # nixpkgs ships 1.91.x; we need the 1.92 dynamic font atlas
        # (ImGuiBackendFlags_RendererHasTextures) so canvas text rasterizes
        # crisp at ANY zoom level instead of scaling fixed-size bitmaps.
        imguiSrc = pkgs.fetchFromGitHub {
          owner = "ocornut";
          repo = "imgui";
          rev = "v1.92.4";
          hash = "sha256-DyQ2fh749S41UFdLto7TtxsnBsd7CBzAUFq36LeZZ5Y=";
        };

        # ── FFmpeg/libav, cross-compiled static (video + GIF decode) ──────
        # Minimal static libav{format,codec,util}+swscale+swresample with
        # FFmpeg's BUILT-IN decoders only (h264/hevc/vp9/av1/gif need no
        # external libs). Static → no DLLs to ship. The upstream cross ffmpeg
        # is flagged broken only because its default feature set pulls libvmaf;
        # the stripped build cross-compiles cleanly.
        ffmpegCross = (pkgs.pkgsCross.mingwW64.ffmpeg.override {
          withHeadlessDeps = false; withSmallDeps = false; withFullDeps = false;
          buildAvcodec = true; buildAvformat = true; buildAvutil = true;
          buildSwscale = true; buildSwresample = true;
          buildAvdevice = false; buildAvfilter = false; buildPostproc = false;
          buildFfmpeg = false; buildFfprobe = false; buildFfplay = false;
          withStatic = true; withShared = false; withNetwork = false;
        }).overrideAttrs (old: { meta = old.meta // { broken = false; }; });
      in {
        devShells.default = pkgs.mkShell {
          name = "teidraw-dev";

          packages = with pkgs; [
            mingw.gcc            # x86_64-w64-mingw32-{gcc,g++} → Win64 PE
            mingw.binutils       # + windres for the icon resource
            gnumake
            pkg-config           # resolves sdl3 + libav for `make linux`
            python3              # tools/embed.py (fonts/icon → C arrays)

            stb                  # stb_image / stb_image_write (image load + screenshots)
            nlohmann_json        # board.json document format

            # host-side media tooling (thumbnails, test assets)
            ffmpeg
            imagemagick

            git
            jq
            ripgrep
            fd
          ];

          # linux-target deps, resolved through the pkg-config hook:
          # SDL3 (window/renderer/clipboard/dialog/audio/pen) + shared libav.
          # The release binary statically links both (see .github/workflows).
          buildInputs = with pkgs; [ sdl3 ffmpeg ];

          shellHook = ''
            export TEIDRAW_ROOT=$PWD

            # Dear ImGui source checkout (compiled directly into the PE).
            export IMGUI_DIR=${imguiSrc}
            export STB_INC=${pkgs.stb}/include
            export NLOHMANN_INC=${pkgs.nlohmann_json}/include

            # FFmpeg/libav cross libs for in-process video/gif decode.
            export FFMPEG_CROSS_INC=${ffmpegCross.dev}/include
            export FFMPEG_CROSS_LIB=${ffmpegCross.lib}/lib

            # All four UI fonts are vendored in assets/fonts/ (OFL) and baked
            # in by tools/embed.py — no font env vars needed anymore.
            export MINGW_CC=x86_64-w64-mingw32-gcc
            export MINGW_CXX=x86_64-w64-mingw32-g++
            export MINGW_STRIP=x86_64-w64-mingw32-strip
            export MINGW_WINDRES=x86_64-w64-mingw32-windres

            echo "teidraw dev shell"
            echo "  imgui:   $IMGUI_DIR"
            echo "  mingw:   $(command -v $MINGW_CXX || echo '(missing)')"
            echo "  libav:   $FFMPEG_CROSS_LIB (cross) + pkg-config (linux)"
            echo "  build:   make -C editor        # windows (default)"
            echo "           make -C editor linux  # native linux (SDL3)"
          '';
        };

        formatter = pkgs.nixfmt-rfc-style;
      });
}
