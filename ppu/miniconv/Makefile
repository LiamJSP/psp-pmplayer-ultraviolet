all:
	$(MAKE) cjk
	$(MAKE) eur
eur:
	$(MAKE) -f Makefile.eur clean 
	$(MAKE) -f Makefile.eur
cjk:
	$(MAKE) -f Makefile.cjk clean 
	$(MAKE) -f Makefile.cjk
	
clean:
	rm -f *.bak
	rm -f *.o
	rm -f *.elf
	rm -f *.prx