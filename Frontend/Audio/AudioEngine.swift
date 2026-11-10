import AVFoundation

/// Output stage for the console's audio.
///
/// On hardware the MCPX audio processor mixes voices and hands 48 kHz stereo
/// PCM to the AC97 codec. The core does the same and writes the result into
/// a lock-free ring buffer; an AVAudioSourceNode pulls from it on the audio
/// I/O thread. The render block calls the C API directly — no Objective-C
/// messaging, locks or allocation on the real-time thread — and the I/O
/// buffer is requested at ~5 ms, so added latency is one small hardware
/// buffer plus whatever the ring holds.
final class AudioEngine {
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private var observers: [NSObjectProtocol] = []

    func start(core: EmulatorCore) throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try session.setPreferredSampleRate(Double(EM_AUDIO_SAMPLE_RATE))
        try session.setPreferredIOBufferDuration(0.005)
        try session.setActive(true)

        guard let format = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(EM_AUDIO_SAMPLE_RATE),
                                         channels: AVAudioChannelCount(EM_AUDIO_CHANNELS), interleaved: true) else {
            throw NSError(domain: "app.ember.audio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Could not create the audio format."])
        }

        // OpaquePointer is copied into the block; the session keeps the core
        // alive for as long as the engine runs (see EmulatorSession.stop()).
        let handle = core.handle
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let out = buffers[0].mData?.assumingMemoryBound(to: Int16.self) else { return noErr }
            _ = em_core_read_audio(handle, out, Int(frameCount)) // zero-fills on underrun
            return noErr
        }
        engine.attach(node)
        // The mixer converts Int16/48k to the hardware format if they differ.
        engine.connect(node, to: engine.mainMixerNode, format: format)
        source = node

        engine.prepare()
        try engine.start()
        observeInterruptions()
    }

    func stop() {
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        engine.stop()
        if let source = source { engine.detach(source) }
        source = nil
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
    }

    func setPaused(_ paused: Bool) {
        if paused {
            engine.pause()
        } else if !engine.isRunning {
            try? engine.start()
        }
    }

    /// Phone calls, Siri and route changes (headphones unplugged) stop the
    /// engine; restart it when the system hands audio back.
    private func observeInterruptions() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }
            try? AVAudioSession.sharedInstance().setActive(true)
            try? self?.engine.start()
        })
        observers.append(center.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
            try? self?.engine.start()
        })
    }
}
