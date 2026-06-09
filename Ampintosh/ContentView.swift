import SwiftUI
import AVFoundation
import CryptoKit
import Accelerate
import UniformTypeIdentifiers
import Playgrounds
#if os(macOS)
import AppKit
import CoreAudio
#endif

@main struct AmpintoshApp: App {
    init() {
        #if os(macOS)
        if let iconURL = Bundle.main.url(forResource: "AmpintoshIcon", withExtension: "png"),
           let icon = NSImage(contentsOf: iconURL) {
            NSApplication.shared.applicationIconImage = icon
        }
        #endif
    }

    var body: some Scene {
        WindowGroup("Ampintosh") {
            ContentView()
                .frame(minWidth: 820, minHeight: 540)
        }
        .windowResizability(.contentSize)
        .commands {
            AmpintoshCommands()
        }
    }
}

struct AmpintoshCommands: Commands {
    @FocusedValue(\.openAudioFiles) private var openAudioFiles
    @FocusedValue(\.clearPlaylist) private var clearPlaylist

    var body: some Commands {
        CommandGroup(after: .newItem) {
            Button("Open Audio or Playlist Files...") {
                openAudioFiles?()
            }
            .keyboardShortcut("o", modifiers: [.command])
            .disabled(openAudioFiles == nil)

            Button("Clear Playlist") {
                clearPlaylist?()
            }
            .keyboardShortcut(.delete, modifiers: [.command])
            .disabled(clearPlaylist == nil)
        }
    }
}

extension FocusedValues {
    @Entry var openAudioFiles: (() -> Void)?
    @Entry var clearPlaylist: (() -> Void)?
}

struct AudioOutputDevice: Equatable {
    var name: String
    var iconName: String

    static let unknown = AudioOutputDevice(name: "Output unavailable", iconName: "speaker.slash.fill")

    #if os(macOS)
    static func current() -> AudioOutputDevice {
        var deviceID = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID) == noErr,
              deviceID != 0 else {
            return .unknown
        }

        let name = propertyString(deviceID: deviceID, selector: kAudioObjectPropertyName) ?? "Default Output"
        let transport = propertyUInt32(deviceID: deviceID, selector: kAudioDevicePropertyTransportType)
        return AudioOutputDevice(name: name, iconName: iconName(for: name, transport: transport))
    }

    private static func propertyString(deviceID: AudioDeviceID, selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)

        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value) == noErr else {
            return nil
        }

        return value?.takeUnretainedValue() as String?
    }

    private static func propertyUInt32(deviceID: AudioDeviceID, selector: AudioObjectPropertySelector) -> UInt32? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value = UInt32(0)
        var size = UInt32(MemoryLayout<UInt32>.size)

        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value) == noErr else {
            return nil
        }

        return value
    }

    private static func iconName(for deviceName: String, transport: UInt32?) -> String {
        let name = deviceName.lowercased()

        if name.contains("airpods pro") {
            return "airpodspro"
        }

        if name.contains("airpods max") {
            return "airpodsmax"
        }

        if name.contains("airpods") {
            return "airpods"
        }

        if name.contains("headphone") || name.contains("headset") || name.contains("beats") {
            return "headphones"
        }

        if name.contains("soundbar") || name.contains("sound bar") || name.contains("homepod") {
            return "hifispeaker.fill"
        }

        if name.contains("display") || name.contains("monitor") || name.contains("lg ") || name.contains("ultrafine") || name.contains("ultrathin") {
            return "display"
        }

        if transport == kAudioDeviceTransportTypeBluetooth {
            return "hifispeaker.and.homepod.fill"
        }

        if transport == kAudioDeviceTransportTypeHDMI || transport == kAudioDeviceTransportTypeDisplayPort {
            return "display"
        }

        if name.contains("speaker") {
            return "speaker.wave.3.fill"
        }

        return "speaker.wave.2.fill"
    }
    #else
    static func current() -> AudioOutputDevice {
        .unknown
    }
    #endif
}

struct Track: Identifiable, Equatable {
    let id: UUID
    let url: URL
    let title: String
    let format: String
    let duration: TimeInterval
    let artist: String?
    let album: String?
    let artworkData: Data?
    let bitDepth: Int?
    let sampleRate: Double?
    let channelCount: Int?
    let bitRate: Double?
    let codec: String?

    init(
        id: UUID = UUID(),
        url: URL,
        title: String,
        format: String,
        duration: TimeInterval,
        artist: String?,
        album: String?,
        artworkData: Data?,
        bitDepth: Int?,
        sampleRate: Double?,
        channelCount: Int?,
        bitRate: Double?,
        codec: String?
    ) {
        self.id = id
        self.url = url
        self.title = title
        self.format = format
        self.duration = duration
        self.artist = artist
        self.album = album
        self.artworkData = artworkData
        self.bitDepth = bitDepth
        self.sampleRate = sampleRate
        self.channelCount = channelCount
        self.bitRate = bitRate
        self.codec = codec
    }

    var subtitle: String {
        if let artist, let album {
            return "\(artist) - \(album)"
        }
        if let artist {
            return artist
        }
        if let album {
            return album
        }
        return url.deletingLastPathComponent().lastPathComponent
    }

    var metadataSummary: String {
        [artist, album].compactMap { $0 }.joined(separator: " / ")
    }

    var technicalSummary: String {
        var parts: [String] = []

        if let bitDepth {
            parts.append("\(bitDepth)-bit")
        }

        if let sampleRate {
            parts.append(String(format: "%.1f kHz", sampleRate / 1000))
        }

        if let channelCount {
            parts.append(channelCount == 1 ? "mono" : "\(channelCount) ch")
        }

        if let bitRate {
            parts.append("\(Int(bitRate / 1000)) kbps")
        }

        if let codec {
            parts.append(codec)
        }

        return parts.isEmpty ? "Metadata unavailable" : parts.joined(separator: " / ")
    }
}

struct LastFMConfiguration: Equatable {
    var isEnabled: Bool
    var apiKey: String
    var sharedSecret: String
    var sessionKey: String

    static let empty = LastFMConfiguration(isEnabled: false, apiKey: "", sharedSecret: "", sessionKey: "")

    var isReady: Bool {
        isEnabled && !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !sharedSecret.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !sessionKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }
}

struct LastFMSettingsStore {
    private enum Key {
        static let isEnabled = "Ampintosh.lastfm.enabled"
        static let apiKey = "Ampintosh.lastfm.apiKey"
        static let sharedSecret = "Ampintosh.lastfm.sharedSecret"
        static let sessionKey = "Ampintosh.lastfm.sessionKey"
    }

    static func load() -> LastFMConfiguration {
        LastFMConfiguration(
            isEnabled: UserDefaults.standard.bool(forKey: Key.isEnabled),
            apiKey: UserDefaults.standard.string(forKey: Key.apiKey) ?? "",
            sharedSecret: UserDefaults.standard.string(forKey: Key.sharedSecret) ?? "",
            sessionKey: UserDefaults.standard.string(forKey: Key.sessionKey) ?? ""
        )
    }

    static func save(_ configuration: LastFMConfiguration) {
        UserDefaults.standard.set(configuration.isEnabled, forKey: Key.isEnabled)
        UserDefaults.standard.set(configuration.apiKey.trimmingCharacters(in: .whitespacesAndNewlines), forKey: Key.apiKey)
        UserDefaults.standard.set(configuration.sharedSecret.trimmingCharacters(in: .whitespacesAndNewlines), forKey: Key.sharedSecret)
        UserDefaults.standard.set(configuration.sessionKey.trimmingCharacters(in: .whitespacesAndNewlines), forKey: Key.sessionKey)
    }
}

enum LastFMStatus: Equatable {
    case disabled
    case needsSetup
    case ready
    case nowPlaying
    case scrobbled
    case failed(String)

    var label: String {
        switch self {
        case .disabled: "LAST.FM OFF"
        case .needsSetup: "LAST.FM SETUP"
        case .ready: "LAST.FM READY"
        case .nowPlaying: "NOW PLAYING"
        case .scrobbled: "SCROBBLED"
        case .failed: "LAST.FM ERROR"
        }
    }

    var detail: String {
        switch self {
        case .disabled: "Last.fm scrobbling is disabled"
        case .needsSetup: "Add API key, shared secret, and session key"
        case .ready: "Last.fm scrobbling is ready"
        case .nowPlaying: "Current track sent to Last.fm"
        case .scrobbled: "Track play sent to Last.fm"
        case .failed(let message): message
        }
    }

    var isActive: Bool {
        switch self {
        case .ready, .nowPlaying, .scrobbled:
            true
        case .disabled, .needsSetup, .failed:
            false
        }
    }
}

struct LastFMClient {
    private let configuration: LastFMConfiguration
    private let endpoint = URL(string: "https://ws.audioscrobbler.com/2.0/")!

    init(configuration: LastFMConfiguration) {
        self.configuration = configuration
    }

    func updateNowPlaying(track: Track) async throws {
        var parameters = baseParameters(method: "track.updateNowPlaying", track: track)
        parameters["duration"] = String(Int(track.duration.rounded()))
        try await send(parameters: parameters)
    }

    func scrobble(track: Track, startedAt date: Date) async throws {
        var parameters = baseParameters(method: "track.scrobble", track: track)
        parameters["timestamp"] = String(Int(date.timeIntervalSince1970))
        parameters["duration"] = String(Int(track.duration.rounded()))
        try await send(parameters: parameters)
    }

    private func baseParameters(method: String, track: Track) -> [String: String] {
        var parameters = [
            "method": method,
            "api_key": configuration.apiKey.trimmingCharacters(in: .whitespacesAndNewlines),
            "sk": configuration.sessionKey.trimmingCharacters(in: .whitespacesAndNewlines),
            "track": track.title,
            "artist": track.artist?.isEmpty == false ? track.artist! : "Unknown Artist"
        ]

        if let album = track.album, !album.isEmpty {
            parameters["album"] = album
        }

        return parameters
    }

    private func send(parameters: [String: String]) async throws {
        var signedParameters = parameters
        signedParameters["api_sig"] = signature(for: parameters)
        signedParameters["format"] = "json"

        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded; charset=utf-8", forHTTPHeaderField: "Content-Type")
        request.httpBody = formEncoded(signedParameters).data(using: .utf8)

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse, (200..<300).contains(httpResponse.statusCode) else {
            throw LastFMError.transport
        }

        if let apiResponse = try? JSONDecoder().decode(LastFMAPIResponse.self, from: data),
           apiResponse.error != nil {
            throw LastFMError.api(apiResponse.message ?? "Last.fm rejected the request")
        }
    }

    private func signature(for parameters: [String: String]) -> String {
        let base = parameters
            .sorted { $0.key < $1.key }
            .map { $0.key + $0.value }
            .joined() + configuration.sharedSecret.trimmingCharacters(in: .whitespacesAndNewlines)
        let digest = Insecure.MD5.hash(data: Data(base.utf8))
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    private func formEncoded(_ parameters: [String: String]) -> String {
        parameters
            .sorted { $0.key < $1.key }
            .map { key, value in "\(Self.escape(key))=\(Self.escape(value))" }
            .joined(separator: "&")
    }

    private static func escape(_ string: String) -> String {
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: ":#[]@!$&'()*+,;=")
        return string.addingPercentEncoding(withAllowedCharacters: allowed) ?? string
    }
}

struct LastFMAPIResponse: Decodable {
    let error: Int?
    let message: String?
}

enum LastFMError: LocalizedError {
    case transport
    case api(String)

    var errorDescription: String? {
        switch self {
        case .transport: "Last.fm request failed"
        case .api(let message): message
        }
    }
}

/// Real-time audio analyzer. Runs an FFT on the live output tap to produce
/// genuine per-band frequency magnitudes, plus time-domain RMS, peak, and a
/// downsampled waveform. All heavy buffers are pre-allocated; the audio render
/// thread only fills them and copies results out under a short lock.
final class AudioAnalyzer {
    let bandCount: Int
    let wavePoints: Int

    private let fftSize: Int
    private let halfSize: Int
    private let log2n: vDSP_Length
    private let fftSetup: FFTSetup

    private var window: [Float]
    private var mono: [Float]
    private var windowed: [Float]
    private var realp: [Float]
    private var imagp: [Float]
    private var magnitudes: [Float]
    private var bandEdges: [Int]

    private let lock = NSLock()
    private var outBands: [Float]
    private var outWave: [Float]
    private var outRMS: Float = 0
    private var outPeak: Float = 0

    init(bandCount: Int = 32, fftSize: Int = 1024, wavePoints: Int = 168, sampleRate: Double = 44100) {
        self.bandCount = bandCount
        self.wavePoints = wavePoints
        self.fftSize = fftSize
        self.halfSize = fftSize / 2
        self.log2n = vDSP_Length(log2(Double(fftSize)))
        self.fftSetup = vDSP_create_fftsetup(log2n, FFTRadix(kFFTRadix2))!

        self.window = [Float](repeating: 0, count: fftSize)
        vDSP_hann_window(&window, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))
        self.mono = [Float](repeating: 0, count: fftSize)
        self.windowed = [Float](repeating: 0, count: fftSize)
        self.realp = [Float](repeating: 0, count: halfSize)
        self.imagp = [Float](repeating: 0, count: halfSize)
        self.magnitudes = [Float](repeating: 0, count: halfSize)
        self.outBands = [Float](repeating: 0, count: bandCount)
        self.outWave = [Float](repeating: 0, count: wavePoints)

