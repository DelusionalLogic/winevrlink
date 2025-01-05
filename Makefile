OBJDIR := obj
SHARED_SRC_DIR := shared
SHARED_CPP_SRC := $(wildcard $(SHARED_SRC_DIR)/*.cpp)
SHARED_CPP_OBJ := $(patsubst %,%.o, $(SHARED_CPP_SRC))
SHARED_CPP_OBJ_32 := $(patsubst %.o,%.32.o, $(SHARED_CPP_OBJ))

print-%  : ; @echo $* = $($*)

.PHONY: clean
clean:
	@$(RM) -rf obj/

# The native linux driver
RESDIR := vrdriver

DRIVER_SRC_DIR := native
DRIVER_CPP_SRC=$(wildcard $(DRIVER_SRC_DIR)/*.cpp)
DRIVER_CPP_OBJ=$(patsubst %,$(OBJDIR)/%.o, $(DRIVER_CPP_SRC)) ${SHARED_CPP_OBJ:%=$(OBJDIR)/$(DRIVER_SRC_DIR)/%}
DRIVER_CPP_OBJ_32=$(patsubst %,$(OBJDIR)/%.32.o, $(DRIVER_CPP_SRC)) ${SHARED_CPP_OBJ_32:%=$(OBJDIR)/$(DRIVER_SRC_DIR)/%}
-include ${DRIVER_CPP_OBJ:.o=.d}

DRIVER_SO=$(OBJDIR)/vrdriver/bin/linux64/driver_vrdriver.so
DRIVER_SO_32=$(OBJDIR)/vrdriver/bin/linux32/driver_vrdriver.so

CXXFLAGS := -g -O0 -ggdb -MMD -DLINUX -DPOSIX -Ddriver_vrdriver_EXPORTS -fstack-protector-all \
	-I/home/delusional/Documents/vrdriver/vrdriver/lib/openvr/headers \
	-I/home/delusional/Documents/vrdriver/vrdriver/lib/openvr/samples/drivers/utils/driverlog \
	-I/home/delusional/Documents/vrdriver/vrdriver/lib/openvr/samples/drivers/utils/vrmath \
	-iquote$(SHARED_SRC_DIR)
LDFLAGS := -shared -Wl,--no-undefined -fstack-protector-all -g -ggdb -O0

$(DRIVER_SO): $(DRIVER_CPP_OBJ) $(OBJDIR)/util/utils/driverlog/libutil_driverlog.a lib/openvr/bin/linux64/libopenvr_api.a
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $^ -o $@

$(DRIVER_SO_32): $(DRIVER_CPP_OBJ_32) $(OBJDIR)/util.32/utils/driverlog/libutil_driverlog.a lib/openvr/bin/linux32/libopenvr_api.a
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -m32 $^ -o $@

$(OBJDIR)/vrdriver/: $(RESDIR)
	@mkdir -p $(@D)
	cp -r $</* $@

$(OBJDIR)/$(DRIVER_SRC_DIR)/%.cpp.o: $(DRIVER_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -MD -o $@ -c $<

$(OBJDIR)/$(DRIVER_SRC_DIR)/%.cpp.32.o: $(DRIVER_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -m32 -MD -o $@ -c $<

$(OBJDIR)/$(DRIVER_SRC_DIR)/$(SHARED_SRC_DIR)/%.cpp.o: $(SHARED_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -MD -o $@ -c $<

$(OBJDIR)/$(DRIVER_SRC_DIR)/$(SHARED_SRC_DIR)/%.cpp.32.o: $(SHARED_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC -m32 -MD -o $@ -c $<

# Build the driverlog utility
$(OBJDIR)/util/Makefile:
	@mkdir -p $(@D)
	cmake -B $(@D) lib/openvr/samples/drivers
$(OBJDIR)/util/utils/driverlog/libutil_driverlog.a: $(OBJDIR)/util/Makefile
	$(MAKE) -C $(OBJDIR)/util util_driverlog

$(OBJDIR)/util.32/Makefile:
	@mkdir -p $(@D)
	cmake -DCMAKE_C_FLAGS="-m32" -DCMAKE_CXX_FLAGS="-m32 -fPIC" -B $(@D) lib/openvr/samples/drivers
$(OBJDIR)/util.32/utils/driverlog/libutil_driverlog.a: $(OBJDIR)/util.32/Makefile
	$(MAKE) -C $(OBJDIR)/util.32 util_driverlog

# Compile the openvr_api.a library
$(OBJDIR)/openvr/Makefile:
	@mkdir -p $(@D)
	cmake -B $(@D) lib/openvr
# Fucking thing can't compile out of the source tree SMH
lib/openvr/bin/linux64/libopenvr_api.a: $(OBJDIR)/openvr/Makefile
	$(MAKE) -C $(OBJDIR)/openvr openvr_api

$(OBJDIR)/openvr.32/Makefile:
	@mkdir -p $(@D)
	cmake -DCMAKE_CXX_FLAGS="-m32" -DCMAKE_C_FLAGS="-m32" -B $(@D) lib/openvr
# Fucking thing can't compile out of the source tree SMH
lib/openvr/bin/linux32/libopenvr_api.a: $(OBJDIR)/openvr.32/Makefile
	$(MAKE) -C $(OBJDIR)/openvr.32 openvr_api

# The wine host process
WINE_CXX = wineg++
WINE_CC = winegcc

HOST_SRC_DIR := dllhost
HOST_CPP_SRC=$(wildcard $(HOST_SRC_DIR)/*.cpp)
HOST_CPP_OBJ=$(patsubst %,$(OBJDIR)/%.o, $(HOST_CPP_SRC)) ${SHARED_CPP_OBJ:%=$(OBJDIR)/$(HOST_SRC_DIR)/%}
-include ${HOST_CPP_OBJ:.o=.d}

HOST_DLLS = \
			odbc32 \
			ole32 \
			oleaut32 \
			winspool \
			odbccp32
HOST_LIBRARIES = uuid
HOST_LDFLAGS = -g -O0 -ggdb -fno-omit-frame-pointer -fstack-protector
HOST_CXXFLAGS = -O0 -g -ggdb -I$(HOST_SRC_DIR) -MMD -iquote$(SHARED_SRC_DIR) -maccumulate-outgoing-args -march=native -fno-omit-frame-pointer -fstack-protector -fpcc-struct-return
HOST_WIN_CXXFLAGS = -mno-cygwin -fstack-protector

$(OBJDIR)/$(HOST_SRC_DIR)/%.win.cpp.o: $(HOST_SRC_DIR)/%.win.cpp
	@mkdir -p $(@D)
	$(WINE_CXX) -c $(HOST_CXXFLAGS) $(HOST_WIN_CXXFLAGS) -o $@ $<

$(OBJDIR)/$(HOST_SRC_DIR)/%.cpp.o: $(HOST_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(WINE_CXX) -c $(HOST_CXXFLAGS) -o $@ $<

$(OBJDIR)/$(HOST_SRC_DIR)/$(SHARED_SRC_DIR)/%.cpp.o: $(SHARED_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(WINE_CXX) -c $(HOST_CXXFLAGS) -o $@ $<

$(OBJDIR)/windll.exe: $(HOST_CPP_OBJ)
	$(WINE_CXX) $(HOST_LDFLAGS) -o $@ $^ $(HOST_DLLS:%=-l%) $(HOST_LIBRARIES:%=-l%)

# amfrt64.dll
AMFRT_SRC_DIR := amf
AMFRT_CPP_SRC=$(wildcard $(AMFRT_SRC_DIR)/*.cpp)
AMFRT_C_SRC=$(wildcard $(AMFRT_SRC_DIR)/*.c)
AMFRT_CPP_OBJ=$(patsubst %,$(OBJDIR)/%.o, $(AMFRT_CPP_SRC))
AMFRT_C_OBJ=$(patsubst %,$(OBJDIR)/%.o, $(AMFRT_C_SRC))
-include ${AMFRT_CPP_OBJ:.o=.d}
-include ${AMFRT_C_OBJ:.o=.d}

AMFRT_DLLS = \
			odbc32 \
			ole32 \
			oleaut32 \
			winspool \
			odbccp32
AMFRT_LIBRARIES = uuid
AMFRT_LDFLAGS = -g -O0 -ggdb -fno-omit-frame-pointer -fstack-protector -shared -lvulkan -lglfw
AMFRT_CXXFLAGS = -O0 -g -ggdb -I$(AMFRT_SRC_DIR) -I"." -I"./lib/wine/include" -MMD -iquote$(SHARED_SRC_DIR) -maccumulate-outgoing-args -march=native -fno-omit-frame-pointer -fstack-protector
AMFRT_CFLAGS = -O0 -g -ggdb -I$(AMFRT_SRC_DIR) -I"." -I"./lib/wine/include" -MMD -iquote$(SHARED_SRC_DIR) -maccumulate-outgoing-args -march=native -fno-omit-frame-pointer -fstack-protector -fno-short-wchar -lvulkan

$(OBJDIR)/$(AMFRT_SRC_DIR)/%.cpp.o: $(AMFRT_SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(WINE_CXX) -c $(AMFRT_CXXFLAGS) -o $@ $<

$(OBJDIR)/$(AMFRT_SRC_DIR)/%.c.o: $(AMFRT_SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(WINE_CC) -c $(AMFRT_CFLAGS) -o $@ $<

$(OBJDIR)/amfrt64.dll.so: $(AMFRT_CPP_OBJ) $(AMFRT_C_OBJ) $(AMFRT_SRC_DIR)/api.spec
	$(WINE_CXX) $(AMFRT_LDFLAGS) -o $(OBJDIR)/amfrt64.dll $^ $(AMFRT_DLLS:%=-l%) $(AMFRT_LIBRARIES:%=-l%)

# Build wine with a custom winevulkan
WINE_SRC_DIR := lib/wine
WINE_ALL_SRC=$(wildcard $(WINE_SRC_DIR)/**/*)

