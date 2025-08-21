/**
 * BluStream Phase 5 - WebRTC Signaling Server
 * 
 * Handles WebRTC peer connection signaling, session management,
 * and control message routing for browser-based seismic streaming.
 */

const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const { v4: uuidv4 } = require('uuid');
const cors = require('cors');
const VDSFrameService = require('./vds-frame-service');

class BluStreamSignalingServer {
    constructor(port = 3000) {
        this.port = port;
        this.app = express();
        this.server = http.createServer(this.app);
        this.io = socketIo(this.server, {
            cors: {
                origin: "*",
                methods: ["GET", "POST"]
            }
        });
        
        // Session management
        this.sessions = new Map();
        this.clients = new Map();
        
        // VDS Frame Service for real-time seismic data
        this.vdsService = new VDSFrameService();
        this.setupVDSService();
        
        this.setupMiddleware();
        this.setupRoutes();
        this.setupSocketHandlers();
    }
    
    setupMiddleware() {
        this.app.use(cors());
        this.app.use(express.json());
        this.app.use(express.static('../browser-client'));
    }
    
    setupRoutes() {
        // Video streaming endpoints
        this.app.get('/video/:filename', (req, res) => {
            const filename = req.params.filename;
            const videoPath = `/home/rocky/blustream-unified/${filename}`;
            
            console.log(`📹 Streaming video: ${filename}`);
            
            const fs = require('fs');
            const path = require('path');
            
            if (!fs.existsSync(videoPath)) {
                return res.status(404).json({ error: 'Video not found' });
            }
            
            const stat = fs.statSync(videoPath);
            const fileSize = stat.size;
            const range = req.headers.range;
            
            if (range) {
                const parts = range.replace(/bytes=/, "").split("-");
                const start = parseInt(parts[0], 10);
                const end = parts[1] ? parseInt(parts[1], 10) : fileSize-1;
                const chunksize = (end-start)+1;
                const file = fs.createReadStream(videoPath, {start, end});
                const head = {
                    'Content-Range': `bytes ${start}-${end}/${fileSize}`,
                    'Accept-Ranges': 'bytes',
                    'Content-Length': chunksize,
                    'Content-Type': 'video/mp4',
                };
                res.writeHead(206, head);
                file.pipe(res);
            } else {
                const head = {
                    'Content-Length': fileSize,
                    'Content-Type': 'video/mp4',
                };
                res.writeHead(200, head);
                fs.createReadStream(videoPath).pipe(res);
            }
        });
        
        // VDS frame streaming endpoint
        this.app.get('/vds-frame/:frameNumber', (req, res) => {
            const fs = require('fs');
            const frameNumber = parseInt(req.params.frameNumber);
            const frameFile = `/tmp/blustream-frames/frame_${frameNumber.toString().padStart(6, '0')}.png`;
            
            console.log(`🖼️  Serving VDS frame: ${frameNumber}`);
            
            if (!fs.existsSync(frameFile)) {
                return res.status(404).json({ error: 'Frame not found' });
            }
            
            res.sendFile(frameFile);
        });
        
        // VDS service status endpoint
        this.app.get('/vds-status', (req, res) => {
            const status = this.vdsService.getStatus();
            res.json({
                ...status,
                uptime: process.uptime(),
                timestamp: new Date().toISOString()
            });
        });
        
        // Health check endpoint
        this.app.get('/health', (req, res) => {
            res.json({
                status: 'healthy',
                uptime: process.uptime(),
                sessions: this.sessions.size,
                clients: this.clients.size,
                timestamp: new Date().toISOString()
            });
        });
        
        // Session management API
        this.app.post('/api/sessions', (req, res) => {
            const sessionId = uuidv4();
            const session = {
                id: sessionId,
                created: new Date(),
                clients: [],
                vdsConfig: req.body.vdsConfig || {},
                streamConfig: {
                    orientation: req.body.orientation || 'XZ',
                    animate: req.body.animate !== false,
                    animationDuration: req.body.animationDuration || 30,
                    quality: req.body.quality || 'high'
                }
            };
            
            this.sessions.set(sessionId, session);
            
            res.json({
                sessionId,
                session: {
                    id: session.id,
                    created: session.created,
                    streamConfig: session.streamConfig
                }
            });
        });
        
        this.app.get('/api/sessions/:sessionId', (req, res) => {
            const session = this.sessions.get(req.params.sessionId);
            if (!session) {
                return res.status(404).json({ error: 'Session not found' });
            }
            
            res.json({
                session: {
                    id: session.id,
                    created: session.created,
                    clientCount: session.clients.length,
                    streamConfig: session.streamConfig
                }
            });
        });
    }
    
