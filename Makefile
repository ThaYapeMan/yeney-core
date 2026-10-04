# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

CXX ?= g++
AR ?= ar
CPPFLAGS += -I.
CXXFLAGS ?= -g -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread -MMD -MP
LDLIBS += -pthread -lFLAC

ALAC_C = EndianPortable ALACBitUtilities ag_dec dp_dec matrix_dec
ALAC_OBJS = $(addprefix build/alac/,$(addsuffix .o,$(ALAC_C))) build/alac/ALACDecoder.o
CORE_OBJS = core/transitions.o core/protocol.o core/player.o core/decoder.o core/mp4.o $(ALAC_OBJS)
APP_OBJS = apps/yeney-player/main.o apps/yeney-player/sinks.o sinks/shm_v1/sink.o
TEST_OBJS = tests/clock_unit.o  tests/shm_unit.o tests/unit.o tests/fixture.o tests/decoder_fixture.o

all: libyeneycore.a yeney-player

libyeneycore.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

yeney-player: $(APP_OBJS) libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(APP_OBJS) libyeneycore.a $(LDLIBS)

unit-test: tests/unit.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test-player: tests/fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/alac/%.o: third_party/alac/codec/%.c
	mkdir -p build/alac
	$(CC) -O2 -Wno-multichar -Ithird_party/alac/codec -c -o $@ $<

build/alac/ALACDecoder.o: third_party/alac/codec/ALACDecoder.cpp
	mkdir -p build/alac
	$(CXX) -O2 -Wno-multichar -Ithird_party/alac/codec -c -o $@ $<

$(ALAC_OBJS): $(wildcard third_party/alac/codec/*.h)

core/decoder.o: CPPFLAGS += -Wno-multichar

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

decoder-test: tests/decoder_fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

demux-sanitized: tests/demux_fixture.cpp core/mp4.cpp core/protocol.cpp core/mp4.h
	$(CXX) $(CPPFLAGS) -std=c++17 -g -O1 -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address,undefined -o $@ tests/demux_fixture.cpp core/mp4.cpp core/protocol.cpp

shm-unit: tests/shm_unit.o sinks/shm_v1/sink.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -pthread -Wl,--wrap=getrandom -Wl,--wrap=open

clock-unit: tests/clock_unit.o sinks/shm_v1/sink.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -pthread

test: format-check clock-unit shm-unit all unit-test test-player decoder-test demux-sanitized
	./clock-unit
	./unit-test
	./shm-unit
	python3 tests/fake_lms.py
	python3 tests/decoders_test.py
	python3 tests/device_script_test.py
	python3 tests/shm_test.py
	python3 tests/transitions_test.py

clean:
	rm -f $(CORE_OBJS) $(APP_OBJS) $(TEST_OBJS) $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d) libyeneycore.a yeney-player clock-unit unit-test test-player decoder-test demux-sanitized shm-unit decoder-benchmark decoder-benchmark.d

-include $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
.PHONY: all clean test format format-check

OWN_SOURCES = $(shell find core apps sinks tests -type f \( -name "*.cpp" -o -name "*.h" \))
CLANG_FORMAT ?= clang-format
format:
	$(CLANG_FORMAT) -i $(OWN_SOURCES)
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(OWN_SOURCES)

decoder-benchmark: tests/decoder_benchmark.cpp libyeneycore.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
