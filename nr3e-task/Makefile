CXX ?= aarch64-linux-android-clang++
CC  ?= aarch64-linux-android-clang
STRIP ?= aarch64-linux-android-strip

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT  := $(ROOT)/outputs/arm64-v8a
SO   := $(OUT)/libnr3e.so
HUD  := $(OUT)/nr3e_hud
IMGUI := $(ROOT)/third_party/imgui
ANWC := $(ROOT)/third_party/android_surface
SYS  := /system/lib64

CXXFLAGS := -std=c++20 -O2 -g0 -fPIC -fvisibility=hidden -fno-ident \
	-ffunction-sections -fdata-sections -w \
	-D__ANDROID__ -DANDROID -DNDEBUG -D_GNU_SOURCE -DNR3E_SILENT \
	-DIMGUI_IMPL_OPENGL_ES3

INCLUDES := -I$(ROOT)/include -I$(IMGUI) -I$(IMGUI)/backends -I$(ANWC)

SO_LDFLAGS := -shared -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all \
	-Wl,-z,max-page-size=16384 -llog -ldl -lm

HUD_LDFLAGS := -fPIE -pie -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all \
	-Wl,-z,max-page-size=16384 \
	-Wl,-rpath,/system/lib64 -Wl,-rpath,/vendor/lib64 -Wl,--enable-new-dtags \
	-llog -ldl -lm \
	$(SYS)/libandroid.so $(SYS)/libEGL.so $(SYS)/libGLESv3.so

SO_SRCS := \
	$(ROOT)/src/main.cpp \
	$(ROOT)/src/ue.cpp \
	$(ROOT)/src/task.cpp \
	$(ROOT)/src/ipc.cpp \
	$(ROOT)/src/hide.cpp

HUD_SRCS := \
	$(ROOT)/hud/main.cpp \
	$(ROOT)/hud/overlay.cpp \
	$(ROOT)/hud/client.cpp \
	$(ROOT)/hud/egl_ctx.cpp \
	$(ROOT)/hud/touch.cpp \
	$(ROOT)/hud/surface.cpp \
	$(ROOT)/hud/anwc_globals.cpp \
	$(ROOT)/hud/font.cpp \
	$(ROOT)/hud/display.cpp

IMGUI_SRCS := \
	$(IMGUI)/imgui.cpp \
	$(IMGUI)/imgui_draw.cpp \
	$(IMGUI)/imgui_tables.cpp \
	$(IMGUI)/imgui_widgets.cpp \
	$(IMGUI)/backends/imgui_impl_opengl3.cpp

SO_OBJS := $(SO_SRCS:$(ROOT)/%.cpp=$(OUT)/obj/%.o)
HUD_OBJS := $(HUD_SRCS:$(ROOT)/%.cpp=$(OUT)/obj/%.o)
IMGUI_OBJS := \
	$(OUT)/obj/imgui/imgui.o \
	$(OUT)/obj/imgui/imgui_draw.o \
	$(OUT)/obj/imgui/imgui_tables.o \
	$(OUT)/obj/imgui/imgui_widgets.o \
	$(OUT)/obj/imgui/imgui_impl_opengl3.o

NR3E := $(OUT)/nr3e
STEALTH_KPM ?= $(ROOT)/prebuilt/stealth.kpm
KPATCH_BIN  ?= $(ROOT)/prebuilt/kpatch
BLOB_DIR    := $(OUT)/obj/blobs

INJECT_CFLAGS := -std=gnu11 -O2 -g0 -fPIE -fvisibility=hidden -fno-ident \
	-ffunction-sections -fdata-sections -w -D__ANDROID__ -DANDROID -DNDEBUG -D_GNU_SOURCE

INJECT_LDFLAGS := -fPIE -pie -Wl,--gc-sections -Wl,--strip-all \
	-Wl,-z,max-page-size=16384 -ldl

PACK_OBJ := $(OUT)/obj/inject/pack.o
NR3E_OBJ := $(OUT)/obj/inject/nr3e.o
STEALTH_OBJ := $(OUT)/obj/inject/stealth_inject_main.o
BLOB_OBJS := \
	$(BLOB_DIR)/so.bin.o \
	$(BLOB_DIR)/hud.bin.o \
	$(BLOB_DIR)/stealth.bin.o \
	$(BLOB_DIR)/kpatch.bin.o

.PHONY: all clean install

all: $(SO) $(HUD) $(NR3E)

$(SO): $(SO_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(SO_LDFLAGS) -o $@ $^
	$(STRIP) --strip-all $@ 2>/dev/null || true
	@echo "built $@"

CJK_TTF := $(ROOT)/data/cjk.ttf
CJK_OBJ := $(OUT)/obj/cjk_ttf.o

$(HUD): $(HUD_OBJS) $(IMGUI_OBJS) $(CJK_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(HUD_LDFLAGS) -o $@ $^
	python3 $(ROOT)/tools/fix_rpath.py $@ || true
	$(STRIP) --strip-all $@ 2>/dev/null || true
	@echo "built $@"

$(OUT)/obj/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OUT)/obj/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OUT)/obj/imgui/imgui_impl_opengl3.o: $(IMGUI)/backends/imgui_impl_opengl3.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(CJK_OBJ): $(CJK_TTF)
	@mkdir -p $(dir $@)
	cp -f $(CJK_TTF) $(OUT)/obj/cjk.ttf
	cd $(OUT)/obj && llvm-objcopy -I binary -O elf64-littleaarch64 -B aarch64 cjk.ttf $(notdir $@)

$(BLOB_DIR)/so.bin: $(SO)
	@mkdir -p $(BLOB_DIR)
	python3 $(ROOT)/inject/mkblob.py $< $@

$(BLOB_DIR)/hud.bin: $(HUD)
	@mkdir -p $(BLOB_DIR)
	python3 $(ROOT)/inject/mkblob.py $< $@

$(BLOB_DIR)/stealth.bin: $(STEALTH_KPM)
	@mkdir -p $(BLOB_DIR)
	python3 $(ROOT)/inject/mkblob.py $< $@

$(BLOB_DIR)/kpatch.bin: $(KPATCH_BIN)
	@mkdir -p $(BLOB_DIR)
	python3 $(ROOT)/inject/mkblob.py $< $@

$(BLOB_DIR)/%.bin.o: $(BLOB_DIR)/%.bin
	cd $(BLOB_DIR) && llvm-objcopy -I binary -O elf64-littleaarch64 -B aarch64 $(notdir $<) $(notdir $@)

$(OUT)/obj/inject/%.o: $(ROOT)/inject/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INJECT_CFLAGS) -I$(ROOT)/inject -I$(ROOT)/include -c $< -o $@

$(STEALTH_OBJ): $(ROOT)/inject/stealth_inject.c $(ROOT)/inject/stealth_inject.h $(ROOT)/inject/kload_api.h
	@mkdir -p $(dir $@)
	$(CC) $(INJECT_CFLAGS) -Dmain=stealth_inject_main -I$(ROOT)/inject -c $< -o $@

$(NR3E): $(NR3E_OBJ) $(PACK_OBJ) $(STEALTH_OBJ) $(BLOB_OBJS)
	$(CC) $(INJECT_LDFLAGS) -o $@ $^
	$(STRIP) --strip-all $@ 2>/dev/null || true
	@echo "built $@"

install: $(NR3E)
	@echo "copy single ELF with Magisk su"
	/system/bin/su -c 'cp -f $(NR3E) /data/local/tmp/nr3e && chmod 755 /data/local/tmp/nr3e'
	@echo "installed /data/local/tmp/nr3e"

clean:
	rm -rf $(OUT)