        // Logarithmically spaced band edges (in FFT bins) across the audible range.
        let minHz = 32.0
        let maxHz = min(16_000.0, sampleRate / 2)
        let binHz = sampleRate / Double(fftSize)
        var edges: [Int] = []
        for b in 0...bandCount {
            let frac = Double(b) / Double(bandCount)
            let hz = minHz * pow(maxHz / minHz, frac)
            let bin = Int((hz / binHz).rounded())
            edges.append(min(max(bin, 1), halfSize - 1))
        }
        self.bandEdges = edges
    }

    deinit { vDSP_destroy_fftsetup(fftSetup) }

    /// Called on the audio render thread for every captured buffer.
    func process(_ buffer: AVAudioPCMBuffer) {
        guard let channelData = buffer.floatChannelData else { return }
        let channels = Int(buffer.format.channelCount)
        let frames = Int(buffer.frameLength)
        guard frames > 0 else { return }

        // Use the most recent `fftSize` samples, mixed to mono.
        let take = min(frames, fftSize)
        let offset = frames - take
        if take < fftSize {
            for i in take..<fftSize { mono[i] = 0 }
        }
        if channels >= 2 {
            let left = channelData[0]
            let right = channelData[1]
            for i in 0..<take { mono[i] = (left[offset + i] + right[offset + i]) * 0.5 }
        } else {
            let left = channelData[0]
            for i in 0..<take { mono[i] = left[offset + i] }
        }

        // Time-domain loudness (pre-window) for amplitude reactivity.
        var rms: Float = 0
        vDSP_rmsqv(mono, 1, &rms, vDSP_Length(fftSize))
        var peak: Float = 0
        vDSP_maxmgv(mono, 1, &peak, vDSP_Length(fftSize))

        // Window then forward real FFT.
        vDSP_vmul(mono, 1, window, 1, &windowed, 1, vDSP_Length(fftSize))
        windowed.withUnsafeBufferPointer { wp in
            wp.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfSize) { typed in
                realp.withUnsafeMutableBufferPointer { rp in
                    imagp.withUnsafeMutableBufferPointer { ip in
                        var split = DSPSplitComplex(realp: rp.baseAddress!, imagp: ip.baseAddress!)
                        vDSP_ctoz(typed, 2, &split, 1, vDSP_Length(halfSize))
                        vDSP_fft_zrip(fftSetup, &split, 1, log2n, FFTDirection(FFT_FORWARD))
                        magnitudes.withUnsafeMutableBufferPointer { mp in
                            vDSP_zvmags(&split, 1, mp.baseAddress!, 1, vDSP_Length(halfSize))
                        }
                    }
                }
            }
        }

        // Group bins into log-spaced bands, convert power -> dB -> normalized.
        var bands = [Float](repeating: 0, count: bandCount)
        for b in 0..<bandCount {
            let lo = bandEdges[b]
            let hi = max(bandEdges[b + 1], lo + 1)
            var sum: Float = 0
            var count = 0
            var bin = lo
            while bin < hi && bin < halfSize {
                sum += magnitudes[bin]
                count += 1
                bin += 1
            }
            let power = (count > 0 ? sum / Float(count) : 0) / Float(fftSize)
            let db = 10 * log10(power + 1e-9)
            var value = (db + 66) / 56          // ~ -66 dB..-10 dB -> 0..1
            value = min(max(value, 0), 1)
            value = powf(value, 1.35)            // gentle gamma for punchier peaks
            value *= 0.82 + 0.5 * Float(b) / Float(bandCount) // lift highs (music tilt)
            bands[b] = min(value, 1)
        }

        // Downsampled waveform for oscilloscope-style visualizers.
        var wave = [Float](repeating: 0, count: wavePoints)
        if take > 1 {
            for i in 0..<wavePoints {
                let idx = Int(Double(i) / Double(wavePoints - 1) * Double(take - 1))
                wave[i] = mono[idx]
            }
        }

        let normRMS = min(1, max(0, (20 * log10(rms + 1e-7) + 52) / 46))
        let normPeak = min(1, peak * 1.4)

        lock.lock()
        outBands = bands
        outWave = wave
        outRMS = normRMS
        outPeak = normPeak
        lock.unlock()
    }

    func snapshot() -> (bands: [Float], wave: [Float], rms: Float, peak: Float) {
        lock.lock()
        defer { lock.unlock() }
        return (outBands, outWave, outRMS, outPeak)
    }
}

@Observable final class PlayerModel {
    var playlist: [Track] = []
    var selectedTrackID: Track.ID?
    var isPlaying = false
    var currentTime: TimeInterval = 0
    var volume: Double = 0.82
    var balance: Double = 0
    var shuffle = false
    var repeatMode = RepeatMode.none
    var gain: Double = 0.58
    var preamp: Double = 0.64
    static let bandCount = 32

    /// Real, smoothed per-band frequency magnitudes (0...1) from the live FFT.
    var spectrumLevels: [Double] = Array(repeating: 0, count: PlayerModel.bandCount)
    /// Overall loudness (RMS, 0...1) — drives amplitude reactivity.
    var amplitude: Double = 0
    /// Transient peak (0...1) — useful for beat-driven effects.
    var peak: Double = 0
    /// Downsampled time-domain waveform (-1...1) for oscilloscope visualizers.
    var waveform: [Double] = Array(repeating: 0, count: 168)
    var lastFMStatus: LastFMStatus = .disabled

    // Engine graph: player -> EQ (gain >100% headroom) -> mainMixer -> output.
    private let engine = AVAudioEngine()
    private let playerNode = AVAudioPlayerNode()
    private let eqNode = AVAudioUnitEQ(numberOfBands: 0)
    private var analyzer = AudioAnalyzer(bandCount: PlayerModel.bandCount)
    private var nodesAttached = false
    private var tapInstalled = false

    private var audioFile: AVAudioFile?
    private var fileSampleRate: Double = 44_100
    private var totalFrames: AVAudioFramePosition = 0
    private var seekOffsetSeconds: Double = 0

    private var displayTimer: Timer?
    private var metadataRefreshTask: Task<Void, Never>?
    private var securityScopedURLs: [URL] = []
    private var lastFMConfiguration = LastFMConfiguration.empty
    private var lastFMClient: LastFMClient?
    private var lastFMTask: Task<Void, Never>?
    private var playbackStartDate: Date?
    private var nowPlayingTrackID: Track.ID?
    private var scrobbledTrackID: Track.ID?

    // Convenience band energies, expressed as fractions of the spectrum so they
    // stay correct regardless of band count.
    var bassLevel: Double { bandEnergy(from: 0.00, to: 0.16) }
    var lowMidLevel: Double { bandEnergy(from: 0.16, to: 0.34) }
    var midLevel: Double { bandEnergy(from: 0.34, to: 0.58) }
    var trebleLevel: Double { bandEnergy(from: 0.58, to: 1.00) }

    private func bandEnergy(from: Double, to: Double) -> Double {
        let count = spectrumLevels.count
        guard count > 0 else { return 0 }
        let lo = Int(Double(count) * from)
        let hi = max(lo + 1, Int(Double(count) * to))
        let clampedHi = min(hi, count)
        guard lo < clampedHi else { return 0 }
        let slice = spectrumLevels[lo..<clampedHi]
        return slice.reduce(0, +) / Double(slice.count)
    }

    var selectedTrack: Track? {
        guard let selectedTrackID else { return playlist.first }
        return playlist.first { $0.id == selectedTrackID }
    }

    var currentDuration: TimeInterval {
        if totalFrames > 0, fileSampleRate > 0 {
            return Double(totalFrames) / fileSampleRate
        }
        return selectedTrack?.duration ?? 0
    }

    var elapsedDisplay: String {
        Self.formatTime(currentTime)
    }

    var durationDisplay: String {
        Self.formatTime(currentDuration)
    }

    var progress: Double {
        guard currentDuration > 0 else { return 0 }
        return min(max(currentTime / currentDuration, 0), 1)
    }

    func updateLastFMConfiguration(_ configuration: LastFMConfiguration) {
        lastFMConfiguration = configuration
        lastFMClient = configuration.isReady ? LastFMClient(configuration: configuration) : nil
        lastFMStatus = configuration.isEnabled ? (configuration.isReady ? .ready : .needsSetup) : .disabled
    }

    func addFiles(_ urls: [URL], playlistMediaFolder: URL? = nil) async {
        var importedTracks: [Track] = []

        if let playlistMediaFolder, playlistMediaFolder.startAccessingSecurityScopedResource() {
            securityScopedURLs.append(playlistMediaFolder)
        }

        for url in urls {
            if url.startAccessingSecurityScopedResource() {
                securityScopedURLs.append(url)
            }

            if Self.isPlaylistURL(url) {
                let playlistFolderURL = url.deletingLastPathComponent()
                if playlistFolderURL.startAccessingSecurityScopedResource() {
                    securityScopedURLs.append(playlistFolderURL)
                }

                let playlistEntries = Self.loadPlaylistEntries(from: url, mediaFolder: playlistMediaFolder)
                for entry in playlistEntries {
                    if entry.url.startAccessingSecurityScopedResource() {
                        securityScopedURLs.append(entry.url)
                    }
                    importedTracks.append(await Self.makeTrack(from: entry.url, fallbackTitle: entry.title))
                }
            } else {
                importedTracks.append(await Self.makeTrack(from: url))
            }
        }

        guard !importedTracks.isEmpty else { return }
        playlist.append(contentsOf: importedTracks)

        if selectedTrackID == nil, let firstTrack = playlist.first {
            load(firstTrack, autoPlay: false)
        }
    }

    @discardableResult
    func load(_ track: Track, autoPlay: Bool) -> Bool {
        selectedTrackID = track.id
        currentTime = 0
        playbackStartDate = nil
        nowPlayingTrackID = nil
        scrobbledTrackID = nil

        do {
            let file = try AVAudioFile(forReading: track.url)
            audioFile = file
            fileSampleRate = file.processingFormat.sampleRate
            totalFrames = file.length
            configureEngine(for: file.processingFormat)
            scheduleSegment(startFrame: 0)
            refreshMetadataIfNeeded(for: track)
        } catch {
            audioFile = nil
            totalFrames = 0
            isPlaying = false
            return false
        }

        startDisplayTimer()

        if autoPlay {
            return startPlayback()
        } else {
            // Leave the node stopped but armed with the scheduled segment;
            // a later play() starts it cleanly. No pause() needed here.
            isPlaying = false
            return true
        }
    }

    private func configureEngine(for format: AVAudioFormat) {
        if !nodesAttached {
            engine.attach(playerNode)
            engine.attach(eqNode)
            nodesAttached = true
        }

        playerNode.stop()   // safe to reconnect once the source node is idle
        engine.connect(playerNode, to: eqNode, format: format)
        engine.connect(eqNode, to: engine.mainMixerNode, format: format)
        engine.prepare()

        if !engine.isRunning {
            try? engine.start()
        }

        if !tapInstalled {
            let tapFormat = engine.mainMixerNode.outputFormat(forBus: 0)
            analyzer = AudioAnalyzer(bandCount: Self.bandCount, sampleRate: tapFormat.sampleRate)
            engine.mainMixerNode.installTap(onBus: 0, bufferSize: 1024, format: tapFormat) { [weak self] buffer, _ in
                self?.analyzer.process(buffer)
            }
            tapInstalled = true
        }

        applyVolumeAndBalance()
    }

    private func scheduleSegment(startFrame: AVAudioFramePosition) {
        guard let file = audioFile, totalFrames > startFrame else { return }
        playerNode.stop()
        seekOffsetSeconds = Double(startFrame) / fileSampleRate
        let frameCount = AVAudioFrameCount(totalFrames - startFrame)
        playerNode.scheduleSegment(file, startingFrame: startFrame, frameCount: frameCount, at: nil, completionHandler: nil)
    }

    @discardableResult
    private func startPlayback() -> Bool {
        guard audioFile != nil else { return false }
        if !engine.isRunning {
            do { try engine.start() } catch { isPlaying = false; return false }
        }
        playerNode.play()
        isPlaying = true
        beginLastFMPlaybackIfNeeded()
        startDisplayTimer()
        return true
    }

    func togglePlay() {
        guard selectedTrack != nil else { return }
        isPlaying ? pause() : play()
    }

    func stop() {
        playerNode.stop()
        currentTime = 0
        isPlaying = false
        if audioFile != nil {
            scheduleSegment(startFrame: 0)   // re-arm so Play works again
        }
    }

    func previous() {
        guard !playlist.isEmpty else { return }
        move(offset: -1)
    }

