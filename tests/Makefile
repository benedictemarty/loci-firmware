# Tests natifs de non-régression pour le firmware LOCI (modèles de logique).
# Compilation stricte, exécution directe. Aucun matériel/toolchain RP2040 requis.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2

TESTS := acia_rx_model_test

.PHONY: all test clean
all: test

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done
	@echo "Tous les tests passent."

$(TESTS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
