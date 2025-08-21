# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Platform Auto-Detection
- `make all` - Builds client on macOS, server on Linux
- `make client` - Build streaming client (macOS/FFmpeg required)
- `make server` - Build streaming server (Linux/OpenGL required)
- `make clean` - Remove build artifacts
- `make debug` - Build with debug symbols

### Testing
- `make test` - Run client test (requires frames directory)
- `make view-frames` - Launch live frame viewer script
- `make create-video` - Create MP4 from captured frames using FFmpeg

### Dependencies
- `make install-deps-macos` - Install FFmpeg on macOS via Homebrew

## Remote Development Workflow

This project is designed for cross-platform development with server on Linux VM and client on macOS:

- `make sync-to-remote` - Upload source to rocky@10.7.4.116 via SSH
- `make sync-from-remote` - Download changes from remote VM
- `make build-remote-server` - Sync and build server on remote VM
- `make setup-remote` - Initialize remote directory structure
- `make test-remote-connection` - Test SSH connectivity

SSH connection uses `~/.ssh/key.pem` to connect to `rocky@10.7.4.116`.

## Architecture

### Core Components
- **Client** (`client/src/streaming_client.cpp`): H.264 streaming client with FFmpeg decoding
- **Server** (`server/src/`): OpenGL-based video streaming server with H.264 encoding
- **Common** (`common/`): Shared protocol definitions and logging utilities

### Protocol Implementation
- Custom TCP protocol with MessageHeader (32 bytes) + StreamConfig (20 bytes)
- Magic number 'BSTR' (0x42535452) for frame identification  
- H.264 video streaming at 1920x1080, 30 FPS, 5000 kbps
- Sequence numbers and CRC32 checksums for reliability

### Key Files
- `common/include/blustream/common/types.h` - Protocol definitions, MessageHeader, StreamConfig
- `common/src/logger.cpp` - Logging system used throughout codebase
- `view_live_frames.sh` - Script for real-time frame viewing during development

## Development Notes

### Frame Processing
- Frames saved as `.h264` (raw) and `.ppm` (decoded) in `frames/` directory
- Use `sips -s format jpeg frames/decoded_0.ppm --out frame_0.jpg` to convert PPM to viewable format
- FFmpeg can create videos from frame sequences

### Dependencies
- **macOS**: FFmpeg via Homebrew (`/opt/homebrew/`)
- **Linux**: FFmpeg system packages, OpenGL libraries (Mesa llvmpipe for headless)

### Platform Differences
- Client builds only on macOS (Darwin)
- Server builds only on Linux  
- Makefile automatically detects platform and sets appropriate target