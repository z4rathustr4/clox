CFLAGS := -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
CC := gcc
SRC_DIR := .
INCLUDE_DIR := include
SRC_FILES := $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES := $(SRC_FILES:.c=.o)
TARGET := clox

$(TARGET): $(OBJ_FILES)
	$(CC) $(OBJ_FILES) -o $(TARGET)
	@echo "build successful"
	$(RM) $(OBJ_FILES)
	@echo "deletion of object files successful"
	@echo "build completed successfully"


%.o: %.c
	$(CC) -I$(INCLUDE_DIR) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_FILES) $(TARGET)

.PHONY: clean

