# moth build entry points. later tasks add: smoke, bench-native, test-integration, bench.
WINE ?= $(HOME)/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine
MOTH_TEST_BOTTLE ?= uhmm
export WINE MOTH_TEST_BOTTLE

DUNE := opam exec -- dune
CMAKE := cmake -G "Unix Makefiles"
JOBS ?= $(shell sysctl -n hw.ncpu)

.PHONY: setup build test test-native test-core helper clean

setup:
	opam init --bare --no-setup -y
	opam switch create . 5.5.2 --no-install -y
	opam install -y dune yojson alcotest

build:
	$(DUNE) build
	mkdir -p bin
	cp -f _build/default/core/bin/main.exe bin/moth

test: test-native test-core

test-native:
	$(CMAKE) -S helper -B build/native -DCMAKE_BUILD_TYPE=Release
	cmake --build build/native -j $(JOBS)
	build/native/moth_tests

test-core:
	opam exec -- dune test

helper:
	mkdir -p bin
	$(CMAKE) -S helper -B build/win64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/helper/cmake/mingw-w64-x86_64.cmake
	cmake --build build/win64 -j $(JOBS)
	$(CMAKE) -S helper -B build/win32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/helper/cmake/mingw-w64-i686.cmake
	cmake --build build/win32 -j $(JOBS)

clean:
	rm -rf build bin _build
