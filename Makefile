.DEFAULT_GOAL := debug
.PHONY: setup setup-debug release debug paper clean

setup:
	meson setup --native-file meson.ini build-rel --buildtype=release
	meson setup --native-file meson.ini build-dbg --buildtype=debug

debug: setup
	cd build-dbg; meson compile

paper:
	@$(MAKE) -C atc2024

clean:
	cd build-rel; meson compile --clean
	cd build-dbg; meson compile --clean

install-deps:
	sudo apt install -y meson libfmt-dev libaio-dev librados-dev mold \
    	libgoogle-perftools-dev libtcmalloc-minimal4 libboost-dev \
    	liburing-dev
