# Host (macOS) build of the engine: a static library, the headless test host
# and the small test tools. The iOS/Mac app compiles the same sources through
# app/project.yml.
#
#   make            build/host/orehost and build/host/libore.a
#   make tests      build the test tools
#   make clean

BUILD   := ../build/host
CXX     := clang++
CC      := clang
CXXFLAGS:= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc -Ithird_party
CFLAGS  := -O2 -g -w -Ithird_party
FRAMEWORKS := -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework Foundation

CPP_SRCS := $(shell find src -name '*.cpp')
MM_SRCS  := $(shell find src -name '*.mm')
C_SRCS   := $(shell find src -name '*.c')
OBJS := $(CPP_SRCS:%.cpp=$(BUILD)/obj/%.o) $(MM_SRCS:%.mm=$(BUILD)/obj/%.o) $(C_SRCS:%.c=$(BUILD)/obj/%.o)
DEPS := $(OBJS:.o=.d)

all: $(BUILD)/orehost

$(BUILD)/libore.a: $(OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/host/stb_write.o: tests/stb_write_impl.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -w -Ithird_party -c $< -o $@

$(BUILD)/orehost: host/main.mm $(BUILD)/libore.a $(BUILD)/obj/host/stb_write.o
	$(CXX) $(CXXFLAGS) -fobjc-arc host/main.mm $(BUILD)/obj/host/stb_write.o $(BUILD)/libore.a $(FRAMEWORKS) -o $@

tests: $(BUILD)/elg_all $(BUILD)/vm_smoke $(BUILD)/movie_test $(BUILD)/movie_flow $(BUILD)/save_roundtrip $(BUILD)/quick_save $(BUILD)/command_input $(BUILD)/image_blend $(BUILD)/native_coverage

$(BUILD)/elg_all: tests/elg_all.cpp $(BUILD)/libore.a $(BUILD)/obj/host/stb_write.o
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/vm_smoke: tests/vm_smoke.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/movie_test: tests/movie_test.cpp $(BUILD)/libore.a $(BUILD)/obj/host/stb_write.o
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/movie_flow: tests/movie_flow.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/save_roundtrip: tests/save_roundtrip.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/quick_save: tests/quick_save.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/command_input: tests/command_input.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/image_blend: tests/image_blend.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

$(BUILD)/native_coverage: tests/native_coverage.cpp $(BUILD)/libore.a
	$(CXX) $(CXXFLAGS) $^ $(FRAMEWORKS) -o $@

clean:
	rm -rf $(BUILD)/obj $(BUILD)/libore.a $(BUILD)/orehost

-include $(DEPS)

.PHONY: all tests clean
