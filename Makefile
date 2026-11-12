CXXFLAGS ?= -O3
BUILD_DIR ?= .build
TARGET ?= cam_viewer

objects := $(addprefix $(BUILD_DIR)/,main.o camera.o display.o platform.o touch.o)
test_objects := $(addprefix $(BUILD_DIR)/,touch.o touch_test.o)
test_target := $(BUILD_DIR)/touch_test

.PHONY: all test clean
all: $(TARGET)

$(TARGET): $(objects)
	$(CXX) $(LDFLAGS) -o "$@" $^ $(LDLIBS) -ljpeg -lpng

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -D_FILE_OFFSET_BITS=64 $(CXXFLAGS) -std=c++17 -Wall -Wextra -Wpedantic -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR):
	mkdir -p "$@"

test: $(test_target)
	$(RUNNER) "$(test_target)"

$(test_target): $(test_objects)
	$(CXX) $(LDFLAGS) -o "$@" $^ -Wl,--wrap=open,--wrap=open64,--wrap=__open_2,--wrap=__open64_2,--wrap=ioctl

clean:
	$(RM) "$(TARGET)" "$(test_target)" $(sort $(objects) $(test_objects)) $(sort $(objects:.o=.d) $(test_objects:.o=.d))

-include $(sort $(objects:.o=.d) $(test_objects:.o=.d))
