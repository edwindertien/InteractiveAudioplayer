#pragma once

// ============================================================
// AudioPlaySdWavMulti
//
// Plays a 5.1 (6-channel) interleaved WAV file from SD card.
// Single SD read per audio block — zero concurrent-reader contention.
//
// Channel mapping (standard 7.1):
//   ch 0  FL  → output 0  (base L)
//   ch 1  FR  → output 1  (base R)
//   ch 2  FC  → output 2  (narration/center, mono)
//   ch 3  LFE → output 3  (haptic, mono)
//   ch 4  BL  → output 4  (overlay 1 L)
//   ch 5  BR  → output 5  (overlay 1 R)
//   ch 6  SL  → output 6  (overlay 2 L)
//   ch 7  SR  → output 7  (overlay 2 R)
//
// Usage:
//   AudioPlaySdWavMulti player;
//   player.play("experience.wav");
//   player.seekMs(60000);   // jump to 1:00
//   player.stop();
// ============================================================

#include <Audio.h>
#include <SD.h>

class AudioPlaySdWavMulti : public AudioStream {
public:
    static constexpr uint8_t NUM_CHANNELS = 8;

    AudioPlaySdWavMulti() : AudioStream(0, nullptr) {
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) _out[i] = nullptr;
    }

    // ── Playback control ──────────────────────────────────────

    bool play(const char* filename) {
        stop();

        _file = SD.open(filename);
        if (!_file) {
            Serial.printf("[WavMulti] ERROR: cannot open %s\n", filename);
            return false;
        }

        if (!parseHeader()) {
            Serial.printf("[WavMulti] ERROR: invalid WAV header in %s\n", filename);
            _file.close();
            return false;
        }

        _playing = true;
        Serial.printf("[WavMulti] Playing %s — %uch %uHz %ubit %lums\n",
                      filename, _channels, _sampleRate, _bitsPerSample, lengthMs());
        return true;
    }

    void stop() {
        _playing = false;
        if (_file) _file.close();
        _dataRead = 0;
    }

    bool isPlaying() { return _playing; }

    // ── Loop region ───────────────────────────────────────────
    // Define a loop region by start/end ms.
    // loopEnd=0 means loop to end of file.
    // Safe to call from main loop — values are read atomically.
    void setLoop(uint32_t startMs, uint32_t endMs, bool enabled = true) {
        // Write order matters: set start/end before enabling
        _loopStartMs = startMs;
        _loopEndMs   = endMs;
        _stopAtEnd   = false;
        _loopEnabled = enabled;
    }

    // Play the region once — stop (not loop) when loopEndMs is reached
    void setPlayOnce(uint32_t startMs, uint32_t endMs) {
        _loopStartMs = startMs;
        _loopEndMs   = endMs;
        _stopAtEnd   = true;
        _loopEnabled = true;
    }

    // ── Seeking ───────────────────────────────────────────────

    // Seek to a position in milliseconds.
    // SAFE TO CALL FROM MAIN LOOP — deferred to next update() ISR tick.
    void seekMs(uint32_t ms) {
        _seekPending   = true;
        _seekTargetMs  = ms;
    }

    // Internal: actually performs the seek inside the audio ISR
    void _doSeek(uint32_t ms) {
        if (!_file) return;   // seeking can resume playback even if
                               // a previous play-once region just ended

        uint32_t targetByte = (uint32_t)((float)ms / 1000.0f * _byteRate);
        uint32_t frameSize  = _channels * (_bitsPerSample / 8);
        targetByte = (targetByte / frameSize) * frameSize;
        if (targetByte > _dataSize) targetByte = 0;

        uint32_t filePos = _dataOffset + targetByte;
        if (!_file.seek(filePos)) {
            Serial.printf("[WavMulti] seek failed to byte %lu\n", filePos);
            return;
        }
        _dataRead = targetByte;
    }

    // ── Position / length ─────────────────────────────────────

    uint32_t positionMs() {
        if (!_playing || _byteRate == 0) return 0;
        return (uint32_t)((float)_dataRead / _byteRate * 1000.0f);
    }

    uint32_t lengthMs() {
        if (_byteRate == 0) return 0;
        return (uint32_t)((float)_dataSize / _byteRate * 1000.0f);
    }

    // ── AudioStream interface ─────────────────────────────────
    // Called by the audio ISR every 128 samples (~2.9ms)

    virtual void update(void) override {
        // Handle deferred seek FIRST, regardless of _playing state — a
        // seek request means "resume from here," even if a previous
        // play-once region just reached its own natural end and set
        // _playing=false. Without this, once ANY once-chapter finishes
        // naturally, the player locks up permanently: every future
        // seek gets silently dropped by the early-return below.
        if (_seekPending) {
            _seekPending = false;
            if (_file) {
                _doSeek(_seekTargetMs);
                _playing = true;   // a seek always (re)starts playback
            }
        }

        if (!_playing || !_file) {
            // Output silence on all channels
            for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                if (_out[i]) { memset(_out[i]->data, 0, AUDIO_BLOCK_SAMPLES * 2); }
            }
            return;
        }

        // Allocate output blocks
        bool ok = true;
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            _out[i] = allocate();
            if (!_out[i]) { ok = false; break; }
        }
        if (!ok) {
            // Out of memory — release what we got and return silence
            for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                if (_out[i]) { release(_out[i]); _out[i] = nullptr; }
            }
            return;
        }

        // Read one block worth of interleaved samples
        // One frame = NUM_CHANNELS samples (one per channel)
        // One block = AUDIO_BLOCK_SAMPLES frames
        const uint32_t bytesPerSample = _bitsPerSample / 8;
        const uint32_t frameBytes     = NUM_CHANNELS * bytesPerSample;
        const uint32_t blockBytes     = AUDIO_BLOCK_SAMPLES * frameBytes;

        // Read into temporary buffer
        // Max frame: 6ch * 2 bytes * 128 samples = 1536 bytes
        static int16_t readBuf[AUDIO_BLOCK_SAMPLES * NUM_CHANNELS];

        // Check loop end point before reading
        if (_loopEnabled) {
            uint32_t loopEnd = (_loopEndMs > 0) ? _loopEndMs : lengthMs();
            if (positionMs() >= loopEnd) {
                if (_stopAtEnd) {
                    _playing = false;  // end of final chapter — stop cleanly
                } else {
                    _doSeek(_loopStartMs);
                }
            }
        }

        uint32_t bytesRemaining = _dataSize - _dataRead;

        if (bytesRemaining == 0) {
            if (_loopEnabled) {
                // Loop back to loop start point
                seekMs(_loopStartMs);
            } else {
                _playing = false;
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                    memset(_out[i]->data, 0, AUDIO_BLOCK_SAMPLES * 2);
                    transmit(_out[i], i);
                    release(_out[i]);
                    _out[i] = nullptr;
                }
                return;
            }
        }

        uint32_t toRead = min(blockBytes, bytesRemaining);
        int32_t  nRead  = _file.read((uint8_t*)readBuf, toRead);

        if (nRead <= 0) {
            _playing = false;
            for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                memset(_out[i]->data, 0, AUDIO_BLOCK_SAMPLES * 2);
                transmit(_out[i], i);
                release(_out[i]);
                _out[i] = nullptr;
            }
            return;
        }

        _dataRead += nRead;

        // How many complete frames did we read?
        uint32_t framesRead = nRead / frameBytes;

        // Deinterleave: scatter each channel into its output block
        for (uint32_t s = 0; s < framesRead; s++) {
            for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
                _out[ch]->data[s] = readBuf[s * NUM_CHANNELS + ch];
            }
        }

        // Zero-pad if short read (end of file)
        if (framesRead < AUDIO_BLOCK_SAMPLES) {
            for (uint32_t s = framesRead; s < AUDIO_BLOCK_SAMPLES; s++) {
                for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
                    _out[ch]->data[s] = 0;
                }
            }
        }

        // Transmit each channel on its output port
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            transmit(_out[i], i);
            release(_out[i]);
            _out[i] = nullptr;
        }
    }

