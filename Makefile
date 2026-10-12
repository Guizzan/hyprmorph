CXXFLAGS ?= -O2
DEPS = pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon hyprutils hyprlang aquamarine hyprgraphics egl glesv2
INC  = -I/usr/include/hyprland -I/usr/include/hyprland/protocols -I/usr/include/hyprland/src
OUT ?= hyprmorph-dev.so

# Never write over a .so a running Hyprland has loaded: build to $(OUT), then
# `make install` swaps it in with a new inode (rm + copy).
$(OUT): main.cpp
	$(CXX) -shared -fPIC --std=c++2c $(CXXFLAGS) $(INC) $$(pkg-config --cflags $(DEPS)) main.cpp -o $@ -lGLESv2

install: $(OUT)
	rm -f hyprmorph.so && cp $(OUT) hyprmorph.so

clean:
	rm -f hyprmorph-dev.so
