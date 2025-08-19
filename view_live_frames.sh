#!/bin/bash

# Live frame viewer for BluStream client
# Converts PPM frames to JPEG and displays them

FRAMES_DIR="frames"
VIEWER_APP="Preview"  # Change to "Finder" or specific app if preferred

echo "🎥 BluStream Live Frame Viewer"
echo "Monitoring: $FRAMES_DIR/"
echo "Press Ctrl+C to stop"
echo

# Create frames directory if it doesn't exist
mkdir -p "$FRAMES_DIR"

# Keep track of last processed frame
LAST_FRAME=-1

while true; do
    # Find the highest numbered decoded frame
    LATEST_FRAME=$(find "$FRAMES_DIR" -name "decoded_*.ppm" -exec basename {} \; 2>/dev/null | \
                   sed 's/decoded_\([0-9]*\)\.ppm/\1/' | \
                   sort -n | tail -1)
    
    if [[ -n "$LATEST_FRAME" && "$LATEST_FRAME" -gt "$LAST_FRAME" ]]; then
        # Convert new frames to JPEG
        for ((i=LAST_FRAME+1; i<=LATEST_FRAME; i++)); do
            PPM_FILE="$FRAMES_DIR/decoded_$i.ppm"
            JPG_FILE="$FRAMES_DIR/frame_$i.jpg"
            
            if [[ -f "$PPM_FILE" && ! -f "$JPG_FILE" ]]; then
                echo "📸 Converting frame $i..."
                sips -s format jpeg "$PPM_FILE" --out "$JPG_FILE" >/dev/null 2>&1
                
                # Open the latest frame
                if [[ "$i" == "$LATEST_FRAME" ]]; then
                    echo "👁️  Displaying frame $i"
                    open -a "$VIEWER_APP" "$JPG_FILE"
                fi
            fi
        done
        
        LAST_FRAME="$LATEST_FRAME"
    fi
    
    sleep 0.5  # Check for new frames every 500ms
done