    /**
     * Setup VDS Frame Service integration
     */
    setupVDSService() {
        // Handle frame generation events
        this.vdsService.on('frame-generated', (data) => {
            // Broadcast frame to all connected clients
            this.io.emit('vds-frame', {
                frameNumber: data.frameNumber,
                frameFile: data.frameData.frameFile,
                sliceIndex: data.frameData.sliceIndex,
                animationTime: data.frameData.animationTime,
                orientation: data.frameData.orientation,
                stats: data.stats,
                timestamp: data.timestamp
            });
        });
        
        this.vdsService.on('service-started', (data) => {
            console.log('🎬 VDS Frame Service started with config:', data.config);
            this.io.emit('vds-service-status', { status: 'started', config: data.config });
        });
        
        this.vdsService.on('service-error', (data) => {
            console.error('❌ VDS Frame Service error:', data.error);
            this.io.emit('vds-service-status', { status: 'error', error: data.error.message });
        });
        
        this.vdsService.on('client-connected', (data) => {
            console.log(`📡 VDS client connected: ${data.clientId}`);
        });
        
        console.log('✅ VDS Frame Service integration configured');
    }
    
    setupSocketHandlers() {
        this.io.on('connection', (socket) => {
            console.log(`🔗 Client connected: ${socket.id}`);
            
            // Store client info
            this.clients.set(socket.id, {
                id: socket.id,
                connected: new Date(),
                sessionId: null,
                peerConnected: false
            });
            
            // Join session
            socket.on('join-session', (data) => {
                this.handleJoinSession(socket, data);
            });
            
            // WebRTC signaling
            socket.on('offer', (data) => {
                this.handleOffer(socket, data);
            });
            
            socket.on('answer', (data) => {
                this.handleAnswer(socket, data);
            });
            
            socket.on('ice-candidate', (data) => {
                this.handleIceCandidate(socket, data);
            });
            
            // VDS control messages
            socket.on('control-message', (data) => {
                this.handleControlMessage(socket, data);
            });
            
            // VDS streaming controls
            socket.on('start-vds-stream', (data) => {
                this.handleStartVDSStream(socket, data);
            });
            
            socket.on('stop-vds-stream', () => {
                this.handleStopVDSStream(socket);
            });
            
            socket.on('update-vds-config', (data) => {
                this.handleUpdateVDSConfig(socket, data);
            });
            
            // Client status updates
            socket.on('peer-connected', () => {
                this.handlePeerConnected(socket);
            });
            
            socket.on('peer-disconnected', () => {
                this.handlePeerDisconnected(socket);
            });
            
            // Disconnect handling
            socket.on('disconnect', () => {
                this.handleDisconnect(socket);
            });
        });
    }
    
    handleJoinSession(socket, data) {
        const { sessionId } = data;
        const session = this.sessions.get(sessionId);
        
        if (!session) {
            socket.emit('error', { message: 'Session not found', code: 'SESSION_NOT_FOUND' });
            return;
        }
        
        // Add client to session
        const client = this.clients.get(socket.id);
        client.sessionId = sessionId;
        session.clients.push(socket.id);
        
        // Join socket room
        socket.join(sessionId);
        
        console.log(`📋 Client ${socket.id} joined session ${sessionId}`);
        
        // Send session configuration
        socket.emit('session-joined', {
            sessionId,
            streamConfig: session.streamConfig,
            clientCount: session.clients.length
        });
        
        // Notify other clients in session
        socket.to(sessionId).emit('client-joined', {
            clientId: socket.id,
            clientCount: session.clients.length
        });
    }
    
