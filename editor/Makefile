# editor/Makefile — teidraw, two targets from one tree:
#   make            → ../build/teidraw.exe  (Win64 PE, mingw-w64 cross; default)
#   make linux      → ../build/teidraw      (native Linux, SDL3 + SDL_Renderer)
# Dear ImGui + the platform backends are source-vendored from the flake's
# $IMGUI_DIR and compiled directly into the binary. Run inside `nix develop`
# so $MINGW_CXX / $IMGUI_DIR / $STB_INC / $NLOHMANN_INC (+ pkg-config for the
# linux target) are set. Fonts and the icon are vendored in ../assets and
# baked in by tools/embed.py — no runtime files to ship.

IMGUI    ?= $(IMGUI_DIR)
STBI     ?= $(STB_INC)
JSON_INC ?= $(NLOHMANN_INC)

BUILD := ../build
FONTS_H := $(BUILD)/fonts_embedded.h

FONT_HAND  := ../assets/fonts/ShantellSans.ttf
FONT_SANS  := ../assets/fonts/InterVariable.ttf
FONT_MONO  := ../assets/fonts/JetBrainsMono-Regular.ttf
FONT_SERIF := ../assets/fonts/Lora-Regular.ttf
ICON_PNG   := ../assets/icon-256.png

CXXFLAGS_COMMON := -std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -I src -I $(BUILD) -I $(IMGUI) -I $(IMGUI)/backends -I $(IMGUI)/misc/cpp \
  -I $(JSON_INC) -I $(STBI) -I $(STBI)/stb

IMGUI_CORE := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI_MISC := imgui_stdlib.cpp

# ══ windows (default) ═══════════════════════════════════════════════════════
# `:=` (not `?=`) for the compiler — the dev shell exports CXX=host-g++ which
# must lose to the mingw cross-compiler (a command-line `make WIN_CXX=` wins).
WIN_CXX := $(MINGW_CXX)
WINDRES := $(MINGW_WINDRES)

# FFmpeg/libav cross libs (from the flake) for in-process video/gif decode.
# Present → -DTEI_LIBAV + static link; absent → images-only build still works.
FFMPEG_INC ?= $(FFMPEG_CROSS_INC)
FFMPEG_LIB ?= $(FFMPEG_CROSS_LIB)
ifneq ($(FFMPEG_LIB),)
  WIN_VIDEO_CXXFLAGS := -DTEI_LIBAV -I $(FFMPEG_INC)
  # Static link order matters: high-level → low-level av libs, THEN the Win32
  # libs they pull (must come after the archives that reference them).
  WIN_VIDEO_LIBS     := -L$(FFMPEG_LIB) -lavformat -lavcodec -lswscale -lswresample -lavutil \
                        -lmfplat -lmfuuid -lstrmiids -lole32 -loleaut32 -lbcrypt -lsecur32 -luser32 -latomic
endif

OBJW    := $(BUILD)/obj
WIN_OUT := $(BUILD)/teidraw.exe
SRC     := src/main.cpp

WIN_BACKENDS := imgui_impl_win32.cpp imgui_impl_dx11.cpp
WIN_IMGUI_OBJ := $(addprefix $(OBJW)/,$(IMGUI_CORE:.cpp=.o) $(WIN_BACKENDS:.cpp=.o) $(IMGUI_MISC:.cpp=.o))
WIN_OBJ := $(OBJW)/main.o $(WIN_IMGUI_OBJ) $(OBJW)/teidraw.res

WIN_CXXFLAGS := $(CXXFLAGS_COMMON) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 $(WIN_VIDEO_CXXFLAGS)
WIN_LDFLAGS  := -static -static-libgcc -static-libstdc++ -mconsole
WIN_LIBS     := -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -limm32 -lole32 -luuid \
                -lwinmm -lcomdlg32 -lshell32 -lshlwapi $(WIN_VIDEO_LIBS)

win: $(WIN_OUT)
$(WIN_OUT): $(WIN_OBJ) | $(BUILD)
	$(WIN_CXX) $(WIN_OBJ) -o $(WIN_OUT) $(WIN_LDFLAGS) $(WIN_LIBS)
	@echo "built $(WIN_OUT)"

$(OBJW)/teidraw.res: teidraw.rc ../assets/icon.ico | $(OBJW)
	$(WINDRES) teidraw.rc -O coff -o $@

$(OBJW)/main.o: $(SRC) $(FONTS_H) | $(OBJW)
	$(WIN_CXX) $(WIN_CXXFLAGS) -c $(SRC) -o $@
$(OBJW)/%.o: $(IMGUI)/%.cpp | $(OBJW)
	$(WIN_CXX) $(WIN_CXXFLAGS) -c $< -o $@
$(OBJW)/%.o: $(IMGUI)/backends/%.cpp | $(OBJW)
	$(WIN_CXX) $(WIN_CXXFLAGS) -c $< -o $@
