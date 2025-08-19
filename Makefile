# BluStream Phase 4 - Build System

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
INCLUDES = -Icommon/include -I/opt/homebrew/include
LIBS = -L/opt/homebrew/lib -lavcodec -lavformat -lavutil

# Directories
BUILD_DIR = build
CLIENT_BUILD_DIR = $(BUILD_DIR)/client
FRAMES_DIR = frames

# Targets
CLIENT_TARGET = $(CLIENT_BUILD_DIR)/streaming_client
CLIENT_SRC = client/src/streaming_client.cpp
COMMON_SRC = common/src/logger.cpp common/src/error_codes.cpp

.PHONY: all clean client test frames-dir

all: client

client: $(CLIENT_TARGET)

$(CLIENT_TARGET): $(CLIENT_SRC) $(COMMON_SRC) | $(CLIENT_BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CLIENT_SRC) $(COMMON_SRC) $(LIBS) -o $@

$(CLIENT_BUILD_DIR):
	mkdir -p $(CLIENT_BUILD_DIR)

frames-dir:
	mkdir -p $(FRAMES_DIR)

test: client frames-dir
	@echo "Starting BluStream client test..."
	@echo "Make sure the Phase 4 server is running on the remote VM"
	@echo "Usage: ./$(CLIENT_TARGET) --save-frames --output-dir $(FRAMES_DIR)"

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(FRAMES_DIR)

# Helper targets
view-frames: frames-dir
	@if [ -f view_live_frames.sh ]; then \
		chmod +x view_live_frames.sh && ./view_live_frames.sh; \
	else \
		echo "view_live_frames.sh not found"; \
	fi

create-video: frames-dir
	@if command -v ffmpeg >/dev/null 2>&1; then \
		cd $(FRAMES_DIR) && ffmpeg -framerate 10 -i frame_%d.jpg -c:v libx264 -pix_fmt yuv420p blustream_output.mp4 -y; \
		echo "Video created: $(FRAMES_DIR)/blustream_output.mp4"; \
	else \
		echo "FFmpeg not found. Install with: brew install ffmpeg"; \
	fi

# Development helpers
debug: CXXFLAGS += -g -DDEBUG
debug: client

install-deps-macos:
	@echo "Installing dependencies on macOS..."
	brew install ffmpeg

help:
	@echo "BluStream Phase 4 Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build everything (default)"
	@echo "  client        - Build streaming client"
	@echo "  test          - Run client test"
	@echo "  clean         - Remove build artifacts"
	@echo "  view-frames   - Launch live frame viewer"
	@echo "  create-video  - Create MP4 from captured frames"
	@echo "  debug         - Build with debug symbols"
	@echo "  install-deps-macos - Install FFmpeg on macOS"
	@echo "  help          - Show this help"