private:
    // ── WAV header parsing ────────────────────────────────────

    bool parseHeader() {
        uint8_t buf[12];

        // RIFF header
        if (_file.read(buf, 12) < 12) return false;
        if (memcmp(buf, "RIFF", 4) != 0) return false;
        if (memcmp(buf + 8, "WAVE", 4) != 0) return false;

        // Walk chunks until we find fmt and data
        bool hasFmt = false, hasData = false;

        while (!hasData) {
            uint8_t chunkHdr[8];
            if (_file.read(chunkHdr, 8) < 8) return false;

            char     chunkId[5] = {};
            memcpy(chunkId, chunkHdr, 4);
            uint32_t chunkSize = chunkHdr[4]|(chunkHdr[5]<<8)|
                                 (chunkHdr[6]<<16)|(chunkHdr[7]<<24);

            if (memcmp(chunkId, "fmt ", 4) == 0) {
                uint8_t fmt[40];
                uint32_t toRead = min(chunkSize, (uint32_t)40);
                if (_file.read(fmt, toRead) < toRead) return false;
                if (chunkSize > 40) _file.seek(_file.position() + chunkSize - 40);

                uint16_t audioFmt = fmt[0] | (fmt[1] << 8);
                // Accept plain PCM (1) and WAVE_FORMAT_EXTENSIBLE (65534 - used by ffmpeg for multichannel)
                if (audioFmt != 1 && audioFmt != 65534) {
                    Serial.printf("[WavMulti] ERROR: not PCM (format %u)\n", audioFmt);
                    return false;
                }
                _channels      = fmt[2] | (fmt[3] << 8);
                _sampleRate    = fmt[4]|(fmt[5]<<8)|(fmt[6]<<16)|(fmt[7]<<24);
                _byteRate      = fmt[8]|(fmt[9]<<8)|(fmt[10]<<16)|(fmt[11]<<24);
                _bitsPerSample = fmt[14] | (fmt[15] << 8);

                if (_channels != NUM_CHANNELS) {
                    Serial.printf("[WavMulti] ERROR: expected %u channels, got %u\n",
                                  NUM_CHANNELS, _channels);
                    Serial.println("[WavMulti] Need 7.1 (8-channel) WAV — use assemble_71.sh");
                    return false;
                }
                if (_sampleRate != 44100) {
                    Serial.printf("[WavMulti] WARNING: sample rate %u (expected 44100)\n",
                                  _sampleRate);
                }
                if (_bitsPerSample != 16) {
                    Serial.printf("[WavMulti] ERROR: only 16-bit PCM supported\n");
                    return false;
                }
                hasFmt = true;

            } else if (memcmp(chunkId, "data", 4) == 0) {
                _dataOffset = _file.position();
                _dataSize   = chunkSize;
                _dataRead   = 0;
                hasData     = true;

            } else {
                // Skip unknown chunk (LIST, INFO, etc.)
                if (!_file.seek(_file.position() + chunkSize)) return false;
            }
        }

        return hasFmt && hasData;
    }

    // ── Members ───────────────────────────────────────────────
    File       _file;
    bool       _playing       = false;
    bool       _loopEnabled   = false;
    uint32_t   _loopStartMs   = 0;
    uint32_t   _loopEndMs     = 0;     // 0 = use file end
    volatile bool     _seekPending  = false;
    volatile uint32_t _seekTargetMs = 0;
    bool              _stopAtEnd    = false;  // stop instead of loop at loopEndMs

    uint16_t   _channels      = 0;
    uint32_t   _sampleRate    = 0;
    uint32_t   _byteRate      = 0;
    uint16_t   _bitsPerSample = 0;
    uint32_t   _dataOffset    = 0;   // byte offset of data chunk in file
    uint32_t   _dataSize      = 0;   // total bytes in data chunk
    uint32_t   _dataRead      = 0;   // bytes read so far

    audio_block_t* _out[NUM_CHANNELS];
};