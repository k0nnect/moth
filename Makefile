# moth build entry points.
WINE ?= $(HOME)/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine
MOTH_TEST_BOTTLE ?= uhmm
export WINE MOTH_TEST_BOTTLE

DUNE := opam exec -- dune
CMAKE := cmake -G "Unix Makefiles"
JOBS ?= $(shell sysctl -n hw.ncpu)

QTBASE ?= $(shell brew --prefix qtbase)
APP_CMAKE = $(CMAKE) -S app -B build/app -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(QTBASE)

.PHONY: setup build test test-native test-core test-app app bench-native helper smoke test-integration bench clean

setup:
	opam init --bare --no-setup -y
	opam switch create . 5.5.2 --no-install -y
	opam install -y dune yojson alcotest

build:
	$(DUNE) build
	mkdir -p bin
	cp -f _build/default/core/bin/main.exe bin/moth

test: test-native test-core test-app

test-native:
	$(CMAKE) -S helper -B build/native -DCMAKE_BUILD_TYPE=Release
	cmake --build build/native -j $(JOBS)
	build/native/moth_tests

test-core:
	opam exec -- dune test

# the window (Qt 6 from Homebrew's qtbase), into bin/moth-app; and build/app/it_app, which the
# end-to-end scenarios drive it with
app:
	$(APP_CMAKE)
	cmake --build build/app -j $(JOBS) --target moth-app it_app
	mkdir -p bin
	cp -f build/app/moth-app bin/moth-app

# the window's tests, offscreen, against a fake core
test-app:
	$(APP_CMAKE)
	cmake --build build/app -j $(JOBS)
	QT_QPA_PLATFORM=offscreen ctest --test-dir build/app --output-on-failure

# engine speed on this machine, native: a relative guide only (the real helper runs under wine)
bench-native:
	$(CMAKE) -S helper -B build/native -DCMAKE_BUILD_TYPE=Release
	cmake --build build/native -j $(JOBS) --target moth_bench
	build/native/moth_bench

helper:
	mkdir -p bin
	$(CMAKE) -S helper -B build/win64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/helper/cmake/mingw-w64-x86_64.cmake
	cmake --build build/win64 -j $(JOBS)
	$(CMAKE) -S helper -B build/win32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/helper/cmake/mingw-w64-i686.cmake
	cmake --build build/win32 -j $(JOBS)

smoke: helper
	@mkdir -p build && rm -f build/smoke.out
	@(printf '{"id":1,"op":"ps"}\n{"id":2,"op":"bye"}\n' | nc -l 127.0.0.1 47113 > build/smoke.out) & \
	 sleep 1; "$(WINE)" --bottle "$(MOTH_TEST_BOTTLE)" --workdir "$(CURDIR)/bin" "$(CURDIR)/bin/moth-helper.exe" 47113 smoketoken >/dev/null 2>&1; wait
	@grep -q '"hello"' build/smoke.out && grep -q 'smoketoken' build/smoke.out && grep -q 'moth-helper.exe' build/smoke.out && echo "smoke ok"

# end to end in the test bottle (MOTH_TEST_BOTTLE): every scenario, one line each. a few minutes.
test-integration: build helper app
	_build/default/core/integration/it_main.exe --bin "$(CURDIR)/bin"

# scenario 10 alone: the real helper's scan speed under wine, on a 1 GB target
bench: build helper
	_build/default/core/integration/it_main.exe --bin "$(CURDIR)/bin" 10

clean:
	rm -rf build bin _build
