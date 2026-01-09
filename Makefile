all:
	$(MAKE) -C SO-2526-sol-parte1
	$(MAKE) -C client-base-with-Makefile-v3

clean:
	$(MAKE) -C SO-2526-sol-parte1 clean
	$(MAKE) -C client-base-with-Makefile-v3 clean