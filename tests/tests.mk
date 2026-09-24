# Build/run/coverage rules for the pacemaker_sim characterization test
# suite (Verification & Test Agent output).
#
# NEW file - does not modify the top-level Makefile or any src/ file.
# Named tests.mk rather than Makefile so it stays out of the top-level
# build's discovery path (opt-in, not part of `make all` at project
# root).
#
# Each test binary links the real, unmodified src/*.c it needs
# (excluding main.c, which supplies its own main()) against its own
# test_*.c main(), so tests exercise actual linked behavior rather than
# mocks or stubs.
#
# Usage:
#   make -C tests -f tests.mk            # build everything
#   make -C tests -f tests.mk run        # build + run all suites
#   make -C tests -f tests.mk coverage   # instrumented build + gcov report
#                                         #   for every covered module
#   make -C tests -f tests.mk clean

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -std=c99 -I../include -g
LDFLAGS ?=

COMMON_SRC := ../src/pacer_core.c \
              ../src/sensing.c \
              ../src/modes.c \
              ../src/telemetry.c \
              ../src/battery.c \
              ../src/eeprom.c \
              ../src/arrhythmia.c

EEPROM_SRC     := $(COMMON_SRC) test_eeprom.c
TELEMETRY_SRC  := $(COMMON_SRC) test_telemetry.c
ARRHYTHMIA_SRC := $(COMMON_SRC) test_arrhythmia.c
MODES_SRC      := $(COMMON_SRC) test_modes.c
# pacer_core.c is already part of COMMON_SRC (every other suite links the
# real state machine too); its own suite additionally needs fake_time.c
# because pacer_core_init() unconditionally calls sensing_init() and
# battery_init(), both of which seed a time(NULL)-based LCG.
PACER_CORE_SRC := $(COMMON_SRC) fake_time.c test_pacer_core.c
# main.c defines its OWN global main(), so it can never be linked
# alongside a test-driver main() - it is not part of COMMON_SRC and has
# no test_main.c. Its suite (test_main.sh) is a black-box shell script
# that drives this instrumented, standalone copy of the REAL, unmodified
# full 8-file program (the exact same sources the top-level Makefile
# builds bin/pacemaker_sim from) as a subprocess, rather than a linked
# C harness.
MAIN_SRC       := ../src/main.c $(COMMON_SRC)
# sensing.c and battery.c suites additionally link fake_time.c, the
# test-only link-time interposition of time() that makes their
# time(NULL)-seeded LCGs deterministic without modifying either source
# file. See fake_time.c's header comment. MUST NOT be linked into the
# real bin/pacemaker_sim build (it never is - that build uses the
# top-level Makefile, not this one).
SENSING_SRC    := $(COMMON_SRC) fake_time.c test_sensing.c
BATTERY_SRC    := $(COMMON_SRC) fake_time.c test_battery.c

EEPROM_BIN     := test_eeprom
TELEMETRY_BIN  := test_telemetry
ARRHYTHMIA_BIN := test_arrhythmia
SENSING_BIN    := test_sensing
BATTERY_BIN    := test_battery
MODES_BIN      := test_modes
PACER_CORE_BIN := test_pacer_core
MAIN_BIN       := pacemaker_sim_cov

.PHONY: all run coverage clean

all: $(EEPROM_BIN) $(TELEMETRY_BIN) $(ARRHYTHMIA_BIN) $(SENSING_BIN) $(BATTERY_BIN) $(MODES_BIN) $(PACER_CORE_BIN) $(MAIN_BIN)