    func next() {
        guard !playlist.isEmpty else { return }

        if shuffle, playlist.count > 1 {
            let candidates = playlist.filter { $0.id != selectedTrackID }
            for track in candidates.shuffled() {
                if load(track, autoPlay: isPlaying) {
                    return
                }
            }
        }

        move(offset: 1)
    }

    func seek(to progress: Double) {
        guard totalFrames > 0 else { return }
        let clampedProgress = min(max(progress, 0), 1)
        let startFrame = AVAudioFramePosition(Double(totalFrames) * clampedProgress)
        let wasPlaying = isPlaying
        scheduleSegment(startFrame: startFrame)
        currentTime = seekOffsetSeconds
        if wasPlaying {
            playerNode.play()
        }
        // If we weren't playing, the node stays stopped + armed at the new
        // position, ready for the next play().
    }

    func updateVolume() {
        applyVolumeAndBalance()
    }

    func updateBalance() {
        applyVolumeAndBalance()
    }

    /// Volume 0...1 maps to the player node directly; 1...2 is achieved as true
    /// amplification via the EQ node's global gain (0 dB at 100%, +6 dB at 200%).
    private func applyVolumeAndBalance() {
        playerNode.volume = Float(min(volume, 1.0))
        eqNode.globalGain = volume > 1 ? Float(min(24, 20 * log10(volume))) : 0
        playerNode.pan = Float(min(max(balance, -1), 1))
    }

    func toggleRepeatMode() {
        repeatMode = repeatMode.next
    }

    func remove(_ track: Track) {
        playlist.removeAll { $0.id == track.id }
        if selectedTrackID == track.id {
            stop()
            selectedTrackID = nil
            if let replacement = playlist.first {
                load(replacement, autoPlay: false)
            }
        }
    }

    func clearPlaylist() {
        stop()
        playlist.removeAll()
        selectedTrackID = nil
        audioFile = nil
        totalFrames = 0
        stopDisplayTimer()
        spectrumLevels = Array(repeating: 0, count: Self.bandCount)
        amplitude = 0
        peak = 0
        waveform = Array(repeating: 0, count: waveform.count)
    }

