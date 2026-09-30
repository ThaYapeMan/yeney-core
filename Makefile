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
CORE_OBJS = core/protocol.o core/player.o core/decoder.o core/mp4.o $(ALAC_OBJS)
APP_OBJS = apps/yeney-player/main.o apps/yeney-player/sinks.o
TEST_OBJS = tests/unit.o tests/fixture.o tests/decoder_fixture.o

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

test: format-check all unit-test test-player decoder-test demux-sanitized
	./unit-test
	python3 tests/fake_lms.py
	python3 tests/decoders_test.py

clean:
	rm -f $(CORE_OBJS) $(APP_OBJS) $(TEST_OBJS) $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d) libyeneycore.a yeney-player unit-test test-player decoder-test demux-sanitized

-include $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
.PHONY: all clean test format format-check

OWN_SOURCES = $(shell find core apps tests -type f \( -name "*.cpp" -o -name "*.h" \))
CLANG_FORMAT ?= clang-format
format:
	$(CLANG_FORMAT) -i $(OWN_SOURCES)
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(OWN_SOURCES)
