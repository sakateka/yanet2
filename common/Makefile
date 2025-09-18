# Optimization level control via environment variables
# Usage examples:
#   make                    # Uses default -O0 (debug)
#   OPT=-O2 make           # Uses -O2 optimization
#   OPT=-O3 make           # Uses -O3 optimization
#   OPT=-Os make           # Uses size optimization
#   CFLAGS="-O2 -DNDEBUG" make  # Full control via CFLAGS
OPT ?= -O2
CFLAGS ?= -g -Wall -Wextra $(OPT) -march=corei7 -pthread

.PHONY: ttl-test ttl-test-build ttl-debug test test-build debug clean

test: test-build
	@echo "\033[0;32m[TEST] Running comprehensive Furry Map test suite\033[0m"
	../build/test_furrymap
	@echo "\033[0;32m[TEST] All tests passed successfully!\033[0m"

test-build:
	@echo "\033[0;35m[BUILD] Compiling comprehensive test_furrymap ($(OPT))\033[0m"
	$(CC) $(CFLAGS) -o ../build/test_furrymap test_furrymap.c $(LDFLAGS)

debug: test-build
	@echo "\033[0;33m[DEBUG] Debugging test_furrymap with gdb\033[0m"
	timeout -s SIGINT 5 gdb -batch -ex "set confirm off" -ex "br exit" -ex run -ex bt -ex quit --args ../build/test_furrymap

clean:
	@echo "\033[0;31m[CLEAN] Removing build artifacts\033[0m"
	rm -f ../build/test_furrymap ../build/test_chainmap ../build/chainmap_benchmark ../build/test_spinlock ../build/chainhash_benchmark
	@echo "\033[0;31m[CLEAN] Clean completed\033[0m"

ttl-test: ttl-test-build
	@echo "\033[0;32m[TEST] Running comprehensive TTL Map test suite\033[0m"
	nice -19 ../build/test_ttlmap
	@echo "\033[0;32m[TEST] All tests passed successfully!\033[0m"

ttl-test-build:
	@echo "\033[0;35m[BUILD] Compiling comprehensive test_ttlmap ($(OPT))\033[0m"
	$(CC) -static $(CFLAGS) -o ../build/test_ttlmap test_ttlmap.c $(LDFLAGS)

ttl-debug: ttl-test-build
	@echo "\033[0;33m[DEBUG] Debugging test_ttlmap with gdb\033[0m"
	timeout -s SIGINT 5 gdb -batch -ex "set confirm off" -ex "br exit" -ex run -ex bt -ex quit --args ../build/test_ttlmap
