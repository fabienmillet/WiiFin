#---------------------------------------------------------------------------------
# devkitPro/devkitPPC environment
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

PREFIX      := $(DEVKITPPC)/bin/powerpc-eabi-
CC          := $(PREFIX)gcc
CXX         := $(PREFIX)g++
LD          := $(CXX)
OBJCOPY     := $(PREFIX)objcopy
PKG_CONFIG  := PKG_CONFIG_LIBDIR=/opt/devkitpro/portlibs/ppc/lib/pkgconfig pkg-config

#---------------------------------------------------------------------------------
# Project structure
#---------------------------------------------------------------------------------
TARGET      := WiiFin
BUILD       := build
SOURCES     := source source/core source/input source/ui source/jellyfin source/player
ASSETS      := data textures
INCLUDES    := $(SOURCES) $(BUILD)

#---------------------------------------------------------------------------------
# Source files and assets
#---------------------------------------------------------------------------------
CPPFILES    := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp))
CFILES      := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))

SRC_OFILES  := $(addprefix $(BUILD)/,$(notdir $(CPPFILES:.cpp=.o)) $(notdir $(CFILES:.c=.o)))
DEPFILES    := $(SRC_OFILES:.o=.d)

# Object files for assets (generated manually)
ASSET_OFILES := \
	$(BUILD)/crt0_pre.o \
	$(BUILD)/stub_zone.o \
	$(BUILD)/wii_font_ttf.o \
	$(BUILD)/jp_font_ttf.o \
	$(BUILD)/button_start_png.o \
	$(BUILD)/logo_wiifin_png.o \
	$(BUILD)/cursor_pointer_png.o \
	$(BUILD)/cursor_hand_open_png.o \
	$(BUILD)/cursor_hand_closed_png.o \
	$(BUILD)/ring_png.o \
	$(BUILD)/cacert_pem.o \
	$(BUILD)/music_mp3.o \
	$(BUILD)/fx_start_mp3.o \
	$(BUILD)/fx_select_mp3.o \
	$(BUILD)/fx_press_key_mp3.o \
	$(BUILD)/fx_menu_exit_mp3.o \
	$(BUILD)/fx_menu_enter_mp3.o \
	$(BUILD)/fx_loading_mp3.o \
	$(BUILD)/fx_backspace_mp3.o \
	$(BUILD)/fx_back_mp3.o \
	$(BUILD)/icon_user_png.o

OFILES      := $(ASSET_OFILES) $(SRC_OFILES)