$(EEPROM_BIN): $(EEPROM_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(EEPROM_SRC) $(LDFLAGS)

$(TELEMETRY_BIN): $(TELEMETRY_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(TELEMETRY_SRC) $(LDFLAGS)

$(ARRHYTHMIA_BIN): $(ARRHYTHMIA_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(ARRHYTHMIA_SRC) $(LDFLAGS)

$(MODES_BIN): $(MODES_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(MODES_SRC) $(LDFLAGS)

$(PACER_CORE_BIN): $(PACER_CORE_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(PACER_CORE_SRC) $(LDFLAGS)

$(SENSING_BIN): $(SENSING_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(SENSING_SRC) $(LDFLAGS)

$(BATTERY_BIN): $(BATTERY_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(BATTERY_SRC) $(LDFLAGS)

# built WITHOUT -fprofile-arcs here (that's only added in the `coverage`
# target below) - `run` just needs a plain, fast binary for test_main.sh
# to drive as a subprocess.
$(MAIN_BIN): $(MAIN_SRC) ../include/pacer.h
	$(CC) $(CFLAGS) -o $@ $(MAIN_SRC) $(LDFLAGS)

run: all
	@echo "############################################"
	@echo "# eeprom.c suite"
	@echo "############################################"
	./$(EEPROM_BIN)
	@echo ""
	@echo "############################################"
	@echo "# telemetry.c suite"
	@echo "############################################"
	./$(TELEMETRY_BIN)
	@echo ""
	@echo "############################################"
	@echo "# arrhythmia.c suite"
	@echo "############################################"
	./$(ARRHYTHMIA_BIN)
	@echo ""
	@echo "############################################"
	@echo "# sensing.c suite (deterministic via fake_time.c)"
	@echo "############################################"
	./$(SENSING_BIN)
	@echo ""
	@echo "############################################"
	@echo "# battery.c suite (deterministic via fake_time.c)"
	@echo "############################################"
	./$(BATTERY_BIN)
	@echo ""
	@echo "############################################"
	@echo "# modes.c suite"
	@echo "############################################"
	./$(MODES_BIN)
	@echo ""
	@echo "############################################"
	@echo "# pacer_core.c suite (deterministic via fake_time.c + threshold-widening technique)"
	@echo "############################################"
	./$(PACER_CORE_BIN)
	@echo ""
	@echo "############################################"
	@echo "# main.c suite (black-box, subprocess - see test_main.sh)"
	@echo "############################################"
	./test_main.sh

# Instrumented builds + real, tool-derived line coverage (gcov) for each
# pilot module, so reported coverage % is measured, not asserted. Each
# module's gcov summary is printed immediately after ITS OWN run, before
# the next module's "rm -f *.gcda *.gcno" clears the shared-glob
# coverage-data files (they are not otherwise module-scoped).
coverage:
	rm -f *.gcda *.gcno *.gcov
	@echo "=== Coverage summary (gcov, lines executed) ==="
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(EEPROM_BIN)_cov $(EEPROM_SRC) $(LDFLAGS) -lgcov
	./$(EEPROM_BIN)_cov >/dev/null
	@echo "eeprom.c:" && gcov -o . $(EEPROM_BIN)_cov-eeprom 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(TELEMETRY_BIN)_cov $(TELEMETRY_SRC) $(LDFLAGS) -lgcov
	./$(TELEMETRY_BIN)_cov >/dev/null
	@echo "telemetry.c:" && gcov -o . $(TELEMETRY_BIN)_cov-telemetry 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(ARRHYTHMIA_BIN)_cov $(ARRHYTHMIA_SRC) $(LDFLAGS) -lgcov
	./$(ARRHYTHMIA_BIN)_cov >/dev/null
	@echo "arrhythmia.c:" && gcov -o . $(ARRHYTHMIA_BIN)_cov-arrhythmia 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(SENSING_BIN)_cov $(SENSING_SRC) $(LDFLAGS) -lgcov
	./$(SENSING_BIN)_cov >/dev/null
	@echo "sensing.c:" && gcov -o . $(SENSING_BIN)_cov-sensing 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(BATTERY_BIN)_cov $(BATTERY_SRC) $(LDFLAGS) -lgcov
	./$(BATTERY_BIN)_cov >/dev/null
	@echo "battery.c:" && gcov -o . $(BATTERY_BIN)_cov-battery 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(MODES_BIN)_cov $(MODES_SRC) $(LDFLAGS) -lgcov
	./$(MODES_BIN)_cov >/dev/null
	@echo "modes.c:" && gcov -o . $(MODES_BIN)_cov-modes 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(PACER_CORE_BIN)_cov $(PACER_CORE_SRC) $(LDFLAGS) -lgcov
	./$(PACER_CORE_BIN)_cov >/dev/null
	@echo "pacer_core.c:" && gcov -o . $(PACER_CORE_BIN)_cov-pacer_core 2>/dev/null | grep "Lines executed"
	rm -f *.gcda *.gcno pacer_nvram.bin
	$(CC) $(CFLAGS) -fprofile-arcs -ftest-coverage -o $(MAIN_BIN) $(MAIN_SRC) $(LDFLAGS) -lgcov
	./test_main.sh >/dev/null
	@echo "main.c:" && gcov -o . $(MAIN_BIN)-main 2>/dev/null | grep "Lines executed"

clean:
	rm -rf $(EEPROM_BIN) $(TELEMETRY_BIN) $(ARRHYTHMIA_BIN) $(SENSING_BIN) $(BATTERY_BIN) $(MODES_BIN) $(PACER_CORE_BIN) $(MAIN_BIN) \
	       $(EEPROM_BIN)_cov $(TELEMETRY_BIN)_cov $(ARRHYTHMIA_BIN)_cov $(SENSING_BIN)_cov $(BATTERY_BIN)_cov $(MODES_BIN)_cov $(PACER_CORE_BIN)_cov \
	       *.o *.gcda *.gcno *.gcov pacer_nvram.bin telemetry_capture_tmp.txt
