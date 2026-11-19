# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export TMPDIR ?= /var/tmp

CXX ?= g++
AR ?= ar
CPPFLAGS += -I.
CXXFLAGS ?= -g -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread -lFLAC

ALAC_C = EndianPortable ALACBitUtilities ag_dec dp_dec matrix_dec
ALAC_OBJS = $(addprefix build/alac/,$(addsuffix .o,$(ALAC_C))) build/alac/ALACDecoder.o
CORE_OBJS = core/decimator.o core/decimating_decoder.o core/transitions.o core/protocol.o core/player.o core/queue_mode.o core/queue_decode.o core/decoder.o core/mp4.o $(ALAC_OBJS)
APP_OBJS = apps/yeney-player/main.o apps/yeney-player/sinks.o sinks/shm_v1/sink.o
TEST_OBJS = tests/decimator_fixture.o tests/decimator_unit.o tests/speaker_fixture.o tests/clock_unit.o  tests/shm_unit.o tests/unit.o tests/fixture.o tests/decoder_fixture.o

comma := ,
DIRECT_PROGRAMS = demux-sanitized decoder-benchmark content-position-test mp4-position-test pcm-origin-test test-shm-player mp3-origin-test
DEP_TARGETS = $(CORE_OBJS) $(APP_OBJS) $(TEST_OBJS) $(DIRECT_PROGRAMS)
DEPS = $(addsuffix .d,$(DEP_TARGETS))

all: libyeneycore.a yeney-player

.DELETE_ON_ERROR:
# A single compile-and-link command can overwrite its dependency output for
# each source. Collect all its translation units in a separate compiler pass.
define compile-program
$(CXX) $(filter-out -o $@ -l% %.o %.a,$1) -MM -MP -MT '$@' > $@.d.tmp
$(CXX) $1
mv $@.d.tmp $@.d
endef

libyeneycore.a: $(CORE_OBJS)
	$(AR) rcs $@ $(CORE_OBJS)

yeney-player: $(APP_OBJS) libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(APP_OBJS) libyeneycore.a $(LDLIBS)

unit-test: tests/unit.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)

test-player: tests/fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)

build/alac/%.o: third_party/alac/codec/%.c
	mkdir -p build/alac
	$(CC) -O2 -Wno-multichar -Ithird_party/alac/codec -MMD -MP -MF $@.d -c -o $@ $<

build/alac/ALACDecoder.o: third_party/alac/codec/ALACDecoder.cpp
	mkdir -p build/alac
	$(CXX) -O2 -Wno-multichar -Ithird_party/alac/codec -MMD -MP -MF $@.d -c -o $@ $<

core/decoder.o: private CPPFLAGS += -Wno-multichar

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

decoder-test: tests/decoder_fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)

demux-sanitized: tests/demux_fixture.cpp core/mp4.cpp core/protocol.cpp
	$(call compile-program,$(CPPFLAGS) -std=c++17 -g -O1 -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address$(comma)undefined -o $@ tests/demux_fixture.cpp core/mp4.cpp core/protocol.cpp)

shm-unit: tests/shm_unit.o sinks/shm_v1/sink.o
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) -pthread -Wl,--wrap=getrandom -Wl,--wrap=open

clock-unit: tests/clock_unit.o sinks/shm_v1/sink.o
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) -pthread

test: decimator-fixture mp3-origin-test test-shm-player pcm-origin-test test-speaker format-check clock-unit shm-unit all unit-test test-player decoder-test demux-sanitized
	./decimator-unit
	python3 tests/decimator_cpu_test.py
	python3 tests/content_position_test.py
	python3 tests/mp3_origin_test.py
	python3 tests/mp4_prefetch_test.py
	python3 tests/mp4_seek_test.py
	python3 tests/pcm_origin_test.py
	./clock-unit
	./unit-test
	./shm-unit
	python3 tests/fake_lms.py
	python3 tests/decoders_test.py
	python3 tests/length_test.py
	python3 tests/device_script_test.py
	python3 tests/shm_test.py
	python3 tests/transitions_test.py
	python3 tests/queue_test.py
	python3 tests/speaker_test.py
	python3 tests/status_request_test.py
	python3 tests/queue_protocol_test.py

clean:
	rm -f $(DEPS) $(addsuffix .tmp,$(DEPS)) build/flags build/flags.tmp
	rm -f mp3-origin-test mp3-origin-test.d pcm-origin-test pcm-origin-test.d test-shm-player test-shm-player.d decimator-fixture decimator-unit tests/decimator_fixture.o tests/decimator_fixture.d tests/decimator_unit.o tests/decimator_unit.d test-speaker $(CORE_OBJS) $(APP_OBJS) $(TEST_OBJS) $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d) libyeneycore.a yeney-player clock-unit unit-test test-player decoder-test demux-sanitized shm-unit content-position-test content-position-test.d decoder-benchmark decoder-benchmark.d mp4-position-test mp4-position-test.d

.PHONY: all clean test format format-check

OWN_SOURCES = $(shell find core apps sinks tests -type f \( -name "*.cpp" -o -name "*.h" \))
CLANG_FORMAT ?= clang-format
format:
	$(CLANG_FORMAT) -i $(OWN_SOURCES)
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(OWN_SOURCES)

decoder-benchmark: tests/decoder_benchmark.cpp libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o %.a,$^) $(LDLIBS))

test-speaker: tests/speaker_fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)

decimator-fixture: tests/decimator_fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)

decimator-unit: tests/decimator_unit.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.o %.a,$^) $(LDLIBS)
test: decimator-unit

content-position-test: tests/content_position.cpp sinks/shm_v1/sink.o libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o %.a,$^) $(LDLIBS))
test: content-position-test

mp4-position-test: tests/mp4_position.cpp libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o %.a,$^) $(LDLIBS))
test: mp4-position-test

pcm-origin-test: tests/pcm_origin.cpp libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o %.a,$^) $(LDLIBS))


test-shm-player: tests/shm_player.cpp apps/yeney-player/main.cpp apps/yeney-player/sinks.o sinks/shm_v1/sink.o libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -DYENEY_PLAYER_LIBRARY -o $@ tests/shm_player.cpp apps/yeney-player/main.cpp apps/yeney-player/sinks.o sinks/shm_v1/sink.o libyeneycore.a $(LDLIBS))


mp3-origin-test: tests/mp3_origin.cpp libyeneycore.a
	$(call compile-program,$(CPPFLAGS) $(CXXFLAGS) -DYENEY_MP3_ORIGIN_API -o $@ $(filter %.cpp %.o %.a,$^) $(LDLIBS))

.PHONY: FORCE
FORCE:

quote = '$(subst ','"'"',$1)'
core_revision := $(shell git rev-parse HEAD)
build_flags := $(call quote,$(CC)) $(call quote,$(CXX)) $(call quote,$(CPPFLAGS)) \
    $(call quote,$(CXXFLAGS)) $(call quote,$(AR)) $(call quote,$(LDLIBS)) $(core_revision)
ifneq ($(shell printf '%s\n' $(build_flags) | cmp -s - build/flags && printf current),current)
build/flags: FORCE
endif
build/flags:
	@mkdir -p $(@D)
	@printf '%s\n' $(build_flags) > $@.tmp
	@mv -f $@.tmp $@

$(DEP_TARGETS) libyeneycore.a yeney-player unit-test test-player decoder-test shm-unit clock-unit test-speaker decimator-fixture decimator-unit: Makefile build/flags
$(patsubst %.d,%,$(filter-out $(wildcard $(DEPS)),$(DEPS))): FORCE

-include $(DEPS)