$(OBJDIR)/wine/configured:
	@mkdir -p "$(@D)/x64"
	@mkdir -p "$(@D)/x32"
	cd "$(@D)/x64" && $(realpath $(WINE_SRC_DIR))/configure --enable-win64
	cd "$(@D)/x32" && PKG_CONFIG_PATH="/usr/lib32/" $(realpath $(WINE_SRC_DIR))/configure --with-wine64="../x64"
	touch "$@"

$(OBJDIR)/wine/built: $(OBJDIR)/wine/configured $(WINE_ALL_SRC)
	$(MAKE) -C "$(@D)/x64"
	$(MAKE) -C "$(@D)/x32"
	touch "$@"

# Build dxvk
DXVK_DIR := lib/dxvk
DXVK_SRC_DIR := $(DXVK_DIR)/src
DXVK_C_SRC=$(wildcard $(DXVK_SRC_DIR)/**/*)

$(OBJDIR)/dxvk/configured:
	@mkdir -p "$(@D)/x64"
	@mkdir -p "$(@D)/x32"
	meson setup --cross-file $(DXVK_DIR)/build-win64.txt --buildtype debug --prefix $(realpath $(OBJDIR))/dxvk/i64 "$(@D)/x64" "$(DXVK_DIR)"
	meson setup --cross-file $(DXVK_DIR)/build-win32.txt --buildtype debug --prefix $(realpath $(OBJDIR))/dxvk/i32 "$(@D)/x32" "$(DXVK_DIR)"
	touch "$@"

$(OBJDIR)/dxvk/built: $(OBJDIR)/dxvk/configured $(DXVK_C_SRC)
	ninja -C $(@D)/x64 install
	ninja -C $(@D)/x32 install
	touch "$@"

.DEFAULT_GOAL:=all
all: $(OBJDIR)/vrdriver/ $(DRIVER_SO) $(OBJDIR)/windll.exe $(OBJDIR)/amfrt64.dll.so $(OBJDIR)/wine/built $(OBJDIR)/dxvk/built
