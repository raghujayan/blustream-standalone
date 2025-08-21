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
        this.server.listen(this.port, () => {
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