CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -MMD -MP

TARGET = server

SRC = src/main.c src/server.c src/http.c
OBJ = src/main.o src/server.o src/http.o 
DEP = $(OBJ:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

-include $(DEP)