/**
 * BluStream Phase 5 - WebRTC Client Library
 * 
 * Handles WebRTC peer connections, signaling, and real-time
 * seismic data streaming for browser-based clients.
 */

class BluStreamWebRTCClient extends EventTarget {
    constructor() {
        super();
        
        // WebRTC configuration
        this.rtcConfig = {
            iceServers: [
                { urls: 'stun:stun.l.google.com:19302' },
                { urls: 'stun:stun1.l.google.com:19302' },
                { urls: 'stun:stun2.l.google.com:19302' }
            ],
            iceCandidatePoolSize: 10
        };
        
        // Connection state
        this.peerConnection = null;
        this.websocket = null;
        this.sessionId = null;
        this.isConnected = false;
        this.isStreaming = false;
        this.autoReconnect = true;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 5;
        
        // Stream configuration
        this.streamConfig = {
            orientation: 'XZ',
            animate: true,
            animationSpeed: 1.0,
            animationDuration: 30,
            quality: 'auto',
            fps: 'auto'
        };
        
        // Performance tracking
        this.stats = {
            packetsLost: 0,
            jitter: 0,
            rtt: 0,
            bitrate: 0,
            fps: 0,
            resolution: '',
            latency: 0,
            hwAccelerated: false
        };
        
        // Bind methods
        this.handleSignalingMessage = this.handleSignalingMessage.bind(this);
        this.handleWebSocketOpen = this.handleWebSocketOpen.bind(this);
        this.handleWebSocketClose = this.handleWebSocketClose.bind(this);
        this.handleWebSocketError = this.handleWebSocketError.bind(this);
        
        console.log('🎬 BluStream WebRTC Client initialized');
    }
    
    /**
     * Helper method to emit events with proper CustomEvent syntax
     */
    emit(eventType, detail) {
        this.dispatchEvent(new CustomEvent(eventType, { detail }));
    }
    
    /**
     * Connect to the signaling server and establish WebRTC session
     */
    async connect(serverUrl, sessionId = null) {
        try {
            console.log(`🔗 Connecting to signaling server: ${serverUrl}`);
            this.serverUrl = serverUrl;
            this.emit('connecting', { serverUrl, sessionId });
            
            // Connect to signaling server
            await this.connectWebSocket(serverUrl);
            
            // Create or join session
            if (sessionId) {
                this.sessionId = sessionId;
                await this.joinSession(sessionId);
            } else {
                await this.createSession();
            }
            
            // Initialize WebRTC peer connection
            await this.initializePeerConnection();
            
            this.isConnected = true;
            this.reconnectAttempts = 0;
            
            console.log('✅ Connected to BluStream server');
            this.emit('connected', { sessionId: this.sessionId });
            
        } catch (error) {
            console.error('❌ Connection failed:', error);
            this.emit('error', { type: 'connection_failed', error });
            
            if (this.autoReconnect && this.reconnectAttempts < this.maxReconnectAttempts) {
                this.scheduleReconnect();
            }
        }
    }
    
    /**
     * Disconnect from server and clean up resources
     */
    disconnect() {
        console.log('🔌 Disconnecting from BluStream server');
        
        this.isConnected = false;
        this.isStreaming = false;
        this.autoReconnect = false;
        
        if (this.peerConnection) {
            this.peerConnection.close();
            this.peerConnection = null;
        }
        
        if (this.websocket) {
            this.websocket.close();
            this.websocket = null;
        }
        
        this.emit('disconnected');
    }
    
    /**
     * Connect to WebSocket signaling server
     */
    async connectWebSocket(serverUrl) {
        return new Promise((resolve, reject) => {
            const httpUrl = serverUrl.replace('ws://', 'http://').replace('wss://', 'https://');
            this.websocket = io(httpUrl, {
                transports: ['websocket'],
                forceNew: true
            });
            
            this.websocket.on('connect', () => {
                console.log('📡 WebSocket connected');
                this.handleWebSocketOpen();
                resolve();
            });
            
            this.websocket.on('disconnect', this.handleWebSocketClose);
            this.websocket.on('connect_error', (error) => {
                console.error('🚫 WebSocket connection error:', error);
                reject(error);
            });
            
            // Signaling message handlers
            this.websocket.on('session-joined', this.handleSignalingMessage);
            this.websocket.on('offer', this.handleSignalingMessage);
            this.websocket.on('answer', this.handleSignalingMessage);
            this.websocket.on('ice-candidate', this.handleSignalingMessage);
            this.websocket.on('error', this.handleSignalingMessage);
        });
    }
    