    handleOffer(socket, data) {
        const { sessionId, offer } = data;
        const client = this.clients.get(socket.id);
        
        if (!client || client.sessionId !== sessionId) {
            socket.emit('error', { message: 'Not in session', code: 'NOT_IN_SESSION' });
            return;
        }
        
        console.log(`📤 Relaying offer from ${socket.id} to BluStream server`);
        
        // In a full implementation, this would forward to the BluStream server
        // For now, we'll simulate an answer
        setTimeout(() => {
            socket.emit('answer', {
                sessionId,
                answer: {
                    type: 'answer',
                    sdp: this.generateMockAnswer(offer.sdp)
                }
            });
        }, 100);
    }
    
    handleAnswer(socket, data) {
        const { sessionId, answer } = data;
        console.log(`📥 Received answer from BluStream server for ${socket.id}`);
        
        // Forward answer to client (this would be from BluStream server in full implementation)
        socket.emit('answer-received', { sessionId, answer });
    }
    
    handleIceCandidate(socket, data) {
        const { sessionId, candidate } = data;
        const client = this.clients.get(socket.id);
        
        if (!client || client.sessionId !== sessionId) {
            return;
        }
        
        console.log(`🧊 ICE candidate from ${socket.id}`);
        
        // Forward ICE candidate to BluStream server
        // In full implementation, this would go to the actual server
        socket.emit('ice-candidate-received', { sessionId, candidate });
    }
    
    handleControlMessage(socket, data) {
        const { sessionId, controlType, controlData } = data;
        const client = this.clients.get(socket.id);
        
        if (!client || client.sessionId !== sessionId) {
            return;
        }
        
        console.log(`🎮 Control message: ${controlType} from ${socket.id}`);
        
        // Forward control message to BluStream server
        // This would update VDS rendering parameters
        switch (controlType) {
            case 'slice-orientation':
                console.log(`  📐 Change orientation to: ${controlData.orientation}`);
                break;
            case 'animation-speed':
                console.log(`  ⚡ Animation speed: ${controlData.speed}x`);
                break;
            case 'quality-level':
                console.log(`  🔧 Quality level: ${controlData.quality}`);
                break;
            case 'pause-resume':
                console.log(`  ⏯️  Animation: ${controlData.paused ? 'PAUSED' : 'RESUMED'}`);
                break;
        }
        
        // Broadcast control changes to other clients in session
        socket.to(sessionId).emit('control-update', {
            clientId: socket.id,
            controlType,
            controlData
        });
    }
    
    handlePeerConnected(socket) {
        const client = this.clients.get(socket.id);
        if (client) {
            client.peerConnected = true;
            console.log(`✅ Peer connection established for ${socket.id}`);
            
            if (client.sessionId) {
                socket.to(client.sessionId).emit('peer-status-update', {
                    clientId: socket.id,
                    connected: true
                });
            }
        }
    }
    
    handlePeerDisconnected(socket) {
        const client = this.clients.get(socket.id);
        if (client) {
            client.peerConnected = false;
            console.log(`❌ Peer connection lost for ${socket.id}`);
            
            if (client.sessionId) {
                socket.to(client.sessionId).emit('peer-status-update', {
                    clientId: socket.id,
                    connected: false
                });
            }
        }
    }
    
    handleDisconnect(socket) {
        const client = this.clients.get(socket.id);
        
        if (client && client.sessionId) {
            const session = this.sessions.get(client.sessionId);
            if (session) {
                // Remove client from session
                session.clients = session.clients.filter(id => id !== socket.id);
                
                // Notify other clients
                socket.to(client.sessionId).emit('client-left', {
                    clientId: socket.id,
                    clientCount: session.clients.length
                });
                
                // Clean up empty sessions
                if (session.clients.length === 0) {
                    this.sessions.delete(client.sessionId);
                    console.log(`🗑️  Removed empty session: ${client.sessionId}`);
                }
            }
        }
        
        this.clients.delete(socket.id);
        console.log(`👋 Client disconnected: ${socket.id}`);
    }
    
