# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L -pthread
LDFLAGS = 

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executable 
TARGET = PacmanIST

# Objects variables
# server.o: o novo main
# game.o: lógica do jogo modificada
# board.o, parser.o: lógica de dados
OBJS = server.o game.o board.o parser.o

# Dependencies
server.o = protocol.h
game.o = board.h protocol.h
board.o = board.h
parser.o = parser.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Make targets
all: pacmanist

pacmanist: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

# Regra genérica para criar objectos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)

# identify targets that do not create files
.PHONY: all clean folders