#---------------------------------------------------------------------------------
# Flags
#---------------------------------------------------------------------------------
INCLUDE := $(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
           -I$(LIBOGC_INC) -I$(BUILD) \
           -I/opt/devkitpro/portlibs/ppc/include \
           -I/opt/devkitpro/portlibs/ppc/include/freetype2 \
           -I/opt/devkitpro/portlibs/wii/include \
           -I$(CURDIR)/libs/mbedtls/include

CFLAGS      := -g -O2 -Wall -MMD -MP $(MACHDEP) $(INCLUDE)
CXXFLAGS    := $(CFLAGS)
LDFLAGS     := -g $(MACHDEP) -Wl,-Map,$(TARGET).map -T $(CURDIR)/tools/wii_wiifin.ld \
               -Wl,--wrap=LWP_JoinThread \
               -Wl,--wrap=SYS_Report       # see source/core/LwpJoinFix.cpp, Log.cpp
LIBPATHS    := -L$(LIBOGC_LIB) -L/opt/devkitpro/portlibs/wii/lib -L/opt/devkitpro/portlibs/ppc/lib \
               -L$(CURDIR)/libs/mbedtls/lib

LIBS        := -lgrrlib -lpngu `$(PKG_CONFIG) freetype2 libpng libjpeg --libs` -lbrotlidec -lbrotlicommon -lwiikeyboard -lfat -lwiiuse -lbte -lmad -lasnd -logc -lm -lz \
               -lmbedtls -lmbedx509 -lmbedcrypto

# Link against mplayer-ce when available (see MPLAYER_CE_BUILD.md)
ifneq ($(wildcard $(CURDIR)/libs/mplayer-ce-build/libmplayer.a),)
LIBPATHS    += -L$(CURDIR)/libs/mplayer-ce-build
LIBS        := -Wl,--start-group -lmplayer -lfribidi -laesnd -ltinysmb -ldi -liso9660 -Wl,--end-group $(LIBS)
endif

#---------------------------------------------------------------------------------
# Rules
#---------------------------------------------------------------------------------
.PHONY: all clean run

all: $(BUILD) $(TARGET).dol

$(BUILD):
	mkdir -p $@

$(TARGET).dol: $(TARGET).elf
	elf2dol $< $@
$(TARGET).elf: $(OFILES)
	$(LD) $^ -o $@ $(LDFLAGS) $(LIBPATHS) $(LIBS)

# Compilation C/C++
$(BUILD)/%.o: source/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/core/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/input/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/ui/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/jellyfin/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/player/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: source/player/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Explicit asset rules
$(BUILD)/cursor_pointer_png.o: data/cursors/PointerP1-64.png
	xxd -i $< > $(BUILD)/cursor_pointer_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/cursor_pointer_png.h

$(BUILD)/cursor_hand_open_png.o: data/cursors/HandOpenP1-64.png
	xxd -i $< > $(BUILD)/cursor_hand_open_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/cursor_hand_open_png.h

$(BUILD)/cursor_hand_closed_png.o: data/cursors/HandClosedP1-64.png
	xxd -i $< > $(BUILD)/cursor_hand_closed_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/cursor_hand_closed_png.h

$(BUILD)/button_start_png.o: data/images/button_start.png
	xxd -i -n data_button_start_png $< > $(BUILD)/button_start_png.h
	$(CC) -x c -c -o $@ $(BUILD)/button_start_png.h

$(BUILD)/logo_wiifin_png.o: data/images/logo_wiifin.png
	xxd -i -n data_logo_wiifin_png $< > $(BUILD)/logo_wiifin_png.h
	$(CC) -x c -c -o $@ $(BUILD)/logo_wiifin_png.h

$(BUILD)/wii_font_ttf.o: data/fonts/wii_font.ttf
	xxd -i -n data_wii_font_ttf $< > $(BUILD)/wii_font_ttf.h
	$(CC) -x c -c -o $@ $(BUILD)/wii_font_ttf.h

$(BUILD)/jp_font_ttf.o: data/fonts/jp_font.ttf
	xxd -i -n data_jp_font_ttf $< > $(BUILD)/jp_font_ttf.h
	$(CC) -x c -c -o $@ $(BUILD)/jp_font_ttf.h

$(BUILD)/ring_png.o: data/images/ring.png
	xxd -i -n data_ring_png $< > $(BUILD)/ring_png.h
	$(CC) -x c -c -o $@ $(BUILD)/ring_png.h

$(BUILD)/cacert_pem.o: data/certs/cacert.pem
	(cat $<; printf '\0') > $(BUILD)/cacert_pem_null.pem
	xxd -i $(BUILD)/cacert_pem_null.pem > $(BUILD)/cacert_pem.h
	sed -i 's/build_cacert_pem_null_pem/data_cacert_pem/g' $(BUILD)/cacert_pem.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/cacert_pem.h

$(BUILD)/music_mp3.o: data/sounds/bgm.mp3
	xxd -i -n data_music_mp3 $< > $(BUILD)/music_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/music_mp3.h

# FX sound effects — use xxd -n to give clean variable names
$(BUILD)/fx_start_mp3.o: data/sounds/start.mp3
	xxd -i -n data_sounds_start_mp3 $< > $(BUILD)/fx_start_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_start_mp3.h

$(BUILD)/fx_select_mp3.o: data/sounds/select.mp3
	xxd -i -n data_sounds_select_mp3 $< > $(BUILD)/fx_select_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_select_mp3.h

$(BUILD)/fx_back_mp3.o: data/sounds/back.mp3
	xxd -i -n data_sounds_back_mp3 $< > $(BUILD)/fx_back_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_back_mp3.h

$(BUILD)/fx_backspace_mp3.o: data/sounds/backspace.mp3
	xxd -i -n data_sounds_backspace_mp3 $< > $(BUILD)/fx_backspace_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_backspace_mp3.h

$(BUILD)/fx_loading_mp3.o: data/sounds/loading.mp3
	xxd -i -n data_sounds_loading_mp3 $< > $(BUILD)/fx_loading_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_loading_mp3.h

$(BUILD)/fx_press_key_mp3.o: data/sounds/press_key.mp3
	xxd -i -n data_sounds_press_key_mp3 $< > $(BUILD)/fx_press_key_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_press_key_mp3.h

$(BUILD)/fx_menu_exit_mp3.o: data/sounds/menu_exit.mp3
	xxd -i -n data_sounds_menu_exit_mp3 $< > $(BUILD)/fx_menu_exit_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_menu_exit_mp3.h

$(BUILD)/fx_menu_enter_mp3.o: data/sounds/menu_enter.mp3
	xxd -i -n data_sounds_menu_enter_mp3 $< > $(BUILD)/fx_menu_enter_mp3.h
	$(CC) -x c -c -o $@ $(BUILD)/fx_menu_enter_mp3.h

$(BUILD)/crt0_pre.o: source/crt0_pre.S
	$(CC) $(MACHDEP) -c $< -o $@

$(BUILD)/stub_zone.o: source/stub_zone.s tools/stub_zone.bin
	$(CC) $(MACHDEP) -c $< -o $@

$(BUILD)/icon_user_png.o: data/images/icon_user.png
	xxd -i -n data_icon_user_png $< > $(BUILD)/icon_user_png.h
	$(CC) -x c -c -o $@ $(BUILD)/icon_user_png.h

# Safe inclusion of dependency files
-include $(DEPFILES)

# Installable channel (title WIFN): see tools/make_wad.py.  Needs libWiiPy;
# setup.sh installs it into tools/.venv, which is used when present.
WADPY := $(if $(wildcard $(CURDIR)/tools/.venv/bin/python),$(CURDIR)/tools/.venv/bin/python,python3)

wad: $(TARGET).wad

$(TARGET).wad: $(TARGET).dol tools/make_wad.py tools/wad/template.wad tools/stub_zone.bin
	$(WADPY) tools/make_wad.py $(TARGET).dol $@

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).dol $(TARGET).map $(TARGET).wad

.PHONY: wad clean run

run: $(TARGET).dol
	wiiload $(TARGET).dol