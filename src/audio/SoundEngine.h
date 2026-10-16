#ifndef _SOUND_ENGINE_H_
#define _SOUND_ENGINE_H_

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "z80.h"
#include "ym3438.h"
}

// Plays the original game's music and sound effects by emulating its sound hardware:
//
//   * the Z80 running the game's own sound driver ("Cube / Idaware" driver, 8064 bytes,
//     found in the ROM at 0x1F6000),
//   * a YM2612 FM chip (Nuked-OPN2) with its DAC for the PCM samples,
//   * an SN76489 PSG.
//
// The 68000 side of the console is replaced by Command(): like the original it writes a
// command byte into the driver's RAM (0x1FFF) and the driver does the rest, reading the
// song data straight from the ROM image through the Z80 bank window.
//
// Commands are the game's sound ids (see landstalker_disasm/code/include/constants/sounds.inc):
//   0x01..0x2F music, 0x41..0x7A sound effects, 0xFD fade out, 0xFE stop.
class SoundEngine
{
public:
    static constexpr int MUSIC_TOWN = 0x01;
    static constexpr int SFX_SWORD_SWING = 0x51;
    static constexpr int SFX_SWORD_HIT = 0x52;
    static constexpr int SFX_JUMP = 0x53;
    static constexpr int SFX_FALL = 0x54;
    static constexpr int SFX_DOOR = 0x55;
    static constexpr int SFX_DOOR_LOCK = 0x58;
    static constexpr int SFX_JUMP_LAND = 0x5D;
    static constexpr int SFX_NIGEL_HIT = 0x61;
    static constexpr int SFX_NIGEL_DIE = 0x65;
    static constexpr int SFX_ENEMY_DIE = 0x66;
    static constexpr int SFX_ENEMY_HIT = 0x67;
    static constexpr int SFX_MENU_OPEN = 0x68;
    static constexpr int SFX_HEALTH = 0x69;
    static constexpr int SFX_PICKUP_MONEY = 0x6A;
    static constexpr int SFX_SWITCH = 0x6C;
    static constexpr int SFX_CURSOR_MOVE = 0x42;
    static constexpr int SFX_CURSOR_SELECT = 0x43;
    static constexpr int SFX_ERROR = 0x44;
    static constexpr int MUSIC_FANFARE = 0x04;
    static constexpr int MUSIC_CHEST_OPEN = 0x05;
    static constexpr int MUSIC_INN_REST = 0x06;
    static constexpr int MUSIC_SAVE_GAME = 0x07;
    static constexpr int SND_FADE_OUT = 0xFD;
    static constexpr int SND_STOP = 0xFE;

    // rom: a Landstalker ROM image (the driver and the songs are read from it).
    explicit SoundEngine(const std::filesystem::path& rom);
    ~SoundEngine();

    SoundEngine(const SoundEngine&) = delete;
    SoundEngine& operator=(const SoundEngine&) = delete;

    bool IsValid() const { return valid; }
    // Opens the SDL audio output and starts playing. False if there is no audio device.
    bool StartOutput();

    // Master volume, 0.0 (silent) to 1.0.
    void SetVolume(float v) { volume = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    float GetVolume() const { return volume; }
    // Music (ids 0x01..0x2F) and sound effects (0x41..0x7A) can be turned off separately.
    void SetMusicEnabled(bool on) { music_enabled = on; if (!on) Command(SND_STOP); }
    void SetSfxEnabled(bool on) { sfx_enabled = on; }
    bool MusicEnabled() const { return music_enabled; }
    bool SfxEnabled() const { return sfx_enabled; }

    // Sends a sound id to the driver (queued if it is still busy with the previous one).
    void Command(int id);
    void PlayMusic(int id) { Command(id); }
    void Stop() { Command(SND_STOP); }

    // Renders interleaved signed 16-bit stereo samples at SAMPLE_RATE (also used by the
    // audio callback). Used directly by the offline test (see DumpWav).
    void Render(int16_t* out, int frames);
    static constexpr int SAMPLE_RATE = 53267;   // the YM2612's native rate

    // Offline render: sends `id`, plays `seconds` and writes a WAV file. For testing.
    bool DumpWav(const std::filesystem::path& wav, int id, double seconds);
    // Counters for debugging (YM / PSG / bank register writes, command byte, Z80 state).
    void LogState(const char* tag) const;

private:
    // Z80 side of the memory map
    static uint8_t ReadByte(void* self, uint16_t addr);
    static void WriteByte(void* self, uint16_t addr, uint8_t value);
    static uint8_t PortIn(z80*, uint8_t) { return 0xFF; }
    static void PortOut(z80*, uint8_t, uint8_t) {}
    static void SDLCALL AudioCallback(void* userdata, SDL_AudioStream* stream, int additional, int total);

    void PsgWrite(uint8_t value);
    void PsgTick();
    int PsgSample() const;
    void DeliverCommands();

    bool valid = false;
    std::vector<uint8_t> rom;
    uint8_t ram[0x2000] = {};
    uint32_t bank = 0;            // 9-bit ROM bank register of the Z80 window (0x8000-0xFFFF)

    z80 cpu;
    ym3438_t ym;
    double z80_budget = 0.0;      // Z80 cycles owed to the emulation
    double dc_prev_in[2] = { 0.0, 0.0 }, dc_prev_out[2] = { 0.0, 0.0 };   // DC-blocking filter state

    // SN76489
    struct Psg
    {
        uint16_t tone_reg[3] = { 0, 0, 0 };
        int tone_counter[3] = { 0, 0, 0 };
        int tone_out[3] = { 1, 1, 1 };
        uint8_t atten[4] = { 15, 15, 15, 15 };   // 15 = silent
        uint8_t noise_reg = 0;
        int noise_counter = 0;
        int noise_out = 1;
        uint16_t lfsr = 0x8000;
        int latched_channel = 0;
        bool latched_volume = false;
        double clock_acc = 0.0;                    // PSG clocks owed
    } psg;

    std::atomic<float> volume{ 1.0f };
    std::atomic<bool> music_enabled{ true }, sfx_enabled{ true };
    unsigned long ym_writes = 0, psg_writes = 0, bank_writes = 0, rom_reads = 0;
    std::deque<int> pending;      // commands waiting for the driver to be free
    std::mutex lock;
    SDL_AudioStream* stream = nullptr;
};

#endif // _SOUND_ENGINE_H_
