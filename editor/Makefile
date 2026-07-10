# editor/Makefile — cross-compile teidraw.exe (Win64 PE) with mingw-w64.
# Dear ImGui + the DX11/Win32 backends are source-vendored from the flake's
# $IMGUI_DIR and compiled directly into the PE. Run inside `nix develop` so
# $MINGW_CXX / $IMGUI_DIR / $STB_INC / $NLOHMANN_INC / $FONT_* are set.
# `:=` (not `?=`) for CXX — the dev shell exports CXX=host-g++ which must lose
# to the mingw cross-compiler (a command-line `make CXX=...` still wins).

CXX      := $(MINGW_CXX)
IMGUI    ?= $(IMGUI_DIR)
STBI     ?= $(STB_INC)
JSON_INC ?= $(NLOHMANN_INC)

# FFmpeg/libav cross libs (from the flake) for in-process video/gif decode.
# Present → -DTEI_LIBAV + static link; absent → images-only build still works.
FFMPEG_INC ?= $(FFMPEG_CROSS_INC)
FFMPEG_LIB ?= $(FFMPEG_CROSS_LIB)
ifneq ($(FFMPEG_LIB),)
  VIDEO_CXXFLAGS := -DTEI_LIBAV -I $(FFMPEG_INC)
  # Static link order matters: high-level → low-level av libs, THEN the Win32
  # libs they pull (must come after the archives that reference them).
  VIDEO_LIBS     := -L$(FFMPEG_LIB) -lavformat -lavcodec -lswscale -lswresample -lavutil \
                    -lmfplat -lmfuuid -lstrmiids -lole32 -loleaut32 -lbcrypt -lsecur32 -luser32 -latomic
endif

BUILD := ../build
OBJ   := $(BUILD)/obj
OUT   := $(BUILD)/teidraw.exe
SRC   := src/main.cpp

# ── embedded fonts (tools/embed.py bakes TTFs into the PE) ────────────────
FONTS_H := $(BUILD)/fonts_embedded.h
FONT_HAND := ../assets/fonts/ShantellSans.ttf

# ImGui core + backends compile ONCE into $(OBJ); only main.o rebuilds on edits.
IMGUI_CORE    := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI_BACKEND := imgui_impl_win32.cpp imgui_impl_dx11.cpp
IMGUI_MISC    := imgui_stdlib.cpp
IMGUI_OBJ := $(addprefix $(OBJ)/,$(IMGUI_CORE:.cpp=.o) $(IMGUI_BACKEND:.cpp=.o) $(IMGUI_MISC:.cpp=.o))
MAIN_OBJ  := $(OBJ)/main.o
ALL_OBJ   := $(MAIN_OBJ) $(IMGUI_OBJ)

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 $(VIDEO_CXXFLAGS) \
  -I src -I $(BUILD) -I $(IMGUI) -I $(IMGUI)/backends -I $(IMGUI)/misc/cpp \
  -I $(JSON_INC) -I $(STBI) -I $(STBI)/stb
LDFLAGS  := -static -static-libgcc -static-libstdc++ -mconsole
LIBS     := -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -limm32 -lole32 -luuid \
            -lwinmm -lcomdlg32 -lshell32 -lshlwapi $(VIDEO_LIBS)

$(OUT): $(ALL_OBJ) | $(BUILD)
	$(CXX) $(ALL_OBJ) -o $(OUT) $(LDFLAGS) $(LIBS)
	@echo "built $(OUT)"

$(FONTS_H): $(FONT_HAND) ../tools/embed.py | $(BUILD)
	python3 ../tools/embed.py $@ \
	  font_hand=$(FONT_HAND) font_sans=$(FONT_SANS) \
	  font_mono=$(FONT_MONO) font_serif=$(FONT_SERIF)

$(MAIN_OBJ): $(SRC) $(FONTS_H) | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $@

$(OBJ)/%.o: $(IMGUI)/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.o: $(IMGUI)/backends/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.o: $(IMGUI)/misc/cpp/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)
$(OBJ):
	mkdir -p $(OBJ)

# Render a headless screenshot of the UI and drop it in the build dir.
shot: $(OUT)
	cd .. && ./build/teidraw.exe --shot build/shot.png --frames 8

run: $(OUT)
	cd .. && ./build/teidraw.exe

clean:
	rm -rf $(OBJ) $(OUT) $(FONTS_H)

.PHONY: clean run shot
