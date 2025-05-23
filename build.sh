/opt/devkitpro/devkitPPC/bin/powerpc-eabi-g++ \
build/main.o build/App.o build/Input.o build/Menu.o build/UI.o build/JellyfinClient.o \
build/bg_main_png.o build/button_start_png.o build/logo_wiifin_png.o build/wii_font_ttf.o build/cursor_png.o \
-o WiiFin.elf \
-g -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float \
-Wl,-Map,WiiFin.map \
-L/opt/devkitpro/libogc/lib/wii \
-L/opt/devkitpro/portlibs/wii/lib \
-L/opt/devkitpro/portlibs/ppc/lib \
-lgrrlib -lpngu `PKG_CONFIG_LIBDIR=/opt/devkitpro/portlibs/ppc/lib/pkgconfig pkg-config freetype2 libpng libjpeg --libs` \
-lfat -lwiiuse -lbte -logc -lm -lz
make
