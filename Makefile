CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra -Iinclude -Isrc
LDLIBS   ?= -pthread

TARGET   := fixclient
SRCS     := $(shell find src -name '*.cpp')
OBJS     := $(patsubst src/%.cpp,build/%.o,$(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
