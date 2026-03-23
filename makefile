# Makefile for building spill.exe using gcc and windres

# === Variables ===
TARGET      := release/spill.exe
OBJ_DIR     := release
SRC         := main.cpp
RES         := resource.rc
RES_OBJ     := $(OBJ_DIR)/resource.o

CXX         := x86_64-w64-mingw32-g++
WINDRES     := x86_64-w64-mingw32-windres

CXXFLAGS    := -mwindows -Wall -std=c++17
LDFLAGS     := -static -static-libgcc -static-libstdc++ -lpthread \
               -lgdi32 -lshell32 -luser32 -lcomctl32

# === Rules ===
all: $(TARGET)

$(TARGET): $(SRC) $(RES_OBJ)
	$(CXX) $(SRC) $(RES_OBJ) -o $@ $(CXXFLAGS) $(LDFLAGS)

$(RES_OBJ): $(RES) | $(OBJ_DIR)
	$(WINDRES) $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -f $(OBJ_DIR)/*

.PHONY: all clean