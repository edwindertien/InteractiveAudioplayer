#pragma once

// ============================================================
// RamPlayer
// Loads short WAV files from SD into static RAM buffers at boot.
// Playback uses AudioPlayMemory — zero SD access after boot.
//
// AudioPlayMemory requires Teensy's special MEMFILE format:
// a uint16_t array where [0] = number of audio blocks,
// and the rest is raw 16-bit PCM samples.
//
// Usage:
//   static RamPlayer fx;
//   fx.load(0, "touch_a.wav");
//   fx.load(1, "touch_b.wav");
//   // connect: AudioConnection p(fx.player, 0, mixer, 3);
//   fx.play(0);
// ============================================================

#include <Audio.h>
#include <SD.h>

static constexpr uint8_t  RAM_FX_COUNT    = 8;
static constexpr uint32_t RAM_FX_MAX_BYTES = 180000; // ~1s stereo at 44.1kHz

class RamPlayer {
public:
    AudioPlayMemory player;   // connect this to your mixer

    bool load(uint8_t slot, const char* filename) {
        if (slot >= RAM_FX_COUNT) {
            Serial.printf("[RAM] slot %u out of range\n", slot);
            return false;
        }

        File f = SD.open(filename);
        if (!f) {
            Serial.printf("[RAM] cannot open %s\n", filename);
            return false;
        }

        // Parse WAV header to find data chunk
        uint8_t hdr[12];
        if (f.read(hdr, 12) < 12 ||
            memcmp(hdr, "RIFF", 4) != 0 ||
            memcmp(hdr + 8, "WAVE", 4) != 0) {
            Serial.printf("[RAM] %s is not a WAV file\n", filename);
            f.close();
            return false;
        }

        uint32_t dataOffset = 0, dataSize = 0;
        uint16_t channels = 0, bitsPerSample = 0;
        uint32_t sampleRate = 0;

        while (true) {
            uint8_t chunk[8];
            if (f.read(chunk, 8) < 8) break;
            uint32_t size = chunk[4]|(chunk[5]<<8)|(chunk[6]<<16)|(chunk[7]<<24);

            if (memcmp(chunk, "fmt ", 4) == 0) {
                uint8_t fmt[16];
                f.read(fmt, 16);
                if (size > 16) f.seek(f.position() + size - 16);
                channels      = fmt[2] | (fmt[3] << 8);
                sampleRate    = fmt[4]|(fmt[5]<<8)|(fmt[6]<<16)|(fmt[7]<<24);
                bitsPerSample = fmt[14] | (fmt[15] << 8);
            } else if (memcmp(chunk, "data", 4) == 0) {
                dataOffset = f.position();
                dataSize   = size;
                break;
            } else {
                f.seek(f.position() + size);
            }
        }

        if (dataOffset == 0 || dataSize == 0) {
            Serial.printf("[RAM] %s: data chunk not found\n", filename);
            f.close();
            return false;
        }
        if (bitsPerSample != 16) {
            Serial.printf("[RAM] %s: only 16-bit PCM supported\n", filename);
            f.close();
            return false;
        }

        // Clamp to buffer size
        if (dataSize > RAM_FX_MAX_BYTES) {
            Serial.printf("[RAM] WARNING: %s truncated to %u bytes\n",
                          filename, RAM_FX_MAX_BYTES);
            dataSize = RAM_FX_MAX_BYTES;
        }

        // AudioPlayMemory format:
        // word[0] = number of audio blocks
        // word[1..n] = raw 16-bit PCM samples (mono or stereo interleaved)
        uint32_t numSamples  = dataSize / 2; // 16-bit samples
        uint32_t numBlocks   = (numSamples + AUDIO_BLOCK_SAMPLES - 1)
                               / AUDIO_BLOCK_SAMPLES;

        // Total storage = 1 header word + numBlocks * AUDIO_BLOCK_SAMPLES words
        // Header is one 32-bit word = two uint16_t. Samples follow.
        uint32_t storageWords = 2 + numBlocks * AUDIO_BLOCK_SAMPLES;
        uint32_t storageBytes = storageWords * 2;

        if (storageBytes > RAM_FX_MAX_BYTES + 64) {
            Serial.printf("[RAM] slot %u: not enough buffer space\n", slot);
            f.close();
            return false;
        }

        // Allocate if not already done
        if (_buf[slot] == nullptr) {
            _buf[slot] = (uint16_t*)malloc(storageBytes);
            if (!_buf[slot]) {
                Serial.printf("[RAM] slot %u: malloc failed (%lu bytes)\n",
                              slot, storageBytes);
                f.close();
                return false;
            }
        }

        // Write 32-bit header as two uint16_t words (little-endian):
        //   bits 31-24 = type (0x81=mono 16-bit, 0x83=stereo 16-bit)
        //   bits 23-0  = number of SAMPLES (AudioPlayMemory decrements by AUDIO_BLOCK_SAMPLES per tick)
        uint8_t  typeFlag = 0x81; // 16-bit PCM 44100Hz (only supported format)
        uint32_t header   = ((uint32_t)typeFlag << 24) | (numSamples & 0x00FFFFFF);
        _buf[slot][0] = (uint16_t)(header & 0xFFFF);        // low word
        _buf[slot][1] = (uint16_t)((header >> 16) & 0xFFFF); // high word

        // Read raw PCM into buffer (after 2-word header)
        f.seek(dataOffset);
        f.read((uint8_t*)(_buf[slot] + 2), dataSize);
        f.close();

        // Zero-pad to end of last block
        uint32_t samplesRead = dataSize / 2;
        for (uint32_t i = samplesRead; i < numBlocks * AUDIO_BLOCK_SAMPLES; i++) {
            _buf[slot][2 + i] = 0;  // offset by 2 for 32-bit header
        }

        _loaded[slot] = true;
        float durationMs = (float)numSamples / sampleRate * 1000.0f;
        Serial.printf("[RAM] slot %u: %s loaded (%uch %.0fms %lu bytes RAM)\n",
                      slot, filename, channels, durationMs, storageBytes);
        return true;
    }

    void play(uint8_t slot) {
        if (slot >= RAM_FX_COUNT || !_loaded[slot] || !_buf[slot]) return;
        player.play((const unsigned int*)_buf[slot]);
    }

    bool isPlaying() { return player.isPlaying(); }

    void printMemoryUsage() {
        uint32_t total = 0;
        for (uint8_t i = 0; i < RAM_FX_COUNT; i++) {
            if (_buf[i]) total += RAM_FX_MAX_BYTES;
        }
        Serial.printf("[RAM] ~%lu bytes used across %u slots\n", total, RAM_FX_COUNT);
    }

private:
    uint16_t* _buf[RAM_FX_COUNT]    = {};
    bool      _loaded[RAM_FX_COUNT] = {};
};