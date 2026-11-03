# AlsaTest - Makefile
#
# Estilo igual ao Makefile dos módulos de kernel (CROSS_COMPILE, etc.),
# mas adaptado a um programa de user space (não é módulo do kernel).
#
# Uso:
#   make                              -> compila AlsaTest para ARM64
#   make clean                        -> apaga AlsaTest e AlsaTest.o
#   make install DESTDIR=<overlay>    -> copia para <overlay>/usr/bin/AlsaTest
#   make help                         -> mostra esta ajuda

# --- o que construir e a partir do quê ---
TARGET := AlsaTest
SRC    := AlsaTest.c
OBJ    := $(SRC:.c=.o)

# --- toolchain (mesmo prefixo do Makefile do kernel) ---
CROSS_COMPILE ?= /home/igor/buildroot/buildroot-2026.08/output/host/bin/aarch64-linux-
CC            := $(CROSS_COMPILE)gcc

# --- flags ---
CFLAGS  := -O2 -Wall -Wextra
LDFLAGS :=
LDLIBS  := -lasound

export CC CFLAGS LDFLAGS LDLIBS

# --- alvos "utilitários" (não são ficheiros) ---
BUILD_TARGETS := clean install help
.PHONY: all $(BUILD_TARGETS)

# --- alvo por defeito (o primeiro do ficheiro) ---
all: $(TARGET)

# --- compilar / ligar ---
$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- limpar ---
clean:
	rm -f $(OBJ) $(TARGET)

# --- ajuda ---
help:
	@echo "make                             compila $(TARGET) para ARM64"
	@echo "make clean                       apaga $(OBJ) e $(TARGET)"
	@echo "make install DESTDIR=<overlay>   copia para <overlay>/usr/bin/$(TARGET)"
	@echo "make help                        mostra esta ajuda"
