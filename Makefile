CXX ?= g++
AR ?= ar
CPPFLAGS += -I.
CXXFLAGS ?= -g -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread -MMD -MP
LDLIBS += -pthread

CORE_OBJS = core/protocol.o core/player.o
APP_OBJS = apps/yeney-player/main.o apps/yeney-player/sinks.o
TEST_OBJS = tests/unit.o tests/fixture.o

all: libyeneycore.a yeney-player

libyeneycore.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

yeney-player: $(APP_OBJS) libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $(APP_OBJS) libyeneycore.a $(LDLIBS)

unit-test: tests/unit.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test-player: tests/fixture.o libyeneycore.a
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test: all unit-test test-player
	./unit-test
	python3 tests/fake_lms.py

clean:
	rm -f $(CORE_OBJS) $(APP_OBJS) $(TEST_OBJS) $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d) libyeneycore.a yeney-player unit-test test-player

-include $(CORE_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
.PHONY: all clean test
