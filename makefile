# Makefile for building spill.exe using gcc and windres

# === Variables ===
TARGET      := release/spill.exe
OBJ_DIR     := release
SRC         := main.cpp
RES         := resource.rc
RES_OBJ     := $(OBJ_DIR)/resource.o

CXX         := g++
WINDRES     := windres

CXXFLAGS    := -mwindows -Wall -std=c++17
LDFLAGS     := -lgdi32 -lshell32 -luser32 -lcomctl32

# === Rules ===
all: $(TARGET)

$(TARGET): $(SRC) $(RES_OBJ)
	$(CXX) $(SRC) $(RES_OBJ) -o $@ $(CXXFLAGS) $(LDFLAGS)

$(RES_OBJ): $(RES) | $(OBJ_DIR)
	$(WINDRES) $< -o $@

$(OBJ_DIR):
	mkdir $(OBJ_DIR)

clean:
	-del /Q $(OBJ_DIR)\* 2> NUL || exit 0

.PHONY: all clean