    func sortPlaylistByTitle() {
        let activeID = selectedTrackID
        playlist.sort { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
        selectedTrackID = activeID
    }

    func removeMissingFiles() {
        let missingIDs = Set(playlist.filter { !FileManager.default.fileExists(atPath: $0.url.path) }.map(\.id))
        guard !missingIDs.isEmpty else { return }
        playlist.removeAll { missingIDs.contains($0.id) }

        if let selectedTrackID, missingIDs.contains(selectedTrackID) {
            stop()
            self.selectedTrackID = nil
            if let replacement = playlist.first {
                load(replacement, autoPlay: false)
            }
        }
    }

    private var selectedIndex: Int? {
        guard let selectedTrackID else { return nil }
        return playlist.firstIndex { $0.id == selectedTrackID }
    }

    private func move(offset: Int) {
        let wasPlaying = isPlaying
        let startIndex = selectedIndex ?? 0

        for step in 1...playlist.count {
            let rawIndex = startIndex + offset * step
            let wrappedIndex = (rawIndex % playlist.count + playlist.count) % playlist.count
            if load(playlist[wrappedIndex], autoPlay: wasPlaying) {
                return
            }
        }
    }

    private func play() {
        if audioFile == nil, let selectedTrack, !load(selectedTrack, autoPlay: false) {
            return
        }
        _ = startPlayback()
    }

    private func pause() {
        playerNode.pause()
        isPlaying = false
    }

    private func beginLastFMPlaybackIfNeeded() {
        guard let track = selectedTrack else { return }
        if playbackStartDate == nil || nowPlayingTrackID != track.id {
            playbackStartDate = Date()
        }
        sendNowPlayingIfNeeded(for: track)
    }

    private func sendNowPlayingIfNeeded(for track: Track) {
        guard nowPlayingTrackID != track.id else { return }
        guard track.artist?.isEmpty == false else {
            lastFMStatus = lastFMConfiguration.isEnabled ? .needsSetup : .disabled
            return
        }
        guard let lastFMClient else {
            lastFMStatus = lastFMConfiguration.isEnabled ? .needsSetup : .disabled
            return
        }

        nowPlayingTrackID = track.id
        lastFMTask?.cancel()
        lastFMTask = Task { [weak self] in
            do {
                try await lastFMClient.updateNowPlaying(track: track)
                await MainActor.run {
                    guard let self, self.selectedTrackID == track.id else { return }
                    self.lastFMStatus = .nowPlaying
                }
            } catch {
                await MainActor.run {
                    guard let self, self.selectedTrackID == track.id else { return }
                    self.lastFMStatus = .failed(error.localizedDescription)
                }
            }
        }
    }

    private func scrobbleIfNeeded() {
        guard let track = selectedTrack,
              scrobbledTrackID != track.id,
              track.duration > 30,
              track.artist?.isEmpty == false,
              let playbackStartDate,
              let lastFMClient else {
            return
        }

        let threshold = min(track.duration / 2, 240)
        guard currentTime >= threshold else { return }

        scrobbledTrackID = track.id
        lastFMTask?.cancel()
        lastFMTask = Task { [weak self] in
            do {
                try await lastFMClient.scrobble(track: track, startedAt: playbackStartDate)
                await MainActor.run {
                    guard let self, self.selectedTrackID == track.id else { return }
                    self.lastFMStatus = .scrobbled
                }
            } catch {
                await MainActor.run {
                    guard let self, self.selectedTrackID == track.id else { return }
                    self.scrobbledTrackID = nil
                    self.lastFMStatus = .failed(error.localizedDescription)
                }
            }
        }
    }

    private func nodeElapsedSeconds() -> Double? {
        guard let nodeTime = playerNode.lastRenderTime,
              let playerTime = playerNode.playerTime(forNodeTime: nodeTime),
              playerTime.sampleRate > 0 else {
            return nil
        }
        return Double(playerTime.sampleTime) / playerTime.sampleRate
    }

    /// Single ~60 Hz tick: advances the clock from the render timeline, pulls a
    /// fresh FFT snapshot, smooths it into the published reactive values, and
    /// handles end-of-track. Smoothing uses a fast attack / slow decay envelope
    /// so peaks pop while tails fall gracefully.
    private func startDisplayTimer() {
        guard displayTimer == nil else { return }
        displayTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    private func tick() {
        if isPlaying {
            if let elapsed = nodeElapsedSeconds() {
                currentTime = min(seekOffsetSeconds + elapsed, currentDuration)
            }

            let snap = analyzer.snapshot()
            if snap.bands.count == spectrumLevels.count {
                for i in spectrumLevels.indices {
                    let target = Double(snap.bands[i])
                    let prev = spectrumLevels[i]
                    let coeff = target > prev ? 0.55 : 0.16
                    spectrumLevels[i] = prev + (target - prev) * coeff
                }
            }

            let targetAmp = Double(snap.rms)
            amplitude += (targetAmp - amplitude) * (targetAmp > amplitude ? 0.45 : 0.12)
            peak = max(Double(snap.peak), peak * 0.86)

            if snap.wave.count == waveform.count {
                for i in waveform.indices {
                    waveform[i] = waveform[i] * 0.4 + Double(snap.wave[i]) * 0.6
                }
            }

            scrobbleIfNeeded()

            if currentDuration > 0, currentTime >= currentDuration - 0.08 {
                handlePlaybackEnded()
            }
        } else {
            // Decay everything toward rest when paused or stopped.
            for i in spectrumLevels.indices { spectrumLevels[i] *= 0.86 }
            amplitude *= 0.86
            peak *= 0.86
            for i in waveform.indices { waveform[i] *= 0.8 }
        }
    }

    private func handlePlaybackEnded() {
        switch repeatMode {
        case .one:
            scheduleSegment(startFrame: 0)
            currentTime = 0
            _ = startPlayback()
        case .all:
            next()
        case .none:
            if selectedIndex == playlist.indices.last {
                isPlaying = false
                playerNode.stop()
                if audioFile != nil { scheduleSegment(startFrame: 0) }
                currentTime = currentDuration
            } else {
                next()
            }
        }
    }

    private func stopDisplayTimer() {
        displayTimer?.invalidate()
        displayTimer = nil
    }

    private func refreshMetadataIfNeeded(for track: Track) {
        guard track.duration == 0 || track.artist == nil || track.album == nil || track.bitDepth == nil else { return }

        metadataRefreshTask?.cancel()
        metadataRefreshTask = Task { [weak self] in
            let refreshedTrack = await Self.makeTrack(from: track.url, id: track.id, fallbackTitle: track.title)
            guard !Task.isCancelled else { return }

            await MainActor.run {
                guard let self, let index = self.playlist.firstIndex(where: { $0.id == track.id }) else { return }
                self.playlist[index] = refreshedTrack
            }
        }
    }

    private static func makeTrack(from url: URL, id: UUID = UUID(), fallbackTitle: String? = nil) async -> Track {
        let asset = AVURLAsset(url: url)
        let duration = await loadDuration(from: asset)
        let metadata = await loadMetadata(from: asset, fallbackURL: url)
        let technicalInfo = await loadTechnicalInfo(from: asset)

        return Track(
            id: id,
            url: url,
            title: metadata.title ?? fallbackTitle ?? url.deletingPathExtension().lastPathComponent,
            format: formatName(for: url),
            duration: duration,
            artist: metadata.artist,
            album: metadata.album,
            artworkData: metadata.artworkData,
            bitDepth: technicalInfo.bitDepth,
            sampleRate: technicalInfo.sampleRate,
            channelCount: technicalInfo.channelCount,
            bitRate: technicalInfo.bitRate,
            codec: technicalInfo.codec
        )
    }

    private static func makeLightweightTrack(from url: URL, title: String? = nil) -> Track {
        Track(
            url: url,
            title: title ?? url.deletingPathExtension().lastPathComponent,
            format: formatName(for: url),
            duration: 0,
            artist: nil,
            album: nil,
            artworkData: nil,
            bitDepth: nil,
            sampleRate: nil,
            channelCount: nil,
            bitRate: nil,
            codec: nil
        )
    }

    private static func formatName(for url: URL) -> String {
        url.pathExtension.uppercased().isEmpty ? "AUDIO" : url.pathExtension.uppercased()
    }

    private static func isPlaylistURL(_ url: URL) -> Bool {
        let fileExtension = url.pathExtension.lowercased()
        return fileExtension == "m3u" || fileExtension == "m3u8"
    }

    private static func playlistText(from playlistURL: URL) -> String? {
        if let utf8Text = try? String(contentsOf: playlistURL, encoding: .utf8) {
            return utf8Text
        }

        return try? String(contentsOf: playlistURL, encoding: .isoLatin1)
    }

    private static func loadPlaylistEntries(from playlistURL: URL, mediaFolder: URL? = nil) -> [PlaylistEntry] {
        guard let text = playlistText(from: playlistURL) else {
            return []
        }

        var entries: [PlaylistEntry] = []
        var pendingTitle: String?
        var seenPaths = Set<String>()
        let baseURL = playlistURL.deletingLastPathComponent()

        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !line.isEmpty else { continue }

            if line.hasPrefix("#EXTINF") {
                pendingTitle = extinfTitle(from: line)
                continue
            }

            guard !line.hasPrefix("#") else { continue }
            guard let candidate = playlistEntryURL(from: line, relativeTo: baseURL, mediaFolder: mediaFolder) else {
                pendingTitle = nil
                continue
            }
            guard isSupportedPlaylistAudioURL(candidate), FileManager.default.fileExists(atPath: candidate.path) else {
                pendingTitle = nil
                continue
            }

            let path = candidate.standardizedFileURL.path
            guard seenPaths.insert(path).inserted else {
                pendingTitle = nil
                continue
            }

            entries.append(PlaylistEntry(url: candidate, title: pendingTitle))
            pendingTitle = nil
        }

        return entries
    }

    private static func extinfTitle(from line: String) -> String? {
        guard let commaIndex = line.firstIndex(of: ",") else { return nil }
        let title = line[line.index(after: commaIndex)...].trimmingCharacters(in: .whitespacesAndNewlines)
        return title.isEmpty ? nil : title
    }

    private static func playlistEntryURL(from line: String, relativeTo baseURL: URL, mediaFolder: URL? = nil) -> URL? {
        let normalizedLine = line.replacingOccurrences(of: "\\", with: "/").removingPercentEncoding ?? line.replacingOccurrences(of: "\\", with: "/")
        let lowercasedLine = normalizedLine.lowercased()

        if lowercasedLine.hasPrefix("http://") || lowercasedLine.hasPrefix("https://") {
            return nil
        }

        if lowercasedLine.hasPrefix("file://") {
            if let url = URL(string: normalizedLine), url.isFileURL {
                return url.standardizedFileURL
            }

            let allowedCharacters = CharacterSet.urlFragmentAllowed.union(.urlHostAllowed).union(.urlPathAllowed)
            if let encodedLine = normalizedLine.addingPercentEncoding(withAllowedCharacters: allowedCharacters),
               let url = URL(string: encodedLine),
               url.isFileURL {
                return url.standardizedFileURL
            }

            let strippedPath = normalizedLine.replacingOccurrences(of: "file://localhost", with: "").replacingOccurrences(of: "file://", with: "")
            return URL(fileURLWithPath: NSString(string: strippedPath).expandingTildeInPath).standardizedFileURL
        }

        if normalizedLine.hasPrefix("/") || normalizedLine.hasPrefix("~") {
            return URL(fileURLWithPath: NSString(string: normalizedLine).expandingTildeInPath).standardizedFileURL
        }

        if let mediaFolder {
            let mediaCandidate = mediaFolder.appendingPathComponent(normalizedLine).standardizedFileURL
            if FileManager.default.fileExists(atPath: mediaCandidate.path) {
                return mediaCandidate
            }
        }

        return baseURL.appendingPathComponent(normalizedLine).standardizedFileURL
    }

    private static func isSupportedPlaylistAudioURL(_ url: URL) -> Bool {
        let supportedExtensions: Set<String> = [
            "aac", "aif", "aiff", "alac", "ape", "caf", "flac", "m4a", "m4b", "mka", "mod", "mp3", "mp4", "oga", "ogg", "opus", "s3m", "tta", "wav", "wma", "wv", "xm"
        ]
        return supportedExtensions.contains(url.pathExtension.lowercased())
    }

    private static func loadDuration(from asset: AVURLAsset) async -> TimeInterval {
        do {
            let duration = try await asset.load(.duration)
            let seconds = duration.seconds
            return seconds.isFinite ? seconds : 0
        } catch {
            return 0
        }
    }

    private static func loadMetadata(from asset: AVURLAsset, fallbackURL: URL) async -> TrackMetadata {
        do {
            let commonItems = try await asset.load(.commonMetadata)
            let allItems = (try? await asset.load(.metadata)) ?? []
            let items = commonItems + allItems
            async let title = metadataString(primary: .commonIdentifierTitle, fallbackKeys: ["title", "tit2", "©nam"], in: items)
            async let artist = metadataString(primary: .commonIdentifierArtist, fallbackKeys: ["artist", "tpe1", "©art"], in: items)
            async let album = metadataString(primary: .commonIdentifierAlbumName, fallbackKeys: ["album", "talb", "©alb"], in: items)
            async let artworkData = metadataArtworkData(in: items)

            return TrackMetadata(
                title: await title,
                artist: await artist,
                album: await album,
                artworkData: await artworkData
            )
        } catch {
            return TrackMetadata(title: fallbackURL.deletingPathExtension().lastPathComponent, artist: nil, album: nil, artworkData: nil)
        }
    }

    private static func metadataString(primary identifier: AVMetadataIdentifier, fallbackKeys: [String], in items: [AVMetadataItem]) async -> String? {
        if let value = await metadataString(for: identifier, in: items) {
            return value
        }

        return await metadataString(matching: fallbackKeys, in: items)
    }

    private static func metadataString(for identifier: AVMetadataIdentifier, in items: [AVMetadataItem]) async -> String? {
        guard let item = AVMetadataItem.metadataItems(from: items, filteredByIdentifier: identifier).first else {
            return nil
        }

        return await metadataString(from: item)
    }

    private static func metadataString(matching keys: [String], in items: [AVMetadataItem]) async -> String? {
        for item in items where metadataKeyMatches(item, keys: keys) {
            if let value = await metadataString(from: item) {
                return value
            }
        }

        return nil
    }

    private static func metadataString(from item: AVMetadataItem) async -> String? {
        do {
            let value = try await item.load(.stringValue)
            return value?.isEmpty == false ? value : nil
        } catch {
            return nil
        }
    }

    private static func metadataKeyMatches(_ item: AVMetadataItem, keys: [String]) -> Bool {
        let normalizedKeys = Set(keys.map { $0.lowercased() })
        let keyString = item.key.map { String(describing: $0).lowercased() } ?? ""
        let identifier = item.identifier?.rawValue.lowercased() ?? ""
        return normalizedKeys.contains { key in keyString.contains(key) || identifier.contains(key) }
    }

    private static func metadataArtworkData(in items: [AVMetadataItem]) async -> Data? {
        let artworkItems = AVMetadataItem.metadataItems(from: items, filteredByIdentifier: .commonIdentifierArtwork)
        guard let item = artworkItems.first else { return nil }

        if let data = try? await item.load(.dataValue) {
            return data
        }

        return nil
    }

    private static func loadTechnicalInfo(from asset: AVURLAsset) async -> TrackTechnicalInfo {
        do {
            let tracks = try await asset.load(.tracks)
            guard let audioTrack = tracks.first(where: { $0.mediaType == .audio }) else {
                return TrackTechnicalInfo()
            }

            async let formatDescriptions = audioTrack.load(.formatDescriptions)
            async let estimatedDataRate = audioTrack.load(.estimatedDataRate)

            let descriptions = (try? await formatDescriptions) ?? []
            let dataRate = (try? await estimatedDataRate).map(Double.init)
            var info = TrackTechnicalInfo(bitRate: dataRate)

            if let description = descriptions.first,
               let streamDescription = CMAudioFormatDescriptionGetStreamBasicDescription(description) {
                let audioDescription = streamDescription.pointee
                info.bitDepth = audioDescription.mBitsPerChannel > 0 ? Int(audioDescription.mBitsPerChannel) : nil
                info.sampleRate = audioDescription.mSampleRate > 0 ? audioDescription.mSampleRate : nil
                info.channelCount = audioDescription.mChannelsPerFrame > 0 ? Int(audioDescription.mChannelsPerFrame) : nil
                info.codec = fourCharacterCode(audioDescription.mFormatID)
            }

            return info
        } catch {
            return TrackTechnicalInfo()
        }
    }

    private static func fourCharacterCode(_ value: AudioFormatID) -> String? {
        let scalarValues = [
            UInt8((value >> 24) & 0xff),
            UInt8((value >> 16) & 0xff),
            UInt8((value >> 8) & 0xff),
            UInt8(value & 0xff)
        ]

        guard scalarValues.allSatisfy({ $0 >= 32 && $0 <= 126 }) else {
            return nil
        }

        return String(bytes: scalarValues, encoding: .macOSRoman)?.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func formatTime(_ interval: TimeInterval) -> String {
        guard interval.isFinite, interval > 0 else { return "00:00" }
        let totalSeconds = Int(interval.rounded())
        let minutes = totalSeconds / 60
        let seconds = totalSeconds % 60
        return String(format: "%02d:%02d", minutes, seconds)
    }
}

struct TrackMetadata {
    var title: String?
    var artist: String?
    var album: String?
    var artworkData: Data?
}

struct TrackTechnicalInfo {
    var bitDepth: Int?
    var sampleRate: Double?
    var channelCount: Int?
    var bitRate: Double?
    var codec: String?
}

struct PlaylistEntry {
    let url: URL
    let title: String?
}

enum RepeatMode: String, CaseIterable {
    case none = "REP"
    case one = "ONE"
    case all = "ALL"

    var next: RepeatMode {
        switch self {
        case .none: .one
        case .one: .all
        case .all: .none
        }
    }
}

enum VisualizationMode: String, CaseIterable, Identifiable {
    case spectrum = "Spectrum"
    case mirror = "Mirror Bars"
    case scope = "Oscilloscope"
    case fractal = "Fractal"
    case orbit = "Orbit"
    case rings = "Rings"
    case tunnel = "Tunnel"
    case radial = "Radial Burst"
    case lissajous = "Lissajous"
    case bloom = "Bloom"
    case particles = "Particles"
    case matrix = "Spectro Fall"

    var id: String { rawValue }
}

enum AmpintoshSkin: String, CaseIterable, Identifiable {
    case ampintosh = "Ampintosh"
    case fruitStudio = "Fruit Studio 12"
    case liveSession = "Live Session"
    case orangeBlack = "Orange Black"
    case digitalTamer = "Digital Tamer"
    case octoHub = "OctoHub"
    case spaceCowboy = "Space Cowboy"
    case musicGlass = "Music Glass"

    var id: String { rawValue }

    var palette: SkinPalette {
        switch self {
        case .ampintosh:
            SkinPalette(
                background: [Color(hex: 0x0c1118), Color(hex: 0x1f2a36), Color(hex: 0x101d18), Color(hex: 0x14151d)],
                primary: Color(hex: 0xa8ff5f),
                secondary: Color(hex: 0xffcf5f),
                tertiary: Color(hex: 0x73ffe1),
                text: Color(hex: 0xeaf6ff),
                mutedText: Color(hex: 0x9fb8d6),
                panel: Color(hex: 0x101923),
                display: Color(hex: 0x041109)
            )
        case .fruitStudio:
            SkinPalette(
                background: [Color(hex: 0x151a20), Color(hex: 0x27303a), Color(hex: 0x1f2d24), Color(hex: 0x10151a)],
                primary: Color(hex: 0x9dff63),
                secondary: Color(hex: 0xff9b45),
                tertiary: Color(hex: 0x5fc7ff),
                text: Color(hex: 0xf3f7ec),
                mutedText: Color(hex: 0xb8c3b4),
                panel: Color(hex: 0x202832),
                display: Color(hex: 0x0f1c15)
            )
        case .liveSession:
            SkinPalette(
                background: [Color(hex: 0xe8e8e3), Color(hex: 0xcfcfc8), Color(hex: 0x222222), Color(hex: 0x111111)],
                primary: Color(hex: 0x111111),
                secondary: Color(hex: 0xff6a00),
                tertiary: Color(hex: 0x007aff),
                text: Color(hex: 0xf8f8f2),
                mutedText: Color(hex: 0xc8c8c2),
                panel: Color(hex: 0x2a2a2a),
                display: Color(hex: 0x151515)
            )
        case .orangeBlack:
            SkinPalette(
                background: [Color(hex: 0x050505), Color(hex: 0x17110b), Color(hex: 0x251505), Color(hex: 0x090909)],
                primary: Color(hex: 0xff9f1c),
                secondary: Color(hex: 0xffffff),
                tertiary: Color(hex: 0xff5a00),
                text: Color(hex: 0xffffff),
                mutedText: Color(hex: 0xb7b7b7),
                panel: Color(hex: 0x151515),
                display: Color(hex: 0x090909)
            )
        case .digitalTamer:
            SkinPalette(
                background: [Color(hex: 0x061922), Color(hex: 0x0b3144), Color(hex: 0x321132), Color(hex: 0x100b22)],
                primary: Color(hex: 0x00e5ff),
                secondary: Color(hex: 0xff345f),
                tertiary: Color(hex: 0xffd447),
                text: Color(hex: 0xe8fbff),
                mutedText: Color(hex: 0x9acdd7),
                panel: Color(hex: 0x102436),
                display: Color(hex: 0x06131d)
            )
        case .octoHub:
            SkinPalette(
                background: [Color(hex: 0x0d1117), Color(hex: 0x161b22), Color(hex: 0x1f2937), Color(hex: 0x010409)],
                primary: Color(hex: 0x7ee787),
                secondary: Color(hex: 0x58a6ff),
                tertiary: Color(hex: 0xd2a8ff),
                text: Color(hex: 0xf0f6fc),
                mutedText: Color(hex: 0x8b949e),
                panel: Color(hex: 0x161b22),
                display: Color(hex: 0x0d1117)
            )
        case .spaceCowboy:
            SkinPalette(
                background: [Color(hex: 0x11141d), Color(hex: 0x22283d), Color(hex: 0x3d1f18), Color(hex: 0x08090f)],
                primary: Color(hex: 0xffd166),
                secondary: Color(hex: 0xe85d75),
                tertiary: Color(hex: 0x66d9ef),
                text: Color(hex: 0xfff1cf),
                mutedText: Color(hex: 0xb7a987),
                panel: Color(hex: 0x1f2536),
                display: Color(hex: 0x141825)
            )
        case .musicGlass:
            SkinPalette(
                background: [Color(hex: 0x2b0820), Color(hex: 0x5d1138), Color(hex: 0x121426), Color(hex: 0x080812)],
                primary: Color(hex: 0xff2d55),
                secondary: Color(hex: 0xff9f0a),
                tertiary: Color(hex: 0x5ac8fa),
                text: Color(hex: 0xffffff),
                mutedText: Color(hex: 0xd9c9d4),
                panel: Color(hex: 0x251525),
                display: Color(hex: 0x120812)
            )
        }
    }
}

struct SkinPalette {
    let background: [Color]
    let primary: Color
    let secondary: Color
    let tertiary: Color
    let text: Color
    let mutedText: Color
    let panel: Color
    let display: Color
}

private struct AmpintoshSkinKey: EnvironmentKey {
    static let defaultValue = AmpintoshSkin.ampintosh.palette
}

extension EnvironmentValues {
    var ampintoshSkin: SkinPalette {
        get { self[AmpintoshSkinKey.self] }
        set { self[AmpintoshSkinKey.self] = newValue }
    }
}

struct ContentView: View {
    @State private var player = PlayerModel()
    @State private var isImporting = false
    @AppStorage("Ampintosh.visualizationMode") private var visualizationMode = VisualizationMode.fractal
    @AppStorage("Ampintosh.selectedSkin") private var selectedSkin = AmpintoshSkin.ampintosh
    @State private var outputDevice = AudioOutputDevice.current()
    @State private var rightPanelWidth: CGFloat = 430
    @State private var visualizerHeight: CGFloat = 150
    @State private var lastFMConfiguration = LastFMSettingsStore.load()
    @State private var isShowingLastFMSettings = false

    private let supportedTypes = [
        UTType.audio,
        UTType.mp3,
        UTType.wav,
        UTType.aiff,
        UTType.mpeg4Audio,
        UTType(filenameExtension: "flac"),
        UTType(filenameExtension: "ogg"),
        UTType(filenameExtension: "opus"),
        UTType(filenameExtension: "wma"),
        UTType(filenameExtension: "m3u"),
        UTType(filenameExtension: "m3u8"),
        UTType(filenameExtension: "ape"),
        UTType(filenameExtension: "wv"),
        UTType(filenameExtension: "tta"),
        UTType(filenameExtension: "mka"),
        UTType(filenameExtension: "mod"),
        UTType(filenameExtension: "xm"),
        UTType(filenameExtension: "s3m")
    ].compactMap { $0 }

    var body: some View {
        ZStack {
            LiquidRetroBackground(palette: selectedSkin.palette)

            GlassEffectContainer(spacing: 16) {
                VStack(spacing: 10) {
                    PlayerHeader(
                        selectedSkin: $selectedSkin,
                        outputDevice: outputDevice,
                        lastFMStatus: player.lastFMStatus,
                        onConfigureLastFM: { isShowingLastFMSettings = true }
                    )

                    HStack(alignment: .top, spacing: 10) {
                        VStack(spacing: 10) {
                            MainDeck(player: player, isImporting: $isImporting)
                            EqualizerPanel(player: player)
                        }
                        .frame(width: 390)

                        VerticalResizeHandle(width: $rightPanelWidth)

                        PlaylistPanel(
                            player: player,
                            isImporting: $isImporting,
                            visualizationMode: $visualizationMode,
                            visualizerHeight: $visualizerHeight
                        )
                        .frame(width: rightPanelWidth)
                    }
                    .padding(.horizontal, 14)
                    .padding(.bottom, 14)
                }
            }
        }
        .environment(\.ampintoshSkin, selectedSkin.palette)
        .contextMenu {
            Button {
                isImporting = true
            } label: {
                Label("Load Audio Files...", systemImage: "folder")
            }

            if player.selectedTrack != nil {
                Button {
                    player.togglePlay()
                } label: {
                    Label(player.isPlaying ? "Pause" : "Play", systemImage: player.isPlaying ? "pause.fill" : "play.fill")
                }

                Button {
                    player.stop()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
            }
        }
        .fileImporter(isPresented: $isImporting, allowedContentTypes: supportedTypes, allowsMultipleSelection: true) { result in
            if case .success(let urls) = result {
                let playlistMediaFolder = Self.choosePlaylistMediaFolderIfNeeded(for: urls)
                Task {
                    await player.addFiles(urls, playlistMediaFolder: playlistMediaFolder)
                }
            }
        }
        .focusedSceneValue(\.openAudioFiles) {
            isImporting = true
        }
        .focusedSceneValue(\.clearPlaylist) {
            player.clearPlaylist()
        }
        .sheet(isPresented: $isShowingLastFMSettings) {
            LastFMSettingsView(configuration: $lastFMConfiguration)
                .frame(width: 420)
        }
        .onChange(of: lastFMConfiguration) { _, newValue in
            LastFMSettingsStore.save(newValue)
            player.updateLastFMConfiguration(newValue)
        }
        .task {
            player.updateLastFMConfiguration(lastFMConfiguration)
            outputDevice = AudioOutputDevice.current()
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(2))
                outputDevice = AudioOutputDevice.current()
            }
        }
    }

    private static func choosePlaylistMediaFolderIfNeeded(for urls: [URL]) -> URL? {
        guard urls.contains(where: { ["m3u", "m3u8"].contains($0.pathExtension.lowercased()) }) else {
            return nil
        }

        #if os(macOS)
        let panel = NSOpenPanel()
        panel.title = "Choose the folder that contains the playlist's audio files"
        panel.message = "Ampintosh needs permission to read the audio files referenced by the playlist. Choose the music folder or a common parent folder."
        panel.prompt = "Use Folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = false

        return panel.runModal() == .OK ? panel.url : nil
        #else
        return nil
        #endif
    }
}

struct PlayerHeader: View {
    @Binding var selectedSkin: AmpintoshSkin
    let outputDevice: AudioOutputDevice
    let lastFMStatus: LastFMStatus
    let onConfigureLastFM: () -> Void
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        HStack(spacing: 8) {
            Text("AMPINTOSH")
                .font(.system(size: 13, weight: .black, design: .monospaced))
                .foregroundStyle(.white)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .foregroundStyle(
                    LinearGradient(
                        colors: [Color.white, skin.primary],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
                .glassEffect(.regular.tint(skin.primary.opacity(0.18)).interactive(), in: .rect(cornerRadius: 8))

            Text("BROAD-FORMAT AUDIO PLAYER")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(skin.mutedText.opacity(0.88))

            Spacer()

            OutputDeviceIndicator(device: outputDevice)

            Button {
                onConfigureLastFM()
            } label: {
                Label(lastFMStatus.label, systemImage: "music.note.list")
            }
            .buttonStyle(LastFMStatusButtonStyle(isActive: lastFMStatus.isActive))
            .help(lastFMStatus.detail)

            Picker("Skin", selection: $selectedSkin) {
                ForEach(AmpintoshSkin.allCases) { skin in
                    Text(skin.rawValue).tag(skin)
                }
            }
            .labelsHidden()
            .frame(width: 174)

            WindowDot(color: Color(hex: 0x86e36f))
            WindowDot(color: Color(hex: 0xffce4a))
            WindowDot(color: Color(hex: 0xff6b5f))
        }
        .padding(.horizontal, 14)
        .padding(.top, 12)
        .padding(.bottom, 2)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(Color.white.opacity(0.045))
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.22), lineWidth: 1))
        )
        .glassEffect(.regular.tint(skin.tertiary.opacity(0.15)).interactive(), in: .rect(cornerRadius: 6))
        .padding(.horizontal, 12)
        .padding(.top, 10)
    }
}

