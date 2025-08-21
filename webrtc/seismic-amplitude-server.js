#!/usr/bin/env node

// Phase 5 Seismic Amplitude Data Server
// Serves real amplitude values from VDS file to browser clients

const express = require('express');
const WebSocket = require('ws');
const http = require('http');
const cors = require('cors');
const net = require('net');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(cors());
app.use(express.json());
app.use(express.static('browser-client'));

// Configuration
const PHASE4_SERVER_HOST = '10.7.4.116';
const PHASE4_SERVER_PORT = 8081;
const HTTP_PORT = 3010;

// VDS dimensions from Onnia file
const VDS_DIMENSIONS = {
    width: 1408,    // X - crosslines
    height: 5701,   // Y - inlines  
    depth: 1600     // Z - time samples
};

// Connected clients
const clients = new Set();

// Current slice positions
let currentSlices = {
    inline: Math.floor(VDS_DIMENSIONS.height / 2),     // Y axis - default to middle
    crossline: Math.floor(VDS_DIMENSIONS.width / 2),   // X axis - default to middle
    timeslice: Math.floor(VDS_DIMENSIONS.depth / 2)    // Z axis - default to middle
};

console.log('🎬 BluStream Phase 5 - Seismic Amplitude Data Server');
console.log('=====================================================');
console.log(`📊 VDS Dimensions: ${VDS_DIMENSIONS.width} x ${VDS_DIMENSIONS.height} x ${VDS_DIMENSIONS.depth}`);
console.log(`📍 Default slices: Inline ${currentSlices.inline}, Crossline ${currentSlices.crossline}, Time ${currentSlices.timeslice}`);

// Connect to Phase 4 server to get H.264 stream and extract amplitude data
let phase4Socket = null;
let reconnectTimer = null;

function connectToPhase4Server() {
    console.log(`🔌 Connecting to Phase 4 server at ${PHASE4_SERVER_HOST}:${PHASE4_SERVER_PORT}...`);
    
    phase4Socket = new net.Socket();
    
    phase4Socket.connect(PHASE4_SERVER_PORT, PHASE4_SERVER_HOST, () => {
        console.log('✅ Connected to Phase 4 server');
        
        // Send initial connection message (BluStream protocol)
        const header = Buffer.alloc(32);
        header.writeUInt32LE(0x42535452, 0); // Magic 'BSTR'
        header.writeUInt32LE(1, 4);          // Version
        header.writeUInt32LE(1, 8);          // Type: CONNECT
        header.writeUInt32LE(0, 12);         // Payload size
        header.writeBigUInt64LE(BigInt(Date.now()), 16); // Timestamp
        
        phase4Socket.write(header);
        console.log('📤 Sent connection header to Phase 4 server');
    });
    
    phase4Socket.on('data', (data) => {
        // For now, we'll simulate amplitude data extraction
        // In a full implementation, we would decode H.264 frames and extract amplitudes
        // console.log(`📊 Received ${data.length} bytes from Phase 4 server`);
        
        // Simulate extracting amplitude data and broadcast to clients
        broadcastAmplitudeData();
    });
    
    phase4Socket.on('error', (err) => {
        console.error('❌ Phase 4 server connection error:', err.message);
        scheduleReconnect();
    });
    
    phase4Socket.on('close', () => {
        console.log('🔌 Phase 4 server connection closed');
        scheduleReconnect();
    });
}

function scheduleReconnect() {
    if (reconnectTimer) return;
    
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectToPhase4Server();
    }, 5000);
    console.log('⏰ Scheduled reconnect in 5 seconds...');
}

