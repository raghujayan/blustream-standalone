# BluStream Phase 4 - Unified Build System with HueSpace Integration
# Builds client on macOS, server on Linux

# Detect platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    CXX = g++
    FFMPEG_INCLUDES = -I/usr/include/ffmpeg
    FFMPEG_LIBS = -lavcodec -lavformat -lavutil -lswscale
    OPENGL_LIBS = -lGL -lGLX -lX11
    
    # HueSpace SDK paths
    HUESPACE_ROOT = /home/rocky/HueSpace-13.3.0-449-gcc8.4.1-redhat-8-x86_64
    HUESPACE_INCLUDES = -I$(HUESPACE_ROOT)/include
    HUESPACE_LIBS = -L$(HUESPACE_ROOT)/lib -lhueproxy -Wl,--rpath,$(HUESPACE_ROOT)/lib
    
    SERVER_LIBS = $(FFMPEG_LIBS) $(OPENGL_LIBS) $(HUESPACE_LIBS) -lpthread
    DEFAULT_TARGET = server
endif
ifeq ($(UNAME_S),Darwin)
    PLATFORM = macos
    CXX = g++
    FFMPEG_INCLUDES = -I/opt/homebrew/include
    FFMPEG_LIBS = -L/opt/homebrew/lib -lavcodec -lavformat -lavutil
    DEFAULT_TARGET = client
endif

# Common settings
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
INCLUDES = -Icommon/include -Iserver/include $(FFMPEG_INCLUDES)

# Add HueSpace includes for server builds
ifeq ($(PLATFORM),linux)
    INCLUDES += $(HUESPACE_INCLUDES)
endif

COMMON_SRC = common/src/logger.cpp common/src/error_codes.cpp

# Directories
BUILD_DIR = build
CLIENT_BUILD_DIR = $(BUILD_DIR)/client
SERVER_BUILD_DIR = $(BUILD_DIR)/server
FRAMES_DIR = frames

# Targets
CLIENT_TARGET = $(CLIENT_BUILD_DIR)/streaming_client
SERVER_TARGET = $(SERVER_BUILD_DIR)/blustream_phase4_server
SERVER_4B_TARGET = $(SERVER_BUILD_DIR)/blustream_phase4b_server
HW_ENCODER_TEST_TARGET = $(SERVER_BUILD_DIR)/test_hardware_encoding
CLIENT_SRC = client/src/streaming_client.cpp
SERVER_SRC = server/src/phase4_main.cpp server/src/streaming_server.cpp server/src/opengl_context.cpp server/src/network_server.cpp server/src/vds_manager.cpp server/src/hardware_encoder.cpp server/src/streaming_server_hw.cpp
SERVER_4B_SRC = server/src/phase4b_main.cpp server/src/streaming_server.cpp server/src/opengl_context.cpp server/src/network_server.cpp server/src/vds_manager.cpp server/src/hardware_encoder.cpp

.PHONY: all clean client server server-4b test frames-dir sync-to-remote sync-from-remote test-hw-encoding

all: $(DEFAULT_TARGET)

client: $(CLIENT_TARGET)

server: $(SERVER_TARGET)

server-4b: $(SERVER_4B_TARGET)

test-hw-encoding: $(HW_ENCODER_TEST_TARGET)

$(CLIENT_TARGET): $(CLIENT_SRC) $(COMMON_SRC) | $(CLIENT_BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CLIENT_SRC) $(COMMON_SRC) $(FFMPEG_LIBS) -o $@

$(SERVER_TARGET): $(SERVER_SRC) $(COMMON_SRC) | $(SERVER_BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SERVER_SRC) $(COMMON_SRC) $(SERVER_LIBS) -o $@

$(SERVER_4B_TARGET): $(SERVER_4B_SRC) $(COMMON_SRC) | $(SERVER_BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SERVER_4B_SRC) $(COMMON_SRC) $(SERVER_LIBS) -o $@

$(HW_ENCODER_TEST_TARGET): server/src/test_hardware_encoding.cpp server/src/hardware_encoder.cpp $(COMMON_SRC) | $(SERVER_BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) server/src/test_hardware_encoding.cpp server/src/hardware_encoder.cpp $(COMMON_SRC) $(SERVER_LIBS) -o $@

$(CLIENT_BUILD_DIR):
	mkdir -p $(CLIENT_BUILD_DIR)

$(SERVER_BUILD_DIR):
	mkdir -p $(SERVER_BUILD_DIR)

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

# Sync targets for remote development
REMOTE_HOST = rocky@10.7.4.116
REMOTE_DIR = ~/blustream-unified
SSH_KEY = ~/.ssh/key.pem

sync-to-remote:
	@echo "🔄 Syncing source code to remote VM..."
	rsync -avz --exclude 'build/' --exclude 'frames/' --exclude '.git/' \
		-e "ssh -i $(SSH_KEY)" ./ $(REMOTE_HOST):$(REMOTE_DIR)/

sync-from-remote:
	@echo "🔄 Syncing from remote VM..."
	rsync -avz --exclude 'build/' --exclude 'frames/' \
		-e "ssh -i $(SSH_KEY)" $(REMOTE_HOST):$(REMOTE_DIR)/ ./

build-remote-server: sync-to-remote
	@echo "🏗️  Building server on remote VM..."
	ssh -i $(SSH_KEY) $(REMOTE_HOST) "cd $(REMOTE_DIR) && make server"

test-remote-connection:
	@echo "🔗 Testing connection to remote VM..."
	ssh -i $(SSH_KEY) $(REMOTE_HOST) "echo '✅ Connected to' \$$(hostname) \$$(uname -a)"

setup-remote:
	@echo "📁 Setting up remote directory..."
	ssh -i $(SSH_KEY) $(REMOTE_HOST) "mkdir -p $(REMOTE_DIR)"
	$(MAKE) sync-to-remote

help:
	@echo "BluStream Phase 4 Unified Build System"
	@echo "Platform: $(PLATFORM)"
	@echo ""
	@echo "Build Targets:"
	@echo "  all               - Build for current platform ($(DEFAULT_TARGET))"
	@echo "  client            - Build streaming client (macOS)"
	@echo "  server            - Build streaming server (Linux)"
	@echo "  clean             - Remove build artifacts"
	@echo "  debug             - Build with debug symbols"
	@echo ""
	@echo "Testing Targets:"
	@echo "  test              - Run client test"
	@echo "  view-frames       - Launch live frame viewer"
	@echo "  create-video      - Create MP4 from captured frames"
	@echo ""
	@echo "Remote Development:"
	@echo "  sync-to-remote    - Upload source to remote VM"
	@echo "  sync-from-remote  - Download from remote VM" 
	@echo "  build-remote-server - Sync and build server on VM"
	@echo "  test-remote-connection - Test SSH connection"
	@echo "  setup-remote      - Initialize remote directory"
	@echo ""
	@echo "Dependencies:"
	@echo "  install-deps-macos - Install FFmpeg on macOS"
