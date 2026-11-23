# Protocol/algorithm checks work on either the laptop or the Pi.
# Controller and GUI have separate run scripts; they are not compiled here.
TESTS := FFTW DSP Pthreads Protocol FileIO
.PHONY: all native clean $(TESTS)
all: $(TESTS)
$(TESTS):
	$(MAKE) -C $@ all
native:
	@set -e; for test in $(TESTS); do $(MAKE) -C "$$test" native; done
clean:
	@set -e; for test in $(TESTS); do $(MAKE) -C "$$test" clean; done
