CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -MMD -MP

TARGET = server

SRC = src/main.c src/server.c src/http.c
OBJ = src/main.o src/server.o src/http.o 
DEP = $(OBJ:.o=.d)

all: $(TARGET)

TEST_TARGET = test_http
TEST_SRC = tests/test_http.c src/http.c src/server.c

$(TEST_TARGET): $(TEST_SRC)
	$(CC) $(CFLAGS) $(TEST_SRC) -o $(TEST_TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET)

test: all $(TEST_TARGET)
	./$(TEST_TARGET)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) $(TEST_TARGET)

-include $(DEP)