struct OutputDeviceIndicator: View {
    let device: AudioOutputDevice
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: device.iconName)
                .font(.system(size: 13, weight: .semibold))
                .frame(width: 18, height: 18)

            Text(device.name)
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 160, alignment: .leading)
        }
        .foregroundStyle(skin.text)
        .padding(.horizontal, 9)
        .padding(.vertical, 5)
        .background(skin.display.opacity(0.28), in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.16), lineWidth: 1))
        .glassEffect(.regular.tint(skin.tertiary.opacity(0.12)).interactive(), in: .rect(cornerRadius: 6))
        .help("Current output device: \(device.name)")
    }
}

struct LastFMSettingsView: View {
    @Binding var configuration: LastFMConfiguration
    @Environment(\.dismiss) private var dismiss
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("LAST.FM")
                        .font(.system(size: 18, weight: .black, design: .monospaced))
                    Text("Scrobble local playback")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(skin.mutedText)
                }

                Spacer()

                Toggle("Enabled", isOn: $configuration.isEnabled)
                    .toggleStyle(.switch)
            }

            VStack(alignment: .leading, spacing: 10) {
                CredentialField(label: "API KEY", text: $configuration.apiKey, isSecret: false)
                CredentialField(label: "SHARED SECRET", text: $configuration.sharedSecret, isSecret: true)
                CredentialField(label: "SESSION KEY", text: $configuration.sessionKey, isSecret: true)
            }

            Text("Uses Last.fm's authenticated scrobbling API. Ampintosh sends Now Playing at playback start and scrobbles after half the track or 4 minutes.")
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundStyle(skin.mutedText)
                .fixedSize(horizontal: false, vertical: true)

            HStack {
                Label(configuration.isReady ? "READY" : "NEEDS SETUP", systemImage: configuration.isReady ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .font(.system(size: 11, weight: .black, design: .monospaced))
                    .foregroundStyle(configuration.isReady ? skin.primary : skin.secondary)

                Spacer()

                Button("Done") {
                    dismiss()
                }
                .buttonStyle(SmallCommandButtonStyle())
            }
        }
        .padding(18)
        .background(skin.panel.opacity(0.96))
        .environment(\.ampintoshSkin, skin)
    }
}

struct CredentialField: View {
    let label: String
    @Binding var text: String
    var isSecret: Bool
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(label)
                .font(.system(size: 10, weight: .black, design: .monospaced))
                .foregroundStyle(skin.mutedText)

            Group {
                if isSecret {
                    SecureField(label, text: $text)
                } else {
                    TextField(label, text: $text)
                }
            }
            .textFieldStyle(.plain)
            .font(.system(size: 12, weight: .semibold, design: .monospaced))
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(skin.display.opacity(0.5), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.16), lineWidth: 1))
        }
    }
}

struct VerticalResizeHandle: View {
    @Binding var width: CGFloat
    @Environment(\.ampintoshSkin) private var skin
    @State private var dragStartWidth: CGFloat?

    var body: some View {
        RoundedRectangle(cornerRadius: 2)
            .fill(skin.mutedText.opacity(0.26))
            .frame(width: 6, height: 220)
            .overlay {
                VStack(spacing: 4) {
                    ForEach(0..<6, id: \.self) { _ in
                        Circle()
                            .fill(skin.text.opacity(0.35))
                            .frame(width: 2, height: 2)
                    }
                }
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture().onChanged { value in
                    if dragStartWidth == nil {
                        dragStartWidth = width
                    }
                    width = min(max((dragStartWidth ?? width) + value.translation.width, 340), 620)
                }
                .onEnded { _ in
                    dragStartWidth = nil
                }
            )
            .help("Drag to resize playlist width")
    }
}

struct HorizontalResizeHandle: View {
    @Binding var height: CGFloat
    @Environment(\.ampintoshSkin) private var skin
    @State private var dragStartHeight: CGFloat?

    var body: some View {
        RoundedRectangle(cornerRadius: 2)
            .fill(skin.mutedText.opacity(0.26))
            .frame(height: 7)
            .overlay {
                HStack(spacing: 4) {
                    ForEach(0..<18, id: \.self) { _ in
                        Circle()
                            .fill(skin.text.opacity(0.28))
                            .frame(width: 2, height: 2)
                    }
                }
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture().onChanged { value in
                    if dragStartHeight == nil {
                        dragStartHeight = height
                    }
                    height = min(max((dragStartHeight ?? height) + value.translation.height, 110), 300)
                }
                .onEnded { _ in
                    dragStartHeight = nil
                }
            )
            .help("Drag to resize visualizer height")
    }
}

struct MainDeck: View {
    @Bindable var player: PlayerModel
    @Binding var isImporting: Bool

    var body: some View {
        RetroPanel {
            VStack(spacing: 12) {
                HStack(alignment: .top, spacing: 10) {
                    TrackDisplay(player: player)
                    SpectrumView(levels: player.spectrumLevels)
                        .frame(width: 126, height: 74)
                }

                ProgressSlider(progress: player.progress) { progress in
                    player.seek(to: progress)
                }
                .frame(height: 18)

                HStack(spacing: 7) {
                    TransportButton(systemName: "backward.fill", label: "Previous") { player.previous() }
                    TransportButton(systemName: player.isPlaying ? "pause.fill" : "play.fill", label: "Play") { player.togglePlay() }
                    TransportButton(systemName: "stop.fill", label: "Stop") { player.stop() }
                    TransportButton(systemName: "forward.fill", label: "Next") { player.next() }
                    TransportButton(systemName: "shuffle", label: player.shuffle ? "Shuffle On" : "Shuffle Off", isActive: player.shuffle) { player.shuffle.toggle() }
                    TransportButton(systemName: "folder.fill", label: "Open") { isImporting = true }
                }

                HStack(spacing: 10) {
                    SliderRow(label: "VOL", value: $player.volume, range: 0...2) {
                        player.updateVolume()
                    }
                    .overlay(alignment: .trailing) {
                        Text("\(Int(player.volume * 100))%")
                            .font(.system(size: 9, weight: .black, design: .monospaced))
                            .foregroundStyle(player.volume > 1 ? Color(hex: 0xffcf5f) : Color(hex: 0xdce8f7))
                            .padding(.trailing, 2)
                            .allowsHitTesting(false)
                    }
                    SliderRow(label: "BAL", value: $player.balance, range: -1...1) {
                        player.updateBalance()
                    }
                }
            }
        }
    }
}

struct TrackDisplay: View {
    @Bindable var player: PlayerModel
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        HStack(spacing: 10) {
            ArtworkTile(data: player.selectedTrack?.artworkData, isPlaying: player.isPlaying)
                .frame(width: 78, height: 78)

            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    Text(player.isPlaying ? "PLAY" : "STOP")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundStyle(player.isPlaying ? skin.primary : skin.secondary)

                    Spacer()

                    Text("\(player.elapsedDisplay) / \(player.durationDisplay)")
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundStyle(skin.primary)
                }

