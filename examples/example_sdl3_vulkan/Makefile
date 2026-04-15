#
# Cross Platform Makefile
# Compatible with MSYS2/MINGW, Ubuntu 14.04.1 and Mac OS X
#
# You will need SDL3 (http://www.libsdl.org) which is still unreleased/unpackaged.
# Mac OS X:
#   brew install sdl3

#CXX = g++
CXX = /usr/bin/clang++

EXE_VK = example_sdl3_vulkan
EXE_GPU = example_sdl3_sdlgpu3
IMGUI_DIR = ../..
IMGUI_TE_DIR = /home/joao/vscode/imgui_test_engine/imgui_test_engine
APP_DIR = ../../app
VCPKG_DIR    = $(CURDIR)/../../vcpkg_installed/x64-linux
COMMON_APP_SOURCES = $(APP_DIR)/platform/sdl_window.cpp $(APP_DIR)/ui/imgui_console.cpp $(APP_DIR)/ui/test_engine_layer.cpp
COMMON_IMGUI_CORE_SOURCES = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
COMMON_IMGUI_CORE_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp $(IMGUI_DIR)/misc/freetype/imgui_freetype.cpp
COMMON_IMGUI_TE_SOURCES = $(IMGUI_TE_DIR)/imgui_te_engine.cpp $(IMGUI_TE_DIR)/imgui_te_context.cpp $(IMGUI_TE_DIR)/imgui_te_coroutine.cpp
COMMON_IMGUI_TE_SOURCES += $(IMGUI_TE_DIR)/imgui_te_exporters.cpp $(IMGUI_TE_DIR)/imgui_te_perftool.cpp
COMMON_IMGUI_TE_SOURCES += $(IMGUI_TE_DIR)/imgui_te_ui.cpp $(IMGUI_TE_DIR)/imgui_te_utils.cpp
COMMON_IMGUI_TE_SOURCES += $(IMGUI_TE_DIR)/imgui_capture_tool.cpp

SOURCES_VK = main.cpp $(APP_DIR)/renderer/vulkan/vulkan_context.cpp $(APP_DIR)/ui/imgui_layer.cpp
SOURCES_VK += $(COMMON_APP_SOURCES) $(COMMON_IMGUI_CORE_SOURCES) $(COMMON_IMGUI_TE_SOURCES)
SOURCES_VK += $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp

SOURCES_GPU = main_sdlgpu3.cpp $(APP_DIR)/renderer/sdlgpu3/sdlgpu3_context.cpp $(APP_DIR)/ui/imgui_layer_sdlgpu3.cpp
SOURCES_GPU += $(COMMON_APP_SOURCES) $(COMMON_IMGUI_CORE_SOURCES) $(COMMON_IMGUI_TE_SOURCES)
SOURCES_GPU += $(IMGUI_DIR)/backends/imgui_impl_sdlgpu3.cpp

OBJS_VK = $(addsuffix .o, $(basename $(notdir $(SOURCES_VK))))
OBJS_GPU = $(addsuffix .o, $(basename $(notdir $(SOURCES_GPU))))
UNAME_S := $(shell uname -s)

CXXFLAGS = -std=c++23 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMGUI_DIR)/misc/freetype
CXXFLAGS += -I.
CXXFLAGS += -I$(IMGUI_TE_DIR)
CXXFLAGS += -I$(VCPKG_DIR)/include
CXXFLAGS += -g -Wall -Wformat
LIBS     += $(VCPKG_DIR)/lib/libreflectcpp.a $(VCPKG_DIR)/lib/libtomlplusplus.a


##---------------------------------------------------------------------
## BUILD FLAGS PER PLATFORM
##---------------------------------------------------------------------

ifeq ($(UNAME_S), Linux) #LINUX
	ECHO_MESSAGE = "Linux"
	LIBS += -ldl
	LIBS += `pkg-config --libs sdl3 vulkan`
	LIBS += `pkg-config --libs freetype2 plutosvg`

	CXXFLAGS += `pkg-config --cflags sdl3 vulkan`
	CXXFLAGS += `pkg-config --cflags freetype2 plutosvg`
	CFLAGS = $(CXXFLAGS)
endif

ifeq ($(UNAME_S), Darwin) #APPLE
	ECHO_MESSAGE = "Mac OS X"
	LIBS += -framework Cocoa -framework IOKit -framework CoreVideo
	LIBS += `pkg-config --libs sdl3 vulkan`
	LIBS += -L/usr/local/lib -L/opt/local/lib

	CXXFLAGS += `pkg-config --cflags sdl3 vulkan`
	CXXFLAGS += -I/usr/local/include -I/opt/local/include
	CFLAGS = $(CXXFLAGS)
endif

ifeq ($(OS), Windows_NT)
	ECHO_MESSAGE = "MinGW"
	LIBS += -lgdi32 -limm32 `pkg-config --static --libs sdl3 vulkan`

	CXXFLAGS += `pkg-config --cflags sdl3 vulkan`
	CFLAGS = $(CXXFLAGS)
endif

##---------------------------------------------------------------------
## BUILD RULES
##---------------------------------------------------------------------

%.o:%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(APP_DIR)/ui/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(APP_DIR)/platform/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(APP_DIR)/renderer/vulkan/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(APP_DIR)/renderer/sdlgpu3/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_DIR)/backends/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_DIR)/misc/freetype/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_TE_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

all: $(EXE_VK) $(EXE_GPU)
	@echo Build complete for $(ECHO_MESSAGE)


$(EXE_VK): $(OBJS_VK)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS)

$(EXE_GPU): $(OBJS_GPU)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS)

clean:
	rm -f $(EXE_VK) $(EXE_GPU) $(OBJS_VK) $(OBJS_GPU)