$(OBJW)/%.o: $(IMGUI)/misc/cpp/%.cpp | $(OBJW)
	$(WIN_CXX) $(WIN_CXXFLAGS) -c $< -o $@

# ══ linux ═══════════════════════════════════════════════════════════════════
# Dev builds resolve SDL3 + shared libav via pkg-config (flake buildInputs)
# and rpath the nix lib dirs so the binary runs from the shell. CI overrides
# SDL3_CFLAGS/SDL3_LIBS/LIN_FFMPEG_CFLAGS/LIN_FFMPEG_LIBS with static
# archives for a portable release binary.
LINUX_CXX ?= $(CXX)

SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS   ?= $(shell pkg-config --libs sdl3 2>/dev/null)

LIN_FFMPEG_PKGS := libavformat libavcodec libswscale libswresample libavutil
LIN_FFMPEG_CFLAGS ?= $(shell pkg-config --cflags $(LIN_FFMPEG_PKGS) 2>/dev/null)
LIN_FFMPEG_LIBS   ?= $(shell pkg-config --libs $(LIN_FFMPEG_PKGS) 2>/dev/null)
ifneq ($(strip $(LIN_FFMPEG_LIBS)),)
  LIN_VIDEO_CXXFLAGS := -DTEI_LIBAV $(LIN_FFMPEG_CFLAGS)
endif

# every -L dir doubles as an rpath so dev binaries find their nix .so's
, := ,
LIN_RPATH := $(patsubst -L%,-Wl$(,)-rpath$(,)%,$(filter -L%,$(SDL3_LIBS) $(LIN_FFMPEG_LIBS)))

OBJL      := $(BUILD)/obj-linux
LINUX_OUT := $(BUILD)/teidraw

LINUX_BACKENDS := imgui_impl_sdl3.cpp imgui_impl_sdlrenderer3.cpp
LINUX_IMGUI_OBJ := $(addprefix $(OBJL)/,$(IMGUI_CORE:.cpp=.o) $(LINUX_BACKENDS:.cpp=.o) $(IMGUI_MISC:.cpp=.o))
LINUX_OBJ := $(OBJL)/main.o $(LINUX_IMGUI_OBJ)

LINUX_CXXFLAGS := $(CXXFLAGS_COMMON) $(SDL3_CFLAGS) $(LIN_VIDEO_CXXFLAGS)
LINUX_LDFLAGS  ?= -static-libgcc -static-libstdc++
LINUX_LIBS     := $(SDL3_LIBS) $(LIN_FFMPEG_LIBS) $(LINUX_EXTRA_LIBS) -lpthread -ldl -lm $(LIN_RPATH)

linux: $(LINUX_OUT)
$(LINUX_OUT): $(LINUX_OBJ) | $(BUILD)
	$(LINUX_CXX) $(LINUX_OBJ) -o $(LINUX_OUT) $(LINUX_LDFLAGS) $(LINUX_LIBS)
	@echo "built $(LINUX_OUT)"

$(OBJL)/main.o: $(SRC) $(FONTS_H) | $(OBJL)
	$(LINUX_CXX) $(LINUX_CXXFLAGS) -c $(SRC) -o $@
$(OBJL)/%.o: $(IMGUI)/%.cpp | $(OBJL)
	$(LINUX_CXX) $(LINUX_CXXFLAGS) -c $< -o $@
$(OBJL)/%.o: $(IMGUI)/backends/%.cpp | $(OBJL)
	$(LINUX_CXX) $(LINUX_CXXFLAGS) -c $< -o $@
$(OBJL)/%.o: $(IMGUI)/misc/cpp/%.cpp | $(OBJL)
	$(LINUX_CXX) $(LINUX_CXXFLAGS) -c $< -o $@

# ══ shared ══════════════════════════════════════════════════════════════════
# tools/embed.py bakes the TTFs + icon into the binary
$(FONTS_H): $(FONT_HAND) $(FONT_SANS) $(FONT_MONO) $(FONT_SERIF) $(ICON_PNG) ../tools/embed.py | $(BUILD)
	python3 ../tools/embed.py $@ \
	  font_hand=$(FONT_HAND) font_sans=$(FONT_SANS) \
	  font_mono=$(FONT_MONO) font_serif=$(FONT_SERIF) \
	  icon_png=$(ICON_PNG)

$(BUILD):
	mkdir -p $(BUILD)
$(OBJW):
	mkdir -p $(OBJW)
$(OBJL):
	mkdir -p $(OBJL)

# Render a headless screenshot of the UI and drop it in the build dir.
shot: $(WIN_OUT)
	cd .. && ./build/teidraw.exe --shot build/shot.png --frames 8

shot-linux: $(LINUX_OUT)
	cd .. && ./build/teidraw --shot build/shot-linux.png --frames 8

run: $(WIN_OUT)
	cd .. && ./build/teidraw.exe

clean:
	rm -rf $(OBJW) $(OBJL) $(WIN_OUT) $(LINUX_OUT) $(FONTS_H)

.PHONY: win linux clean run shot shot-linux
