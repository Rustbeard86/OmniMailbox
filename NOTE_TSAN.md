# Thread Sanitizer (TSAN) Testing Guide

## Current Limitation

**Thread Sanitizer (TSAN) is not available on Windows with MSVC.**

TSAN is a powerful data race detector, but it is only supported on:
- **Linux**: GCC 4.8+, Clang 3.2+
- **macOS**: Clang (Xcode)

This project is currently built on Windows using MSVC, which does not support TSAN.

## Why TSAN Matters

According to the OmniMailbox Design Specification (Section 7.4 - Sanitizer Validation):
- All code must be **TSAN clean** (no data races)
- Integration tests should pass with Thread Sanitizer enabled
- This validates the lock-free SPSC queue implementation

Data races are critical issues in concurrent code that can cause:
- Non-deterministic behavior
- Crashes in production
- Subtle corruption that's hard to debug

## Current Test Status

The integration tests pass successfully on Windows:

```
✅ IntegrationTest.ProducerConsumerRoundTrip (8 ms)
✅ IntegrationTest.MultipleChannels (206 ms)
✅ All 69 tests passing
```

However, this does **not** validate absence of data races without TSAN.

## Testing Options

### Option 1: WSL (Windows Subsystem for Linux) - Recommended

If WSL is available on your Windows machine, you can run TSAN tests locally.

**Setup (one-time):**
```bash
# In PowerShell (as Administrator)
wsl --install  # If WSL not already installed
wsl --set-default-version 2
```

**Run TSAN tests:**
```bash
# Open WSL terminal (Ubuntu)
cd /mnt/c/Users/drewb/source/repos/OmniMailbox

# Install dependencies (first time only)
sudo apt update
sudo apt install -y build-essential cmake clang libflatbuffers-dev

# Configure with TSAN
cmake -S . -B build-tsan \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build-tsan --parallel

# Run integration tests with TSAN
./build-tsan/omni-tests --gtest_filter=IntegrationTest.*

# Run all tests with TSAN
./build-tsan/omni-tests
```

**Expected output if clean:**
```
==================
WARNING: ThreadSanitizer: data race (would appear if race detected)
==================
[  PASSED  ] All tests
```

### Option 2: GitHub Actions CI/CD - Recommended for Production

Add automated TSAN testing to your GitHub repository:

**Create `.github/workflows/sanitizers.yml`:**
```yaml
name: Sanitizers

on: [push, pull_request]

jobs:
  tsan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Install dependencies
        run: |
          sudo apt update
          sudo apt install -y clang libflatbuffers-dev
      
      - name: Configure with TSAN
        run: |
          cmake -S . -B build-tsan \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
            -DCMAKE_BUILD_TYPE=Debug
      
      - name: Build
        run: cmake --build build-tsan --parallel
      
      - name: Run tests with TSAN
        run: |
          cd build-tsan
          ./omni-tests --gtest_filter=IntegrationTest.*
```

This ensures every push/PR is validated for data races.

### Option 3: Address Sanitizer (ASAN) - Windows Alternative

While TSAN isn't available, ASAN (Address Sanitizer) works on Windows MSVC and can catch some threading issues (use-after-free, memory corruption).

**Run ASAN on Windows:**
```powershell
# Configure with ASAN
cmake -S . -B build-asan `
    -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi" `
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build-asan --config Debug

# Run tests
.\build-asan\Debug\omni-tests.exe --gtest_filter=IntegrationTest.*
```

**Note:** ASAN detects memory errors but **not data races**. It's a partial validation only.

### Option 4: Native Linux Testing

Test on a Linux machine or VM:

**With GCC:**
```bash
cmake -S . -B build-tsan \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1" \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build-tsan --parallel
./build-tsan/omni-tests --gtest_filter=IntegrationTest.*
```

**With Clang:**
```bash
cmake -S . -B build-tsan \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build-tsan --parallel
./build-tsan/omni-tests --gtest_filter=IntegrationTest.*
```

## Recommended Approach

1. **Short-term**: Use WSL for local TSAN testing during development
2. **Production**: Set up GitHub Actions for automated TSAN on every commit
3. **Optional**: Run ASAN on Windows as an additional safety check

## TSAN Flags Explained

- `-fsanitize=thread`: Enable Thread Sanitizer
- `-fno-omit-frame-pointer`: Better stack traces in error reports
- `-g`: Include debug symbols for line numbers in reports
- `-O1`: Light optimization (TSAN requires some optimization)

## Common TSAN Warnings to Watch For

If TSAN detects issues, you'll see reports like:

```
==================
WARNING: ThreadSanitizer: data race (data race on variable)
  Write of size 8 at 0x7b0400001234 by thread T1:
    #0 ProducerHandle::Push() producer_handle.cpp:123
    
  Previous read of size 8 at 0x7b0400001234 by thread T2:
    #0 ConsumerHandle::Pop() consumer_handle.cpp:456
    
SUMMARY: ThreadSanitizer: data race
==================
```

These indicate where to fix memory ordering or synchronization issues.

## Verification Checklist

To meet the design specification requirements:

- [ ] Integration tests pass with TSAN (no data races reported)
- [ ] Unit tests pass with TSAN
- [ ] All sanitizer builds are clean (ASAN, UBSAN, Valgrind)
- [ ] CI/CD pipeline includes TSAN checks

## Additional Resources

- [Thread Sanitizer Documentation](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
- [WSL Installation Guide](https://docs.microsoft.com/en-us/windows/wsl/install)
- [GitHub Actions Documentation](https://docs.github.com/en/actions)

## Status

- ✅ Integration tests implemented (section 7.3)
- ✅ Tests pass on Windows (MSVC)
- ⚠️ TSAN validation pending (requires Linux/WSL)
- 📋 Recommended: Set up GitHub Actions for automated TSAN testing

---

**Last Updated**: [Current Date]  
**Maintainer**: Project Team
