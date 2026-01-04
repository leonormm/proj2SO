# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L -pthread
LDFLAGS = -lncurses -pthread

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executable 
TARGET = Pacmanist

# Objects variables
OBJS = game.o display.o board.o

# Dependencies
display.o = display.h
board.o = board.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Support calling: make run /path/to/dir  (treat extra goal as DIR)
DIR_GOAL := $(filter-out run,$(MAKECMDGOALS))
.PHONY: $(DIR_GOAL)

# Make targets
all: pacmanist

pacmanist: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(SLEEP) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# run the program
run: pacmanist
	@./$(BIN_DIR)/$(TARGET) $(if $(DIR_GOAL),$(lastword $(DIR_GOAL)),$(DIR))

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)
	rm -f *.log

# indentify targets that do not create files
.PHONY: all clean run folders