                Text(player.selectedTrack?.title ?? "Drop in a stack of FLAC, MP3, AAC, WAV, AIFF, OGG, OPUS...")
                    .font(.system(size: 16, weight: .black, design: .monospaced))
                    .foregroundStyle(
                        LinearGradient(
                            colors: [skin.text, skin.primary],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    )
                    .lineLimit(1)
                    .minimumScaleFactor(0.65)
                    .frame(maxWidth: .infinity, alignment: .leading)

                Text(player.selectedTrack?.metadataSummary.isEmpty == false ? player.selectedTrack?.metadataSummary ?? "" : "No embedded artist or album metadata")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(skin.text.opacity(0.78))
                    .lineLimit(1)

                Text(player.selectedTrack?.technicalSummary ?? "Open a file to inspect bit depth, sample rate, channels, and artwork")
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .foregroundStyle(skin.mutedText.opacity(0.82))
                    .lineLimit(1)

                HStack(spacing: 6) {
                    FormatChip(text: player.selectedTrack?.format ?? "READY")
                    FormatChip(text: player.selectedTrack?.bitDepth.map { "\($0)-BIT" } ?? "BIT --")
                    FormatChip(text: player.selectedTrack?.sampleRate.map { String(format: "%.1f KHZ", $0 / 1000) } ?? "RATE --")
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, minHeight: 112, maxHeight: 112)
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(
                    LinearGradient(
                        colors: [
                            skin.display.opacity(0.72),
                            skin.panel.opacity(0.46),
                            Color.white.opacity(0.06)
                        ],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(skin.primary.opacity(0.28), lineWidth: 1))
                .overlay(alignment: .topLeading) {
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(.white.opacity(0.28), lineWidth: 1)
                        .padding(1)
                }
        )
        .glassEffect(.regular.tint(skin.primary.opacity(0.18)).interactive(), in: .rect(cornerRadius: 4))
    }
}

struct EqualizerPanel: View {
    @Bindable var player: PlayerModel

    private let bands = ["55", "110", "220", "440", "880", "1.8", "3.5", "7", "14", "20"]

    var body: some View {
        RetroPanel {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("GRAPHIC EQUALIZER")
                        .font(.system(size: 11, weight: .black, design: .monospaced))
                        .foregroundStyle(Color(hex: 0xf0f4ff))
                    Spacer()
                    ToggleChip(title: "SHUF", isOn: $player.shuffle)
                    Button(player.repeatMode.rawValue) {
                        player.toggleRepeatMode()
                    }
                    .buttonStyle(ChipButtonStyle(isActive: player.repeatMode != .none))
                }

                HStack(spacing: 8) {
                    VerticalSlider(label: "PRE", value: $player.preamp)
                    ForEach(bands, id: \.self) { band in
                        VerticalSlider(label: band, value: $player.gain)
                    }
                }
                .frame(height: 124)
            }
        }
    }
}

struct ArtworkTile: View {
    let data: Data?
    let isPlaying: Bool
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 7)
                .fill(
                    LinearGradient(
                        colors: [skin.panel, skin.primary.opacity(0.22), skin.display],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )

            if let image {
                image
                    .resizable()
                    .scaledToFill()
                    .clipShape(RoundedRectangle(cornerRadius: 7))
            } else {
                VStack(spacing: 5) {
                    Image(systemName: "opticaldisc.fill")
                        .font(.system(size: 28, weight: .semibold))
                    Text("ART")
                        .font(.system(size: 9, weight: .black, design: .monospaced))
                }
                .foregroundStyle(isPlaying ? skin.primary : skin.mutedText)
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 7).stroke(.white.opacity(0.22), lineWidth: 1))
        .glassEffect(.regular.tint(skin.primary.opacity(isPlaying ? 0.16 : 0.06)).interactive(), in: .rect(cornerRadius: 7))
        .clipped()
    }

    private var image: Image? {
        guard let data else { return nil }
        #if os(macOS)
        guard let nsImage = NSImage(data: data) else { return nil }
        return Image(nsImage: nsImage)
        #else
        return nil
        #endif
    }
}

struct VisualizationPanel: View {
    @Bindable var player: PlayerModel
    @Binding var mode: VisualizationMode
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Text("VISUALIZER")
                    .font(.system(size: 11, weight: .black, design: .monospaced))
                    .foregroundStyle(skin.text)

                Spacer()

