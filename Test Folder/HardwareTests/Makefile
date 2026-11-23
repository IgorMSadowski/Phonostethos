# Build each independent hardware diagnostic in its own folder.
TESTS := ALSA USB LEDs Udev TCP Scheduling
.PHONY: all native clean $(TESTS)
all: $(TESTS)
$(TESTS):
	$(MAKE) -C $@ all
native:
	@set -e; for test in $(TESTS); do $(MAKE) -C "$$test" native; done
clean:
	@set -e; for test in $(TESTS); do $(MAKE) -C "$$test" clean; done