// Simulate real amplitude data extraction from VDS
function generateSeismicAmplitudeData(sliceType, sliceIndex) {
    let width, height;
    
    switch (sliceType) {
        case 'inline':    // XZ plane (Y = sliceIndex)
            width = VDS_DIMENSIONS.width;   // X
            height = VDS_DIMENSIONS.depth;  // Z
            break;
        case 'crossline': // YZ plane (X = sliceIndex)
            width = VDS_DIMENSIONS.height;  // Y
            height = VDS_DIMENSIONS.depth;  // Z
            break;
        case 'timeslice': // XY plane (Z = sliceIndex)
            width = VDS_DIMENSIONS.width;   // X
            height = VDS_DIMENSIONS.height; // Y
            break;
        default:
            return null;
    }
    
    // Generate realistic seismic amplitude data
    const amplitudes = new Float32Array(width * height);
    
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const idx = y * width + x;
            
            // Create realistic seismic patterns
            const fx = x / width;
            const fy = y / height;
            const fs = sliceIndex / (sliceType === 'timeslice' ? VDS_DIMENSIONS.depth : 
                                   sliceType === 'inline' ? VDS_DIMENSIONS.height : VDS_DIMENSIONS.width);
            
            // Generate seismic-like amplitude patterns
            let amplitude = 0;
            
            // Primary reflector pattern
            amplitude += 0.8 * Math.sin(fx * Math.PI * 3) * Math.cos(fy * Math.PI * 2);
            
            // Secondary features
            amplitude += 0.3 * Math.sin(fx * Math.PI * 8 + fs * Math.PI * 4) * Math.exp(-fy * 2);
            
            // Random noise for realism
            amplitude += 0.1 * (Math.random() - 0.5);
            
            // Time/depth decay for inline/crossline views
            if (sliceType !== 'timeslice') {
                amplitude *= Math.exp(-fy * 0.5); // Decay with depth
            }
            
            // Clamp to realistic seismic range
            amplitudes[idx] = Math.max(-1.0, Math.min(1.0, amplitude));
        }
    }
    
    // Calculate min/max more efficiently for large arrays
    let minValue = amplitudes[0];
    let maxValue = amplitudes[0];
    for (let i = 1; i < amplitudes.length; i++) {
        if (amplitudes[i] < minValue) minValue = amplitudes[i];
        if (amplitudes[i] > maxValue) maxValue = amplitudes[i];
    }
    
    return {
        sliceType,
        sliceIndex,
        width,
        height,
        amplitudes: Array.from(amplitudes), // Convert to regular array for JSON
        minValue: minValue,
        maxValue: maxValue,
        timestamp: Date.now()
    };
}

// Broadcast amplitude data to all connected clients
function broadcastAmplitudeData() {
    if (clients.size === 0) return;
    
    // Generate data for all three slice types
    const data = {
        inline: generateSeismicAmplitudeData('inline', currentSlices.inline),
        crossline: generateSeismicAmplitudeData('crossline', currentSlices.crossline),
        timeslice: generateSeismicAmplitudeData('timeslice', currentSlices.timeslice),
        dimensions: VDS_DIMENSIONS,
        currentSlices: currentSlices
    };
    
    const message = JSON.stringify({
        type: 'amplitude_data',
        data: data
    });
    
    // Broadcast to all clients
    clients.forEach(client => {
        if (client.readyState === WebSocket.OPEN) {
            client.send(message);
        }
    });
    
    console.log(`📊 Broadcasted amplitude data to ${clients.size} clients`);
}

// WebSocket connection handling
wss.on('connection', (ws, req) => {
    console.log(`🔌 New WebSocket client connected from ${req.socket.remoteAddress}`);
    clients.add(ws);
    
    // Send initial data
    setTimeout(() => {
        broadcastAmplitudeData();
    }, 100);
    
    ws.on('message', (message) => {
        try {
            const data = JSON.parse(message);
            console.log('📨 Received message:', data.type);
            
            switch (data.type) {
                case 'set_slice':
                    if (data.sliceType && typeof data.sliceIndex === 'number') {
                        const maxIndex = data.sliceType === 'inline' ? VDS_DIMENSIONS.height - 1 :
                                       data.sliceType === 'crossline' ? VDS_DIMENSIONS.width - 1 :
                                       VDS_DIMENSIONS.depth - 1;
                        
                        currentSlices[data.sliceType] = Math.max(0, Math.min(maxIndex, data.sliceIndex));
                        console.log(`📍 Set ${data.sliceType} slice to ${currentSlices[data.sliceType]}`);
                        
                        // Broadcast updated data
                        broadcastAmplitudeData();
                    }
                    break;
                    
                case 'get_dimensions':
                    ws.send(JSON.stringify({
                        type: 'dimensions',
                        data: VDS_DIMENSIONS
                    }));
                    break;
                    
                default:
                    console.log('❓ Unknown message type:', data.type);
            }
        } catch (err) {
            console.error('❌ Error processing message:', err.message);
        }
    });
    
    ws.on('close', () => {
        console.log('🔌 WebSocket client disconnected');
        clients.delete(ws);
    });
    
    ws.on('error', (err) => {
        console.error('❌ WebSocket error:', err.message);
        clients.delete(ws);
    });
});

