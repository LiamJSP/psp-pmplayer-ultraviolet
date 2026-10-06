.PHONY: all clean

all:
	$(MAKE) -C src
	cp -f src/cooleyesBridge.prx .
	psp-build-exports -s src/cooleyesBridge.exp

clean:
	$(MAKE) -C src clean
	rm -f cooleyesBridge.prx cooleyesBridge.S cooleyesBridge.o
	rm -f *.bak src/*.bak
