# Swiss Map Fuzzing Tests

This directory contains comprehensive fuzzing tests for the Swiss map implementation in `common/swissmap.h`. The fuzzing tests are designed to find security vulnerabilities, memory safety issues, and algorithmic correctness problems under adversarial conditions.

## Overview

Three specialized fuzzing tests target different aspects of the Swiss map:

1. **`swissmap_operations`** - Hash collisions and key/value operations
2. **`swissmap_memory`** - Memory management and allocation failures
3. **`swissmap_internals`** - Low-level control byte and bitset operations

## Building

### With Fuzzing Enabled (LibFuzzer)

```bash
meson setup builddir -Dfuzzing=enabled
meson compile -C builddir
```

This builds the fuzzing targets with AddressSanitizer and LibFuzzer instrumentation.

### Without Fuzzing (Reproducer Mode)

```bash
make fuzz
```

This builds standalone executables that can reproduce crashes from fuzzing corpus files.

## Running Fuzzing Tests

### LibFuzzer Mode

```bash
# Run operations fuzzing
./builddir/tests/common/fuzzing/swissmap_operations

# Run memory fuzzing
./builddir/tests/common/fuzzing/swissmap_memory

# Run internals fuzzing
./builddir/tests/common/fuzzing/swissmap_internals
```

### Reproducer Mode

```bash
# Reproduce a crash from a corpus file
./builddir/tests/common/fuzzing/swissmap_operations crash_file.bin
```

## Fuzzing Test Details

### 1. Operations Fuzzing (`swissmap_operations.c`)

**Target**: Hash collisions and key/value operations

**What it tests**:
- Hash collision scenarios with controlled hash functions
- Random sequences of put/get/delete operations
- Variable key and value sizes (up to 256 bytes each)
- Edge cases: empty keys, maximum size keys, NULL pointers
- Data integrity verification after operations
- Map consistency during concurrent operations

**Fuzzing strategy**:
- Parses fuzzer input as operation commands
- Extracts key/value pairs from fuzzer data
- Tests with collision-inducing hash functions
- Verifies map state consistency after each operation

### 2. Memory Fuzzing (`swissmap_memory.c`)

**Target**: Memory allocation failures and growth scenarios

**What it tests**:
- Map creation under memory pressure
- Table growth and splitting with allocation failures
- Directory expansion failures
- Memory leak detection
- Multiple maps under memory constraints
- Recovery from allocation failures

**Fuzzing strategy**:
- Uses custom allocator that can fail at specific points
- Simulates memory pressure scenarios
- Tests error path handling and cleanup
- Verifies memory consistency after failures

### 3. Internals Fuzzing (`swissmap_internals.c`)

**Target**: Low-level control byte and bitset operations

**What it tests**:
- Control byte state transitions (empty/deleted/full)
- Bitset manipulation functions
- Probe sequence generation and advancement
- Hash extraction (H1/H2) with edge cases
- Group operations and slot management
- Utility functions (alignment, etc.)

**Fuzzing strategy**:
- Direct testing of internal functions
- Fuzzes control group states and transitions
- Tests bitset operations with malformed data
- Verifies algorithmic correctness of core operations

## Key Areas Tested

### Hash Function Vulnerabilities
- FNV-1a hash implementation
- Hash collision handling in probe sequences
- H1/H2 extraction functions

### Memory Management
- Map creation and initial allocation
- Table growth operations
- Table splitting logic
- Directory expansion

### Control Byte Operations
- H2 hash matching
- Empty slot detection
- Available slot detection (empty or deleted)

### Core Operations
- Key insertion (`swiss_map_put`)
- Key lookup (`swiss_map_get`)
- Key deletion (`swiss_map_delete`)

### Extendible Hashing
- Directory index calculation
- Table splitting and directory management
- Global/local depth consistency

## Expected Findings

The fuzzing tests are designed to catch:

1. **Memory safety issues**: Buffer overflows, use-after-free, double-free
2. **Integer overflows**: In size calculations and hash computations
3. **Logic errors**: Inconsistent map state, incorrect probe sequences
4. **Hash collision attacks**: Algorithmic complexity attacks
5. **Memory leaks**: Incomplete cleanup in error paths
6. **Assertion failures**: Violated invariants in debug builds

## Integration

The fuzzing tests integrate with the existing meson build system:
- Conditional compilation based on `get_option('fuzzing').enabled()`
- AddressSanitizer and FuzzingSanitizer flags when fuzzing is enabled
- Shared dependencies with existing Swiss map tests
- Reproducer mode for debugging crashes

## Usage Tips

1. **Start with short runs** to verify the fuzzing setup works
2. **Use corpus files** from previous runs to improve coverage
3. **Monitor memory usage** as fuzzing can be memory-intensive
4. **Save interesting crashes** for later analysis with the reproducer
5. **Run different fuzzing targets** to get comprehensive coverage

## Corpus Management

LibFuzzer automatically manages corpus files:
- Saves interesting inputs that increase coverage
- Minimizes test cases to reduce redundancy
- Merges corpus directories for better efficiency

Example corpus management:
```bash
# Merge corpus from multiple runs
./swissmap_operations -merge=1 corpus_dir1 corpus_dir2

# Minimize existing corpus
./swissmap_operations -minimize_crash=1 crash_file
```

## Coverage Analysis

To analyze code coverage from fuzzing runs, you can generate and view coverage reports using LLVM coverage tools.

### Generating Coverage Reports

First, run the fuzzer with a limited number of runs to generate profiling data:

```bash
# Run fuzzer with coverage profiling (using buildfuzz directory)
./buildfuzz/tests/common/fuzzing/swissmap_operations -runs=1 corpus/
```

Then merge the profiling data and generate a coverage report:

```bash
# Merge profiling data and generate coverage report
llvm-profdata merge -sparse *.profraw -o default.profdata && \
llvm-cov show ./buildfuzz/tests/common/fuzzing/swissmap_operations \
  -instr-profile=default.profdata --use-color > cover.txt
```

### Viewing Coverage Reports

View the coverage report with syntax highlighting:

```bash
# View coverage report with color support
less -R cover.txt
```

The coverage report shows:
- **Lines with execution counts**: Code that was executed during fuzzing (e.g., `1|`, `224k|`)
- **Highlighted lines**: Code that was not executed
- **Coverage percentages**: For functions and overall coverage
- **Execution counts**: How many times each line was executed

### Coverage Analysis Tips

1. **Focus on uncovered code paths** - Red lines indicate potential areas for improved test cases
2. **Check error handling paths** - Ensure exception and error paths are covered
3. **Verify edge cases** - Look for boundary conditions that may not be tested
4. **Monitor coverage trends** - Track coverage improvements over multiple fuzzing sessions
