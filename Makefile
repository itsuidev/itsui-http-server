CC = gcc
CFLAGS = -Wall -Wextra -Iinclude

TARGET = server

SRC = src/main.c src/server.c src/http.c
OBJ = src/main.o src/server.o src/http.o 

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@