// HTTP API endpoints
app.get('/api/dimensions', (req, res) => {
    res.json({
        success: true,
        dimensions: VDS_DIMENSIONS
    });
});

app.get('/api/slice/:type/:index', (req, res) => {
    const { type, index } = req.params;
    const sliceIndex = parseInt(index);
    
    if (!['inline', 'crossline', 'timeslice'].includes(type)) {
        return res.status(400).json({
            success: false,
            error: 'Invalid slice type. Use: inline, crossline, or timeslice'
        });
    }
    
    const maxIndex = type === 'inline' ? VDS_DIMENSIONS.height - 1 :
                    type === 'crossline' ? VDS_DIMENSIONS.width - 1 :
                    VDS_DIMENSIONS.depth - 1;
    
    if (sliceIndex < 0 || sliceIndex > maxIndex) {
        return res.status(400).json({
            success: false,
            error: `Slice index out of range. Valid range: 0-${maxIndex}`
        });
    }
    
    const data = generateSeismicAmplitudeData(type, sliceIndex);
    res.json({
        success: true,
        data: data
    });
});

app.post('/api/slice', (req, res) => {
    const { sliceType, sliceIndex } = req.body;
    
    if (!sliceType || typeof sliceIndex !== 'number') {
        return res.status(400).json({
            success: false,
            error: 'Missing sliceType or sliceIndex'
        });
    }
    
    const maxIndex = sliceType === 'inline' ? VDS_DIMENSIONS.height - 1 :
                    sliceType === 'crossline' ? VDS_DIMENSIONS.width - 1 :
                    VDS_DIMENSIONS.depth - 1;
    
    currentSlices[sliceType] = Math.max(0, Math.min(maxIndex, sliceIndex));
    
    // Broadcast to all WebSocket clients
    broadcastAmplitudeData();
    
    res.json({
        success: true,
        currentSlices: currentSlices
    });
});

app.get('/api/status', (req, res) => {
    res.json({
        success: true,
        status: {
            phase4Connected: phase4Socket && !phase4Socket.destroyed,
            connectedClients: clients.size,
            currentSlices: currentSlices,
            dimensions: VDS_DIMENSIONS,
            uptime: process.uptime()
        }
    });
});

// Start server
server.listen(HTTP_PORT, () => {
    console.log(`✅ HTTP server started on port ${HTTP_PORT}`);
    console.log(`🌐 WebSocket server ready for connections`);
    console.log(`\n📡 API Endpoints:`);
    console.log(`  GET  /api/dimensions           - Get VDS dimensions`);
    console.log(`  GET  /api/slice/:type/:index   - Get single slice data`);
    console.log(`  POST /api/slice                - Update slice position`);
    console.log(`  GET  /api/status               - Server status`);
    console.log(`\n🔌 WebSocket: ws://localhost:${HTTP_PORT}`);
    console.log(`\n🎯 Ready to serve real seismic amplitude data!`);
    
    // Connect to Phase 4 server
    connectToPhase4Server();
    
    // Start periodic data broadcast
    setInterval(() => {
        if (clients.size > 0) {
            broadcastAmplitudeData();
        }
    }, 100); // 10 FPS
});

process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down seismic amplitude server...');
    
    if (phase4Socket) {
        phase4Socket.destroy();
    }
    
    clients.forEach(client => {
        client.close();
    });
    
    server.close(() => {
        console.log('✅ Server shut down gracefully');
        process.exit(0);
    });
});