                Picker("Visualizer", selection: $mode) {
                    ForEach(VisualizationMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(width: 150)
            }

            TimelineView(.animation) { timeline in
                Canvas { context, size in
                    let time = timeline.date.timeIntervalSinceReferenceDate
                    drawBackground(in: &context, size: size)

                    switch mode {
                    case .spectrum:
                        drawSpectrum(in: &context, size: size)
                    case .mirror:
                        drawMirror(in: &context, size: size, time: time)
                    case .scope:
                        drawScope(in: &context, size: size, time: time)
                    case .fractal:
                        drawFractal(in: &context, size: size, time: time)
                    case .orbit:
                        drawOrbit(in: &context, size: size, time: time)
                    case .rings:
                        drawRings(in: &context, size: size, time: time)
                    case .tunnel:
                        drawTunnel(in: &context, size: size, time: time)
                    case .radial:
                        drawRadial(in: &context, size: size, time: time)
                    case .lissajous:
                        drawLissajous(in: &context, size: size, time: time)
                    case .bloom:
                        drawBloom(in: &context, size: size, time: time)
                    case .particles:
                        drawParticles(in: &context, size: size, time: time)
                    case .matrix:
                        drawMatrix(in: &context, size: size, time: time)
                    }
                }
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(skin.display.opacity(0.48))
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.14), lineWidth: 1))
                )
                .glassEffect(.regular.tint(skin.tertiary.opacity(0.10)).interactive(), in: .rect(cornerRadius: 6))
            }
        }
    }

    private func drawBackground(in context: inout GraphicsContext, size: CGSize) {
        let gridColor = skin.mutedText.opacity(0.08)
        for x in stride(from: 0.0, through: size.width, by: 28) {
            var path = Path()
            path.move(to: CGPoint(x: x, y: 0))
            path.addLine(to: CGPoint(x: x, y: size.height))
            context.stroke(path, with: .color(gridColor), lineWidth: 0.6)
        }

        for y in stride(from: 0.0, through: size.height, by: 24) {
            var path = Path()
            path.move(to: CGPoint(x: 0, y: y))
            path.addLine(to: CGPoint(x: size.width, y: y))
            context.stroke(path, with: .color(gridColor), lineWidth: 0.6)
        }
    }

    // Shorthands for the live, real audio analysis.
    private var bass: Double { player.bassLevel }
    private var lowMid: Double { player.lowMidLevel }
    private var mid: Double { player.midLevel }
    private var treble: Double { player.trebleLevel }
    private var amp: Double { player.amplitude }      // RMS loudness 0...1
    private var beat: Double { player.peak }           // transient peak 0...1

    /// Classic bars. Height per bar = that band's real magnitude (frequency).
    /// Overall brightness/glow scales with live amplitude.
    private func drawSpectrum(in context: inout GraphicsContext, size: CGSize) {
        let levels = player.spectrumLevels
        guard !levels.isEmpty else { return }
        let spacing = 3.0
        let barWidth = max(2, (size.width - spacing * Double(levels.count + 1)) / Double(levels.count))
        let glow = 0.5 + amp * 0.5

        for (index, level) in levels.enumerated() {
            let height = max(3, size.height * 0.84 * level)
            let x = spacing + Double(index) * (barWidth + spacing)
            let rect = CGRect(x: x, y: size.height - height - 8, width: barWidth, height: height)
            let hue = 0.34 - Double(index) / Double(levels.count) * 0.30
            let color = Color(hue: hue, saturation: 0.78, brightness: 0.7 + level * 0.3,
                              opacity: (0.45 + level * 0.5) * glow)
            context.fill(Path(roundedRect: rect, cornerRadius: 1.5), with: .color(color))

            // Peak cap that rides on amplitude.
            let capY = size.height - max(3, size.height * 0.84 * min(1, level + beat * 0.12)) - 10
            let cap = CGRect(x: x, y: capY, width: barWidth, height: 2)
            context.fill(Path(cap), with: .color(skin.text.opacity(0.35 + amp * 0.4)))
        }
    }

    /// Mirrored bars radiating from the centre line, with a glow that pumps on
    /// the beat. Each bar is a real frequency band.
    private func drawMirror(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let levels = player.spectrumLevels
        guard !levels.isEmpty else { return }
        let spacing = 3.0
        let barWidth = max(2, (size.width - spacing * Double(levels.count + 1)) / Double(levels.count))
        let midY = size.height * 0.5
        let drive = 0.4 + amp * 0.6

        for (index, level) in levels.enumerated() {
            let h = size.height * 0.46 * level * (0.7 + drive * 0.5)
            let x = spacing + Double(index) * (barWidth + spacing)
            let hue = 0.5 - Double(index) / Double(levels.count) * 0.34
            let color = Color(hue: hue, saturation: 0.7, brightness: 0.8 + level * 0.2,
                              opacity: 0.4 + level * 0.55)
            context.fill(Path(CGRect(x: x, y: midY - h, width: barWidth, height: h)), with: .color(color))
            context.fill(Path(CGRect(x: x, y: midY, width: barWidth, height: h)), with: .color(color.opacity(0.55)))
        }

        var centre = Path()
        centre.move(to: CGPoint(x: 0, y: midY))
        centre.addLine(to: CGPoint(x: size.width, y: midY))
        context.stroke(centre, with: .color(skin.secondary.opacity(0.35 + beat * 0.5)), lineWidth: 1 + beat * 2)
    }

    /// Real oscilloscope. Plots the actual downsampled PCM waveform, so it
    /// reacts to both the literal signal shape (frequency) and its level
    /// (amplitude). Falls back to a gentle idle line at rest.
    private func drawScope(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let wave = player.waveform
        guard wave.count > 1 else { return }
        let midY = size.height * 0.5
        let gain = size.height * 0.46 * (0.6 + amp * 1.4)
        var path = Path()

        for (i, sample) in wave.enumerated() {
            let x = size.width * Double(i) / Double(wave.count - 1)
            let idle = player.isPlaying ? 0 : sin(Double(i) * 0.16 + time * 0.8) * 0.04
            let y = midY - (sample + idle) * gain
            if i == 0 { path.move(to: CGPoint(x: x, y: y)) } else { path.addLine(to: CGPoint(x: x, y: y)) }
        }

        context.stroke(path, with: .color(skin.tertiary.opacity(0.28 + amp * 0.3)), lineWidth: 6)
        context.stroke(path, with: .color(skin.primary.opacity(player.isPlaying ? 0.95 : 0.4)), lineWidth: 2)
    }

    /// Recursive tree. Trunk thickness/length follow the bass, branching angle
    /// follows the mids, sparkle/extra splits follow the treble, and the whole
    /// thing's vigour is gated by live amplitude.
    private func drawFractal(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let reactiveScale = player.isPlaying ? amp : amp * 0.35

        var branches: [(start: CGPoint, length: Double, angle: Double, depth: Int)] = [
            (CGPoint(x: size.width * 0.5, y: size.height * 0.96),
             size.height * (0.22 + bass * 0.22), -.pi / 2, 0)
        ]

        let sway = sin(time * (player.isPlaying ? 1.6 + treble * 2.4 : 0.25)) * (0.12 + mid * 0.42)
        let maxDepth = player.isPlaying ? 6 + Int(treble * 3) : 6

        while let branch = branches.popLast() {
            let depthPulse = player.spectrumLevels[branch.depth % player.spectrumLevels.count]
            let jitter = sin(time * 2.1 + Double(branch.depth) * 1.7) * depthPulse * 0.11
            let end = CGPoint(
                x: branch.start.x + cos(branch.angle + jitter) * branch.length,
                y: branch.start.y + sin(branch.angle + jitter) * branch.length
            )

            var path = Path()
            path.move(to: branch.start)
            path.addLine(to: end)

            let opacity = max(0.16, 0.5 + reactiveScale * 0.6 - Double(branch.depth) * 0.09)
            let color = Color(
                hue: 0.26 + treble * 0.2 + Double(branch.depth).truncatingRemainder(dividingBy: 3) * 0.025,
                saturation: 0.62 + mid * 0.28,
                brightness: 0.7 + reactiveScale * 0.3,
                opacity: opacity
            )
            context.stroke(path, with: .color(color), lineWidth: max(0.7, 2.0 + bass * 5.4 - Double(branch.depth) * 0.38))

            if branch.depth < maxDepth {
                let nextLength = branch.length * (0.58 + bass * 0.14 + depthPulse * 0.07)
                let split = 0.34 + mid * 0.42 + sway
                branches.append((end, nextLength, branch.angle - split - treble * 0.06, branch.depth + 1))
                branches.append((end, nextLength, branch.angle + split + treble * 0.06, branch.depth + 1))

                if player.isPlaying, treble > 0.4, branch.depth.isMultiple(of: 3) {
                    branches.append((end, nextLength * 0.72, branch.angle + sin(time + Double(branch.depth)) * 0.75, branch.depth + 1))
                }
            }
        }
    }

    /// Particles orbiting the centre; each particle's size pulses with its own
    /// frequency band, and orbital speed/spread responds to amplitude.
    private func drawOrbit(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.5)
        let radius = min(size.width, size.height) * (0.3 + amp * 0.14)
        let speed = player.isPlaying ? (0.7 + amp * 1.1) : 0.22

        for index in 0..<48 {
            let energy = player.spectrumLevels[index % player.spectrumLevels.count]
            let t = time * speed * (0.8 + energy * 1.7) + Double(index) * 0.38
            let x = center.x + cos(t * 1.7) * radius * cos(Double(index) * 0.09)
            let y = center.y + sin(t * 2.1) * radius * sin(Double(index) * 0.13)
            let dotSize = 2.0 + energy * 6.0 + beat * 2.0
            let rect = CGRect(x: x - dotSize / 2, y: y - dotSize / 2, width: dotSize, height: dotSize)
            let color = index.isMultiple(of: 3) ? skin.secondary : skin.primary
            context.fill(Path(ellipseIn: rect), with: .color(color.opacity(player.isPlaying ? 0.5 + energy * 0.45 : 0.3)))
        }
    }

    /// Concentric rings keyed to bass/mid/treble, with a pulsing core sized by
    /// live amplitude and beat transients.
    private func drawRings(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.5)
        let maxRadius = min(size.width, size.height) * 0.48
        let ringCount = 12

        for ring in 0..<ringCount {
            let level = player.spectrumLevels[ring * player.spectrumLevels.count / ringCount]
            let pulse = sin(time * (1.4 + bass * 3.6) + Double(ring) * 0.72) * 0.5 + 0.5
            let radius = maxRadius * (Double(ring + 1) / Double(ringCount + 1)) * (0.8 + amp * 0.25 + pulse * level * 0.22)
            let rect = CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)
            let color = ring.isMultiple(of: 2) ? skin.primary : (ring.isMultiple(of: 3) ? skin.secondary : skin.tertiary)
            context.stroke(Path(ellipseIn: rect), with: .color(color.opacity(0.16 + level * 0.6)), lineWidth: 1.2 + mid * 3.6)
        }

        let coreRadius = 8 + bass * 32 + beat * 22
        let core = CGRect(x: center.x - coreRadius, y: center.y - coreRadius, width: coreRadius * 2, height: coreRadius * 2)
        context.fill(Path(ellipseIn: core), with: .color(skin.secondary.opacity(player.isPlaying ? 0.55 + beat * 0.35 : 0.25)))
    }

    /// Flight-through tunnel; travel speed follows amplitude, ring thickness and
    /// spoke reach follow per-band frequency content.
    private func drawTunnel(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.52)
        let speed = player.isPlaying ? 0.7 + amp * 2.6 : 0.15
        let depthCount = 14

        for depth in 0..<depthCount {
            let normalized = Double(depth) / Double(depthCount)
            let level = player.spectrumLevels[depth * player.spectrumLevels.count / depthCount]
            let phase = (time * speed + normalized).truncatingRemainder(dividingBy: 1)
            let scale = pow(phase, 1.7)
            let width = size.width * (0.16 + scale * 1.18 + level * 0.1)
            let height = size.height * (0.12 + scale * 1.02 + level * 0.1)
            let rect = CGRect(x: center.x - width / 2, y: center.y - height / 2, width: width, height: height)
            let opacity = max(0.08, (1 - scale) * 0.72 + level * 0.24)
            context.stroke(Path(roundedRect: rect, cornerRadius: 4), with: .color(skin.primary.opacity(opacity)), lineWidth: 0.8 + level * 3.0)
        }

        for spoke in 0..<12 {
            let angle = Double(spoke) / 12 * .pi * 2 + time * (0.18 + amp * 0.4)
            let level = player.spectrumLevels[spoke * player.spectrumLevels.count / 12]
            var path = Path()
            path.move(to: center)
            path.addLine(to: CGPoint(x: center.x + cos(angle) * size.width * (0.58 + level * 0.24),
                                     y: center.y + sin(angle) * size.height * (0.58 + level * 0.24)))
            context.stroke(path, with: .color(skin.tertiary.opacity(0.12 + level * 0.3)), lineWidth: 0.8)
        }
    }

    /// Radial spectrum: a full circle of bars, one per band, length = magnitude.
    /// The ring rotates slowly and the inner disc breathes with amplitude.
    private func drawRadial(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let levels = player.spectrumLevels
        guard !levels.isEmpty else { return }
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.5)
        let inner = min(size.width, size.height) * (0.12 + amp * 0.05)
        let reach = min(size.width, size.height) * 0.34
        let rotation = time * (0.15 + amp * 0.5)

        for (index, level) in levels.enumerated() {
            let angle = rotation + Double(index) / Double(levels.count) * .pi * 2
            let length = inner + reach * level
            let start = CGPoint(x: center.x + cos(angle) * inner, y: center.y + sin(angle) * inner)
            let end = CGPoint(x: center.x + cos(angle) * length, y: center.y + sin(angle) * length)
            var path = Path()
            path.move(to: start)
            path.addLine(to: end)
            let hue = Double(index) / Double(levels.count) * 0.5 + 0.05
            context.stroke(path, with: .color(Color(hue: hue, saturation: 0.75, brightness: 1.0, opacity: 0.4 + level * 0.5)),
                           lineWidth: 1.5 + level * 3)
        }

        let discR = inner * (0.7 + beat * 0.6)
        let disc = CGRect(x: center.x - discR, y: center.y - discR, width: discR * 2, height: discR * 2)
        context.fill(Path(ellipseIn: disc), with: .color(skin.primary.opacity(0.25 + amp * 0.4)))
    }

    /// Lissajous figure. X is driven by bass-band phase, Y by treble-band phase,
    /// so the curve's shape encodes the frequency balance while its size tracks
    /// amplitude.
    private func drawLissajous(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.5)
        let rx = size.width * 0.36 * (0.4 + amp * 0.8)
        let ry = size.height * 0.4 * (0.4 + amp * 0.8)
        let a = 2.0 + bass * 4.0
        let b = 3.0 + treble * 5.0
        let delta = time * (0.4 + mid * 1.2)
        let steps = 256
        var path = Path()

        for step in 0...steps {
            let t = Double(step) / Double(steps) * .pi * 2
            let x = center.x + sin(a * t + delta) * rx
            let y = center.y + sin(b * t) * ry
            if step == 0 { path.move(to: CGPoint(x: x, y: y)) } else { path.addLine(to: CGPoint(x: x, y: y)) }
        }

        context.stroke(path, with: .color(skin.tertiary.opacity(0.25 + amp * 0.3)), lineWidth: 5)
        context.stroke(path, with: .color(skin.primary.opacity(0.5 + amp * 0.4)), lineWidth: 1.6)
    }

    /// Blooming flower. Petal count comes from the dominant frequency content,
    /// petal length from each band, and the bloom opens/closes with amplitude.
    private func drawBloom(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.5)
        let base = min(size.width, size.height) * 0.46
        let petals = player.spectrumLevels.count
        let openness = 0.35 + amp * 0.65
        let spin = time * (0.1 + bass * 0.6)

        for (index, level) in player.spectrumLevels.enumerated() {
            let angle = spin + Double(index) / Double(petals) * .pi * 2
            let reach = base * (0.2 + level * openness)
            let ctrl = base * (0.1 + level * openness * 0.6)
            let tip = CGPoint(x: center.x + cos(angle) * reach, y: center.y + sin(angle) * reach)
            let leftCtrl = CGPoint(x: center.x + cos(angle - 0.16) * ctrl, y: center.y + sin(angle - 0.16) * ctrl)
            let rightCtrl = CGPoint(x: center.x + cos(angle + 0.16) * ctrl, y: center.y + sin(angle + 0.16) * ctrl)

            var petal = Path()
            petal.move(to: center)
            petal.addQuadCurve(to: tip, control: leftCtrl)
            petal.addQuadCurve(to: center, control: rightCtrl)

            let hue = 0.85 + Double(index) / Double(petals) * 0.25
            context.fill(petal, with: .color(Color(hue: hue.truncatingRemainder(dividingBy: 1), saturation: 0.7, brightness: 1.0,
                                                    opacity: 0.12 + level * 0.5)))
        }

        let coreR = 4 + beat * 14
        context.fill(Path(ellipseIn: CGRect(x: center.x - coreR, y: center.y - coreR, width: coreR * 2, height: coreR * 2)),
                     with: .color(skin.secondary.opacity(0.5 + amp * 0.4)))
    }

    /// Particle fountain. Each particle is anchored to a frequency band: louder
    /// bands push their particles higher, and beats give the whole field a kick.
    private func drawParticles(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let levels = player.spectrumLevels
        guard !levels.isEmpty else { return }
        let perBand = 4

        for (index, level) in levels.enumerated() {
            let baseX = size.width * (Double(index) + 0.5) / Double(levels.count)
            for p in 0..<perBand {
                let seed = Double(index * perBand + p)
                let phase = (time * (0.4 + level * 1.6 + beat * 0.6) + seed * 0.37).truncatingRemainder(dividingBy: 1)
                let lift = (level * 0.85 + 0.15) * size.height * (1 - phase)
                let wobble = sin(time * 2 + seed) * (4 + level * 10)
                let x = baseX + wobble
                let y = size.height - lift
                let dot = 1.4 + level * 4 + beat * 1.5
                let fade = (1 - phase) * (0.3 + level * 0.6)
                let color = p.isMultiple(of: 2) ? skin.primary : skin.tertiary
                context.fill(Path(ellipseIn: CGRect(x: x - dot / 2, y: y - dot / 2, width: dot, height: dot)),
                             with: .color(color.opacity(fade)))
            }
        }
    }

    /// Falling spectrogram. The bottom row is the live spectrum; each frame the
    /// columns shed brightness as they "fall", giving a waterfall of frequency
    /// energy whose vibrancy tracks amplitude.
    private func drawMatrix(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let levels = player.spectrumLevels
        guard !levels.isEmpty else { return }
        let cols = levels.count
        let rows = 16
        let cellW = size.width / Double(cols)
        let cellH = size.height / Double(rows)
        let scroll = (time * (2 + amp * 6)).truncatingRemainder(dividingBy: 1)

        for col in 0..<cols {
            let level = levels[col]
            for row in 0..<rows {
                let depth = (Double(row) + scroll) / Double(rows)
                // Energy a cell shows: the band level, dimmed as it rises.
                let lit = level * (1 - depth) * (0.6 + amp * 0.5)
                guard lit > 0.04 else { continue }
                let rect = CGRect(x: Double(col) * cellW + 0.6,
                                  y: size.height - Double(row + 1) * cellH + 0.6,
                                  width: cellW - 1.2, height: cellH - 1.2)
                let hue = 0.34 - Double(col) / Double(cols) * 0.3
                context.fill(Path(roundedRect: rect, cornerRadius: 1),
                             with: .color(Color(hue: hue, saturation: 0.7, brightness: 0.7 + lit * 0.3, opacity: min(0.95, lit * 1.3))))
            }
        }
    }
}

struct PlaylistPanel: View {
    @Bindable var player: PlayerModel
    @Binding var isImporting: Bool
    @Binding var visualizationMode: VisualizationMode
    @Binding var visualizerHeight: CGFloat
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        RetroPanel {
            VStack(spacing: 9) {
                HStack(spacing: 8) {
                    Text("PLAYLIST EDITOR")
                        .font(.system(size: 12, weight: .black, design: .monospaced))
                        .foregroundStyle(skin.text)
                    Spacer()
                    Text("\(player.playlist.count) FILES")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundStyle(skin.mutedText)
                    Button {
                        isImporting = true
                    } label: {
                        Label("Add", systemImage: "plus")
                    }
                    .buttonStyle(SmallCommandButtonStyle())
                }

                VisualizationPanel(player: player, mode: $visualizationMode)
                    .frame(height: visualizerHeight)

                HorizontalResizeHandle(height: $visualizerHeight)

                ScrollView {
                    LazyVStack(spacing: 5) {
                        if player.playlist.isEmpty {
                            EmptyPlaylistView()
                                .frame(maxWidth: .infinity, minHeight: 280)
                        } else {
                            ForEach(Array(player.playlist.enumerated()), id: \.element.id) { index, track in
                                PlaylistRow(
                                    index: index + 1,
                                    track: track,
                                    isSelected: track.id == player.selectedTrackID,
                                    isPlaying: track.id == player.selectedTrackID && player.isPlaying,
                                    onPlay: { player.load(track, autoPlay: true) },
                                    onRemove: { player.remove(track) }
                                )
                            }
                        }
                    }
                    .padding(8)
                }
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(skin.display.opacity(0.58))
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.16), lineWidth: 1))
                )
                .glassEffect(.regular.tint(skin.tertiary.opacity(0.10)), in: .rect(cornerRadius: 6))
                .contextMenu {
                    Button {
                        isImporting = true
                    } label: {
                        Label("Load Audio Files...", systemImage: "folder")
                    }

                    if !player.playlist.isEmpty {
                        Button {
                            player.sortPlaylistByTitle()
                        } label: {
                            Label("Sort by Title", systemImage: "textformat")
                        }

                        Button {
                            player.removeMissingFiles()
                        } label: {
                            Label("Remove Missing Files", systemImage: "exclamationmark.triangle")
                        }

                        Button(role: .destructive) {
                            player.clearPlaylist()
                        } label: {
                            Label("Clear Playlist", systemImage: "trash")
                        }
                    }
                }
            }
        }
    }
}