    /**
     * Handle VDS streaming start request
     */
    handleStartVDSStream(socket, data) {
        const client = this.clients.get(socket.id);
        if (!client || !client.sessionId) {
            socket.emit('error', { message: 'Not in session', code: 'NOT_IN_SESSION' });
            return;
        }
        
        console.log(`🎬 Starting VDS stream for client: ${socket.id}`);
        
        // Add client to VDS service
        this.vdsService.addClient(socket.id);
        
        // Start VDS service if not already running
        if (!this.vdsService.getStatus().isStreaming) {
            const vdsConfig = {
                orientation: data.orientation || 'XZ',
                frameRate: data.frameRate || 30,
                resolution: data.resolution || { width: 1920, height: 1080 },
                quality: data.quality || 'high',
                animate: data.animate !== false
            };
            
            this.vdsService.startService(vdsConfig).then(() => {
                socket.emit('vds-stream-started', {
                    sessionId: client.sessionId,
                    config: vdsConfig
                });
            }).catch(error => {
                console.error('❌ Failed to start VDS service:', error);
                socket.emit('error', { 
                    message: 'Failed to start VDS streaming', 
                    code: 'VDS_START_FAILED',
                    error: error.message
                });
            });
        } else {
            // Service already running, just notify client
            socket.emit('vds-stream-started', {
                sessionId: client.sessionId,
                config: this.vdsService.getStatus().config
            });
        }
    }
    
    /**
     * Handle VDS streaming stop request
     */
    handleStopVDSStream(socket) {
        console.log(`🛑 Stopping VDS stream for client: ${socket.id}`);
        
        // Remove client from VDS service
        this.vdsService.removeClient(socket.id);
        
        socket.emit('vds-stream-stopped', {
            clientId: socket.id
        });
    }
    
    /**
     * Handle VDS configuration update
     */
    handleUpdateVDSConfig(socket, data) {
        const client = this.clients.get(socket.id);
        if (!client || !client.sessionId) {
            return;
        }
        
        console.log(`⚙️ Updating VDS config for client ${socket.id}:`, data);
        
        // Update VDS service configuration
        this.vdsService.updateConfig(data);
        
        // Notify all clients in session about config change
        this.io.to(client.sessionId).emit('vds-config-updated', {
            clientId: socket.id,
            config: data
        });
    }
    
    // Mock SDP answer generation (replace with actual WebRTC integration)
    generateMockAnswer(offerSdp) {
        return `v=0
o=- 4611731400430051336 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE video
a=msid-semantic:WMS
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:mock
a=ice-pwd:mockpassword
a=ice-options:trickle
a=fingerprint:sha-256 MOCK:FINGERPRINT
a=setup:active
a=mid:video
a=extmap:1 urn:ietf:params:rtp-hdrext:toffset
a=sendonly
a=rtcp-mux
a=rtpmap:96 H264/90000
a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f`;
    }
    
    start() {
        this.server.listen(this.port, '0.0.0.0', () => {
            console.log('🚀 BluStream WebRTC Signaling Server started');
            console.log(`📡 Server running on http://localhost:${this.port}`);
            console.log(`🌐 Browser client available at http://localhost:${this.port}`);
            console.log('');
            console.log('📊 Server Status:');
            console.log(`   Sessions: ${this.sessions.size}`);
            console.log(`   Connected clients: ${this.clients.size}`);
            console.log('');
            console.log('🔗 API Endpoints:');
            console.log(`   GET  /health - Server health check`);
            console.log(`   POST /api/sessions - Create new session`);
            console.log(`   GET  /api/sessions/:id - Get session info`);
            console.log('');
            console.log('Ready for Phase 5 WebRTC connections! 🎬');
        });
    }
    
    stop() {
        this.server.close(() => {
            console.log('🛑 BluStream Signaling Server stopped');
        });
    }
}

// Start server if run directly
if (require.main === module) {
    const port = process.env.PORT || 3000;
    const server = new BluStreamSignalingServer(port);
    server.start();
    
    // Graceful shutdown
    process.on('SIGINT', () => {
        console.log('\n🛑 Received SIGINT, shutting down gracefully...');
        server.stop();
        process.exit(0);
    });
}

module.exports = BluStreamSignalingServer;