    /**
     * Create a new streaming session
     */
    async createSession() {
        console.log('📋 Creating new session');
        
        try {
            const serverBaseUrl = this.serverUrl || 'http://localhost:3001';
            const response = await fetch(`${serverBaseUrl}/api/sessions`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    vdsConfig: {},
                    orientation: this.streamConfig.orientation,
                    animate: this.streamConfig.animate,
                    animationDuration: this.streamConfig.animationDuration,
                    quality: this.streamConfig.quality
                })
            });
            
            const data = await response.json();
            this.sessionId = data.sessionId;
            
            console.log(`✅ Session created: ${this.sessionId}`);
            return this.sessionId;
            
        } catch (error) {
            console.error('❌ Failed to create session:', error);
            throw error;
        }
    }
    
    /**
     * Join existing session
     */
    async joinSession(sessionId) {
        console.log(`📋 Joining session: ${sessionId}`);
        
        this.websocket.emit('join-session', { sessionId });
        
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error('Session join timeout'));
            }, 10000);
            
            const handleJoined = (data) => {
                clearTimeout(timeout);
                this.websocket.off('session-joined', handleJoined);
                this.websocket.off('error', handleError);
                
                this.streamConfig = { ...this.streamConfig, ...data.streamConfig };
                console.log(`✅ Joined session: ${sessionId}`);
                resolve(data);
            };
            
            const handleError = (error) => {
                clearTimeout(timeout);
                this.websocket.off('session-joined', handleJoined);
                this.websocket.off('error', handleError);
                reject(error);
            };
            
            this.websocket.on('session-joined', handleJoined);
            this.websocket.on('error', handleError);
        });
    }
    
    /**
     * Initialize WebRTC peer connection
     */
    async initializePeerConnection() {
        console.log('🔗 Initializing WebRTC peer connection');
        
        this.peerConnection = new RTCPeerConnection(this.rtcConfig);
        
        // Handle incoming media streams
        this.peerConnection.ontrack = (event) => {
            console.log('📹 Received remote stream');
            this.handleRemoteStream(event);
        };
        
        // Handle ICE candidates
        this.peerConnection.onicecandidate = (event) => {
            if (event.candidate) {
                console.log('🧊 Sending ICE candidate');
                this.websocket.emit('ice-candidate', {
                    sessionId: this.sessionId,
                    candidate: event.candidate
                });
            }
        };
        
        // Handle connection state changes
        this.peerConnection.onconnectionstatechange = () => {
            const state = this.peerConnection.connectionState;
            console.log(`🔗 Peer connection state: ${state}`);
            
            this.emit('connectionStateChange', { state });
            
            if (state === 'connected') {
                this.isStreaming = true;
                this.startStatsCollection();
                this.websocket.emit('peer-connected');
            } else if (state === 'disconnected' || state === 'failed') {
                this.isStreaming = false;
                this.websocket.emit('peer-disconnected');
                
                if (this.autoReconnect) {
                    this.scheduleReconnect();
                }
            }
        };
        
        // Create offer to start streaming
        await this.createOffer();
    }
    
    /**
     * Create WebRTC offer
     */
    async createOffer() {
        try {
            console.log('📤 Creating WebRTC offer');
            
            const offer = await this.peerConnection.createOffer({
                offerToReceiveVideo: true,
                offerToReceiveAudio: false
            });
            
            await this.peerConnection.setLocalDescription(offer);
            
            this.websocket.emit('offer', {
                sessionId: this.sessionId,
                offer: offer
            });
            
        } catch (error) {
            console.error('❌ Failed to create offer:', error);
            throw error;
        }
    }
    
    /**
     * Handle signaling messages
     */
    handleSignalingMessage(data) {
        const { type } = data;
        
        switch (type) {
            case 'answer':
                this.handleAnswer(data);
                break;
            case 'ice-candidate':
                this.handleIceCandidate(data);
                break;
            case 'error':
                this.handleSignalingError(data);
                break;
            default:
                console.log('📨 Signaling message:', type, data);
        }
    }
    
    /**
     * Handle WebRTC answer
     */
    async handleAnswer(data) {
        try {
            console.log('📥 Received WebRTC answer');
            
            const answer = new RTCSessionDescription(data.answer);
            await this.peerConnection.setRemoteDescription(answer);
            
        } catch (error) {
            console.error('❌ Failed to handle answer:', error);
        }
    }
    
    /**
     * Handle ICE candidate
     */
    async handleIceCandidate(data) {
        try {
            if (data.candidate) {
                console.log('🧊 Adding ICE candidate');
                await this.peerConnection.addIceCandidate(data.candidate);
            }
        } catch (error) {
            console.error('❌ Failed to add ICE candidate:', error);
        }
    }
    
    /**
     * Handle remote media stream
     */
    handleRemoteStream(event) {
        const stream = event.streams[0];
        
        console.log('📹 Remote stream received:', stream);
        this.emit('stream', { stream });
        
        // Update stream statistics
        const track = stream.getVideoTracks()[0];
        if (track) {
            const settings = track.getSettings();
            this.stats.resolution = `${settings.width}x${settings.height}`;
            this.stats.fps = settings.frameRate;
        }
    }
    
    /**
     * Send control message to server
     */
    sendControlMessage(controlType, controlData) {
        if (!this.websocket || !this.sessionId) {
            console.warn('⚠️ Cannot send control message: not connected');
            return;
        }
        
        console.log(`🎮 Sending control: ${controlType}`, controlData);
        
        this.websocket.emit('control-message', {
            sessionId: this.sessionId,
            controlType,
            controlData
        });
        
        // Update local config
        switch (controlType) {
            case 'slice-orientation':
                this.streamConfig.orientation = controlData.orientation;
                break;
            case 'animation-speed':
                this.streamConfig.animationSpeed = controlData.speed;
                break;
            case 'pause-resume':
                this.streamConfig.animate = !controlData.paused;
                break;
            case 'quality-level':
                this.streamConfig.quality = controlData.quality;
                break;
        }
        
        this.emit('controlSent', { controlType, controlData });
    }
    
    /**
     * Update stream configuration
     */
    updateStreamConfig(config) {
        const oldConfig = { ...this.streamConfig };
        this.streamConfig = { ...this.streamConfig, ...config };
        
        console.log('⚙️ Stream config updated:', this.streamConfig);
        this.emit('configUpdated', { oldConfig, newConfig: this.streamConfig });
    }
    
    /**
     * Start collecting WebRTC statistics
     */
    startStatsCollection() {
        if (!this.peerConnection) return;
        
        const collectStats = async () => {
            try {
                const stats = await this.peerConnection.getStats();
                this.processStats(stats);
                
                if (this.isStreaming) {
                    setTimeout(collectStats, 1000); // Collect every second
                }
            } catch (error) {
                console.error('📊 Stats collection error:', error);
            }
        };
        
        collectStats();
    }
    
    /**
     * Process WebRTC statistics
     */
    processStats(statsReport) {
        let inboundRtp = null;
        let remoteInboundRtp = null;
        
        for (const report of statsReport.values()) {
            if (report.type === 'inbound-rtp' && report.mediaType === 'video') {
                inboundRtp = report;
            } else if (report.type === 'remote-inbound-rtp') {
                remoteInboundRtp = report;
            }
        }
        
        if (inboundRtp) {
            // Calculate bitrate
            if (this.lastStats && this.lastStats.bytesReceived) {
                const deltaBytes = inboundRtp.bytesReceived - this.lastStats.bytesReceived;
                const deltaTime = inboundRtp.timestamp - this.lastStats.timestamp;
                this.stats.bitrate = Math.round((deltaBytes * 8) / (deltaTime / 1000) / 1000); // kbps
            }
            
            // Update other stats
            this.stats.packetsLost = inboundRtp.packetsLost || 0;
            this.stats.jitter = Math.round((inboundRtp.jitter || 0) * 1000); // ms
            this.stats.fps = inboundRtp.framesPerSecond || 0;
            
            this.lastStats = {
                bytesReceived: inboundRtp.bytesReceived,
                timestamp: inboundRtp.timestamp
            };
        }
        
        if (remoteInboundRtp) {
            this.stats.rtt = remoteInboundRtp.roundTripTime ? 
                Math.round(remoteInboundRtp.roundTripTime * 1000) : 0; // ms
        }
        
        // Estimate latency (rough calculation)
        this.stats.latency = this.stats.rtt + this.stats.jitter + 50; // Add processing time
        
        this.emit('statsUpdated', { stats: this.stats });
    }
    
    /**
     * Handle WebSocket connection open
     */
    handleWebSocketOpen() {
        console.log('✅ WebSocket connection established');
        this.emit('signalingConnected');
    }
    
    /**
     * Handle WebSocket connection close
     */
    handleWebSocketClose() {
        console.log('❌ WebSocket connection closed');
        this.emit('signalingDisconnected');
        
        if (this.autoReconnect && this.isConnected) {
            this.scheduleReconnect();
        }
    }
    
    /**
     * Handle WebSocket error
     */
    handleWebSocketError(error) {
        console.error('🚫 WebSocket error:', error);
        this.emit('signalingError', { error });
    }
    
    /**
     * Handle signaling error
     */
    handleSignalingError(data) {
        console.error('🚫 Signaling error:', data);
        this.emit('error', { type: 'signaling_error', error: data });
    }
    
    /**
     * Schedule reconnection attempt
     */
    scheduleReconnect() {
        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            console.error('❌ Max reconnection attempts reached');
            this.emit('error', { type: 'max_reconnects_reached' });
            return;
        }
        
        this.reconnectAttempts++;
        const delay = Math.min(1000 * Math.pow(2, this.reconnectAttempts), 30000); // Exponential backoff
        
        console.log(`🔄 Reconnecting in ${delay}ms (attempt ${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
        
        setTimeout(() => {
            if (this.autoReconnect) {
                this.connect(this.websocket?.io?.uri, this.sessionId);
            }
        }, delay);
    }
    
    /**
     * Get current connection status
     */
    getStatus() {
        return {
            connected: this.isConnected,
            streaming: this.isStreaming,
            sessionId: this.sessionId,
            peerConnectionState: this.peerConnection?.connectionState || 'closed',
            signalingState: this.peerConnection?.signalingState || 'closed',
            stats: this.stats,
            config: this.streamConfig
        };
    }
    
    /**
     * Get available browser capabilities
     */
    static async getBrowserCapabilities() {
        const capabilities = {
            webrtc: false,
            h264: false,
            vp8: false,
            vp9: false,
            av1: false,
            hardwareAcceleration: false
        };
        
        // Check WebRTC support
        if (typeof RTCPeerConnection !== 'undefined') {
            capabilities.webrtc = true;
            
            // Check codec support
            try {
                const pc = new RTCPeerConnection();
                const offer = await pc.createOffer({ offerToReceiveVideo: true });
                const sdp = offer.sdp;
                
                capabilities.h264 = sdp.includes('H264');
                capabilities.vp8 = sdp.includes('VP8');
                capabilities.vp9 = sdp.includes('VP9');
                capabilities.av1 = sdp.includes('AV01');
                
                pc.close();
            } catch (error) {
                console.warn('Could not detect codec support:', error);
            }
        }
        
        // Check hardware acceleration (basic detection)
        if (typeof VideoDecoder !== 'undefined') {
            try {
                const config = { codec: 'avc1.42E01E', hardwareAcceleration: 'prefer-hardware' };
                const supported = await VideoDecoder.isConfigSupported(config);
                capabilities.hardwareAcceleration = supported.supported;
            } catch (error) {
                // Hardware acceleration detection not available
            }
        }
        
        return capabilities;
    }
}

// Export for module systems
if (typeof module !== 'undefined' && module.exports) {
    module.exports = BluStreamWebRTCClient;
}

// Global registration for browser
if (typeof window !== 'undefined') {
    window.BluStreamWebRTCClient = BluStreamWebRTCClient;
}

console.log('📦 BluStream WebRTC Client library loaded');