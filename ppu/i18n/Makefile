all:
	$(MAKE) eng
	$(MAKE) chs
	$(MAKE) cht

eng:
	$(MAKE) -f Makefile.eng clean 
	$(MAKE) -f Makefile.eng
chs:
	$(MAKE) -f Makefile.chs clean 
	$(MAKE) -f Makefile.chs
cht:
	$(MAKE) -f Makefile.cht clean 
	$(MAKE) -f Makefile.cht
	
clean:
	rm -f *.bak
	rm -f *.o
	rm -f *.elf
	rm -f *.prx