struct PlaylistRow: View {
    let index: Int
    let track: Track
    let isSelected: Bool
    let isPlaying: Bool
    let onPlay: () -> Void
    let onRemove: () -> Void
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        HStack(spacing: 9) {
            Text(String(format: "%02d", index))
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(skin.mutedText)
                .frame(width: 28, alignment: .trailing)

            VStack(alignment: .leading, spacing: 2) {
                Text(track.title)
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundStyle(isSelected ? skin.primary : skin.text)
                    .lineLimit(1)
                Text(track.subtitle)
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .foregroundStyle(skin.mutedText)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            Text(track.format)
                .font(.system(size: 10, weight: .black, design: .monospaced))
                .foregroundStyle(skin.display)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(skin.secondary, in: RoundedRectangle(cornerRadius: 2))

            Text(format(track.duration))
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(skin.text.opacity(0.82))
                .frame(width: 46, alignment: .trailing)

            Button(action: onPlay) {
                Image(systemName: isPlaying ? "speaker.wave.2.fill" : "play.fill")
                    .frame(width: 18, height: 18)
            }
            .buttonStyle(.plain)
            .foregroundStyle(isPlaying ? skin.primary : skin.text)
            .help("Play")

            Button(action: onRemove) {
                Image(systemName: "xmark")
                    .frame(width: 18, height: 18)
            }
            .buttonStyle(.plain)
            .foregroundStyle(skin.secondary)
            .help("Remove")
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 3)
                .fill(isSelected ? skin.primary.opacity(0.22) : skin.panel.opacity(0.42))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(isSelected ? skin.primary.opacity(0.5) : Color.white.opacity(0.11), lineWidth: 1))
        )
        .glassEffect(.regular.tint((isSelected ? skin.primary : skin.tertiary).opacity(isSelected ? 0.15 : 0.07)).interactive(), in: .rect(cornerRadius: 4))
        .contentShape(Rectangle())
        .onTapGesture(perform: onPlay)
        .contextMenu {
            Button(action: onPlay) {
                Label("Play", systemImage: "play.fill")
            }

            Button(role: .destructive, action: onRemove) {
                Label("Remove from Playlist", systemImage: "xmark")
            }
        }
    }

    private func format(_ interval: TimeInterval) -> String {
        guard interval.isFinite, interval > 0 else { return "--:--" }
        let totalSeconds = Int(interval.rounded())
        return String(format: "%02d:%02d", totalSeconds / 60, totalSeconds % 60)
    }
}

struct EmptyPlaylistView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "waveform.path.ecg.rectangle")
                .font(.system(size: 38, weight: .semibold))
                .foregroundStyle(Color(hex: 0x5d748d))
            Text("OPEN AUDIO FILES")
                .font(.system(size: 15, weight: .black, design: .monospaced))
                .foregroundStyle(Color(hex: 0xdce8f7))
            Text("MP3 AAC FLAC WAV AIFF OGG OPUS WMA APE WV M3U M3U8")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Color(hex: 0x7f93a8))
                .multilineTextAlignment(.center)
                .lineLimit(2)
        }
        .padding(24)
    }
}

struct SpectrumView: View {
    let levels: [Double]
    @Environment(\.ampintoshSkin) private var skin

    private let displayBars = 16

    // Average the (now 32) real bands down to a tidy set of bars for the small tile.
    private var bars: [Double] {
        guard levels.count > displayBars else { return levels }
        let groupSize = Double(levels.count) / Double(displayBars)
        return (0..<displayBars).map { i in
            let lo = Int(Double(i) * groupSize)
            let hi = max(lo + 1, Int(Double(i + 1) * groupSize))
            let slice = levels[lo..<min(hi, levels.count)]
            return slice.isEmpty ? 0 : slice.reduce(0, +) / Double(slice.count)
        }
    }

    var body: some View {
        HStack(alignment: .bottom, spacing: 3) {
            ForEach(Array(bars.enumerated()), id: \.offset) { _, level in
                RoundedRectangle(cornerRadius: 1)
                    .fill(barGradient)
                    .frame(height: max(4, 70 * level))
            }
        }
        .padding(8)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(skin.display.opacity(0.58))
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.14), lineWidth: 1))
        )
        .glassEffect(.regular.tint(skin.tertiary.opacity(0.16)).interactive(), in: .rect(cornerRadius: 6))
    }

    private var barGradient: LinearGradient {
        LinearGradient(colors: [skin.primary, skin.tertiary.opacity(0.85), skin.secondary], startPoint: .bottom, endPoint: .top)
    }
}

struct ProgressSlider: View {
    let progress: Double
    let onSeek: (Double) -> Void

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .leading) {
                Capsule()
                    .fill(Color(hex: 0x0a1118).opacity(0.58))
                    .overlay(Capsule().stroke(.white.opacity(0.14), lineWidth: 1))
                Capsule()
                    .fill(LinearGradient(colors: [Color(hex: 0x7dff78), Color(hex: 0xd9fff0), Color(hex: 0xffcf5f)], startPoint: .leading, endPoint: .trailing))
                    .frame(width: max(8, geometry.size.width * progress))
                RoundedRectangle(cornerRadius: 2)
                    .fill(.white.opacity(0.82))
                    .frame(width: 12, height: 18)
                    .offset(x: max(0, min(geometry.size.width - 12, geometry.size.width * progress - 6)))
                    .shadow(color: .black.opacity(0.45), radius: 2, y: 1)
                    .glassEffect(.regular.tint(Color.white.opacity(0.24)).interactive(), in: .rect(cornerRadius: 2))
            }
            .gesture(DragGesture(minimumDistance: 0).onChanged { value in
                onSeek(value.location.x / max(1, geometry.size.width))
            })
        }
    }
}

struct SliderRow: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    var onChange: () -> Void = { }
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.system(size: 10, weight: .black, design: .monospaced))
                .foregroundStyle(skin.text)
                .frame(width: 26, alignment: .leading)
            Slider(value: $value, in: range)
                .tint(skin.primary)
                .onChange(of: value) { _, _ in onChange() }
        }
    }
}

struct VerticalSlider: View {
    let label: String
    @Binding var value: Double
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        VStack(spacing: 5) {
            Slider(value: $value, in: 0...1)
                .rotationEffect(.degrees(-90))
                .tint(skin.primary)
                .frame(width: 82, height: 18)
                .padding(.vertical, 29)
            Text(label)
                .font(.system(size: 8, weight: .black, design: .monospaced))
                .foregroundStyle(skin.mutedText)
                .frame(width: 30)
        }
        .frame(width: 28)
    }
}

struct TransportButton: View {
    let systemName: String
    let label: String
    var isActive = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 13, weight: .black))
                .frame(width: 42, height: 28)
        }
        .buttonStyle(TransportButtonStyle(isActive: isActive))
        .help(label)
    }
}

struct ToggleChip: View {
    let title: String
    @Binding var isOn: Bool

    var body: some View {
        Button(title) {
            isOn.toggle()
        }
        .buttonStyle(ChipButtonStyle(isActive: isOn))
    }
}

struct FormatChip: View {
    let text: String
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        Text(text)
            .font(.system(size: 9, weight: .black, design: .monospaced))
            .foregroundStyle(skin.display)
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .background(skin.primary.opacity(0.82), in: RoundedRectangle(cornerRadius: 4))
            .glassEffect(.regular.tint(skin.primary.opacity(0.18)), in: .rect(cornerRadius: 4))
    }
}

struct WindowDot: View {
    let color: Color

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 11, height: 11)
            .overlay(Circle().stroke(.white.opacity(0.35), lineWidth: 1))
    }
}

struct RetroPanel<Content: View>: View {
    @ViewBuilder let content: Content
    @Environment(\.ampintoshSkin) private var skin

    var body: some View {
        content
            .padding(12)
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .fill(
                        LinearGradient(
                            colors: [
                                Color.white.opacity(0.18),
                                skin.tertiary.opacity(0.11),
                                skin.panel.opacity(0.34)
                            ],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        )
                    )
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.white.opacity(0.28), lineWidth: 1))
                    .overlay(alignment: .topLeading) {
                        RoundedRectangle(cornerRadius: 10)
                            .stroke(LinearGradient(colors: [.white.opacity(0.72), .clear], startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1)
                            .padding(1)
                    }
                    .shadow(color: .black.opacity(0.26), radius: 16, y: 8)
            )
            .glassEffect(.regular.tint(skin.tertiary.opacity(0.16)).interactive(), in: .rect(cornerRadius: 10))
    }
}

struct LiquidRetroBackground: View {
    let palette: SkinPalette

    var body: some View {
        LinearGradient(
            colors: palette.background,
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
            .overlay {
                VStack(spacing: 5) {
                    ForEach(0..<72, id: \.self) { index in
                        Rectangle()
                            .fill(Color.white.opacity(index.isMultiple(of: 3) ? 0.028 : 0.008))
                            .frame(height: 1)
                    }
                }
            }
            .overlay(alignment: .topLeading) {
                LinearGradient(
                    colors: [palette.primary.opacity(0.18), .clear],
                    startPoint: .topLeading,
                    endPoint: .center
                )
            }
            .overlay(alignment: .bottomTrailing) {
                LinearGradient(
                    colors: [palette.secondary.opacity(0.13), .clear],
                    startPoint: .bottomTrailing,
                    endPoint: .center
                )
            }
            .ignoresSafeArea()
    }
}

struct TransportButtonStyle: ButtonStyle {
    var isActive = false
    @Environment(\.ampintoshSkin) private var skin

    func makeBody(configuration: Configuration) -> some View {
        let highlighted = configuration.isPressed || isActive

        configuration.label
            .foregroundStyle(highlighted ? skin.display : skin.text)
            .background(
                RoundedRectangle(cornerRadius: 7)
                    .fill(
                        LinearGradient(
                            colors: highlighted
                                ? [skin.primary.opacity(0.72), skin.secondary.opacity(0.48)]
                                : [Color.white.opacity(0.24), skin.tertiary.opacity(0.22)],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    )
                    .overlay(RoundedRectangle(cornerRadius: 7).stroke(Color.white.opacity(0.22), lineWidth: 1))
            )
            .glassEffect(.regular.tint((isActive ? skin.primary : skin.tertiary).opacity(0.15)).interactive(), in: .rect(cornerRadius: 7))
            .opacity(configuration.isPressed ? 0.8 : 1)
    }
}

struct ChipButtonStyle: ButtonStyle {
    let isActive: Bool
    @Environment(\.ampintoshSkin) private var skin

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 9, weight: .black, design: .monospaced))
            .foregroundStyle(isActive ? skin.display : skin.text)
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .background(isActive ? skin.primary.opacity(0.82) : skin.panel.opacity(0.45), in: RoundedRectangle(cornerRadius: 5))
            .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color.white.opacity(0.18), lineWidth: 1))
            .glassEffect(.regular.tint((isActive ? skin.primary : skin.tertiary).opacity(0.16)).interactive(), in: .rect(cornerRadius: 5))
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

struct LastFMStatusButtonStyle: ButtonStyle {
    let isActive: Bool
    @Environment(\.ampintoshSkin) private var skin

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 9, weight: .black, design: .monospaced))
            .foregroundStyle(isActive ? skin.display : skin.text)
            .labelStyle(.titleAndIcon)
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(
                isActive ? skin.primary.opacity(0.82) : skin.panel.opacity(0.45),
                in: RoundedRectangle(cornerRadius: 6)
            )
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.white.opacity(0.18), lineWidth: 1))
            .glassEffect(.regular.tint((isActive ? skin.primary : skin.tertiary).opacity(0.14)).interactive(), in: .rect(cornerRadius: 6))
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

struct SmallCommandButtonStyle: ButtonStyle {
    @Environment(\.ampintoshSkin) private var skin

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 11, weight: .bold, design: .monospaced))
            .foregroundStyle(skin.display)
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(skin.secondary.opacity(0.78), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.22), lineWidth: 1))
            .glassEffect(.regular.tint(skin.secondary.opacity(0.18)).interactive(), in: .rect(cornerRadius: 6))
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

extension Color {
    init(hex: UInt) {
        self.init(
            red: Double((hex >> 16) & 0xff) / 255,
            green: Double((hex >> 8) & 0xff) / 255,
            blue: Double(hex & 0xff) / 255
        )
    }
}

#Preview {
    ContentView()
}

#Playground {
    _ = 1 + 2
}
