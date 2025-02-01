BUILD_DIR := build

# Targets
all: $(BUILD_DIR)
	$(MAKE) -C $(BUILD_DIR)
	@echo "[OK] target built successfully"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	@echo "[info] generating CMake build files"
	cd $(BUILD_DIR) && cmake ..
	@echo "[OK] success"

clean:
	rm -rf $(BUILD_DIR)
	@echo "[OK] success"

.PHONY: all clean

