#---------------------------------------------------------------------------------
# Environnement devkitPro/devkitPPC
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
# Structure projet
#---------------------------------------------------------------------------------
TARGET      := WiiFin
BUILD       := build
SOURCES     := source source/core source/input source/ui source/jellyfin
ASSETS      := data textures
INCLUDES    := $(SOURCES) $(BUILD)

#---------------------------------------------------------------------------------
# Récupération des fichiers sources et assets
#---------------------------------------------------------------------------------
CPPFILES    := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp))
CFILES      := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))

SRC_OFILES  := $(addprefix $(BUILD)/,$(notdir $(CPPFILES:.cpp=.o)) $(notdir $(CFILES:.c=.o)))
DEPFILES    := $(SRC_OFILES:.o=.d)

# Objets pour les assets (générés manuellement)
ASSET_OFILES := \
	$(BUILD)/bg_main_png.o \
	$(BUILD)/button_start_png.o \
	$(BUILD)/logo_wiifin_png.o \
	$(BUILD)/wii_font_ttf.o \
	$(BUILD)/cursor_png.o

OFILES      := $(SRC_OFILES) $(ASSET_OFILES)

#---------------------------------------------------------------------------------
# Flags
#---------------------------------------------------------------------------------
INCLUDE := $(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
           -I$(LIBOGC_INC) -I$(BUILD) \
           -I/opt/devkitpro/portlibs/ppc/include \
           -I/opt/devkitpro/portlibs/wii/include

CFLAGS      := -g -O2 -Wall -MMD -MP $(MACHDEP) $(INCLUDE)
CXXFLAGS    := $(CFLAGS)
LDFLAGS     := -g $(MACHDEP) -Wl,-Map,$(TARGET).map
LIBPATHS    := -L$(LIBOGC_LIB) -L/opt/devkitpro/portlibs/wii/lib -L/opt/devkitpro/portlibs/ppc/lib

LIBS        := -lgrrlib -lpngu `$(PKG_CONFIG) freetype2 libpng libjpeg --libs` -lfat -lwiiuse -lbte -logc -lm -lz

#---------------------------------------------------------------------------------
# Règles
#---------------------------------------------------------------------------------
.PHONY: all clean run

all: $(BUILD) $(TARGET).dol

$(BUILD):
	mkdir -p $@

$(TARGET).dol: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

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

# Assets explicites
$(BUILD)/bg_main_png.o: data/bg_main.png
	xxd -i $< > $(BUILD)/bg_main_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/bg_main_png.h

$(BUILD)/cursor_png.o: data/cursor.png
	xxd -i $< > $(BUILD)/cursor_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/cursor_png.h

$(BUILD)/button_start_png.o: data/button_start.png
	xxd -i $< > $(BUILD)/button_start_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/button_start_png.h

$(BUILD)/logo_wiifin_png.o: data/logo_wiifin.png
	xxd -i $< > $(BUILD)/logo_wiifin_png.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/logo_wiifin_png.h

$(BUILD)/wii_font_ttf.o: data/wii_font.ttf
	xxd -i $< > $(BUILD)/wii_font_ttf.h
	$(CC) -x c -c -o $@ -I$(BUILD) $(BUILD)/wii_font_ttf.h

# Inclusion sûre des dépendances
-include $(DEPFILES)

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).dol $(TARGET).map

run: $(TARGET).dol
	wiiload $(TARGET).dol