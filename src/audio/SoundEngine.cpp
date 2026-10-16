#include "audio/SoundEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
constexpr uint32_t DRIVER_ROM_OFFSET = 0x1F6000;   // the Z80 driver inside the ROM
constexpr uint32_t DRIVER_SIZE = 0x1F80;
constexpr uint16_t RAM_COMMAND = 0x1FFF;           // NEW_OPERATION: the sound id
constexpr uint16_t RAM_VOLUME = 0x1FFC;            // FADE_IN_PARAMETERS / volume of the music
constexpr uint16_t RAM_FADE = 0x1FFD;              // MUSIC_LEVEL target for the 0xFC command

constexpr double Z80_CLOCK = 3579545.0;
constexpr double YM_CLOCK = 7670454.0;
// One OPN2_Clock() is 6 YM clocks (24 of them make a sample): Z80 cycles per call.
constexpr double Z80_CYCLES_PER_OPN_CLOCK = Z80_CLOCK / (YM_CLOCK / 6.0);
// The PSG ticks every 16 Z80 cycles: PSG clocks per output sample.
constexpr double PSG_CLOCKS_PER_SAMPLE = (Z80_CLOCK / 16.0) / (YM_CLOCK / 144.0);

// The summed Nuked output is small compared to the PSG's: bring both to a usable level.
constexpr int FM_GAIN = 15;

const double PSG_VOLUME[16] = {
    1.0, 0.794, 0.631, 0.501, 0.398, 0.316, 0.251, 0.200,
    0.158, 0.126, 0.100, 0.079, 0.063, 0.050, 0.040, 0.0 };
}

SoundEngine::SoundEngine(const std::filesystem::path& rom_path)
{
    std::ifstream f(rom_path, std::ios::binary);
    if (!f) return;
    rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (rom.size() < DRIVER_ROM_OFFSET + DRIVER_SIZE) { rom.clear(); return; }

    // Like InitZ80Driver on the console: copy the driver into the Z80's RAM, then reset.
    std::memcpy(ram, &rom[DRIVER_ROM_OFFSET], DRIVER_SIZE);

    z80_init(&cpu);
    cpu.userdata = this;
    cpu.read_byte = &SoundEngine::ReadByte;
    cpu.write_byte = &SoundEngine::WriteByte;
    cpu.port_in = &SoundEngine::PortIn;
    cpu.port_out = &SoundEngine::PortOut;

    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&ym);
    valid = true;
}

SoundEngine::~SoundEngine()
{
    if (stream)
    {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
}

// ---- Z80 memory map ---------------------------------------------------------------------
//   0000-3FFF  8 KB RAM (mirrored)       4000-5FFF  YM2612 (4 ports, mirrored)
//   6000-60FF  bank register (serial)    7F11       PSG
//   8000-FFFF  window onto the 68000 ROM through the bank register
uint8_t SoundEngine::ReadByte(void* self, uint16_t addr)
{
    auto* s = static_cast<SoundEngine*>(self);
    if (addr < 0x4000) return s->ram[addr & 0x1FFF];
    if (addr < 0x6000) return OPN2_Read(&s->ym, addr & 3);
    if (addr >= 0x8000)
    {
        const uint32_t rom_addr = (s->bank << 15) | (addr & 0x7FFF);
        ++s->rom_reads;
        const uint8_t v = rom_addr < s->rom.size() ? s->rom[rom_addr] : 0xFF;
        static const bool trace = std::getenv("LS_ROM_TRACE") != nullptr;
        if (trace && s->rom_reads < 300) SDL_Log("ROM r %06X bank=%u addr=%04X -> %02X (pc=%04X)", rom_addr, s->bank, addr, v, s->cpu.pc);
        return v;
    }
    return 0xFF;
}

void SoundEngine::WriteByte(void* self, uint16_t addr, uint8_t value)
{
    auto* s = static_cast<SoundEngine*>(self);
    if (addr < 0x4000)
    {
        s->ram[addr & 0x1FFF] = value;
    }
    else if (addr < 0x6000)
    {
        OPN2_Write(&s->ym, addr & 3, value);
        ++s->ym_writes;
        if (std::getenv("LS_YM_TRACE") && s->ym_writes < 4000) SDL_Log("YM w port%d = %02X (pc=%04X)", addr & 3, value, s->cpu.pc);
    }
    else if (addr >= 0x6000 && addr < 0x6100)
    {
        // The 9-bit bank number is shifted in one bit per write (bit 0, LSB first).
        s->bank = (s->bank >> 1) | ((value & 1u) << 8);
        ++s->bank_writes;
    }
    else if (addr == 0x7F11)
    {
        s->PsgWrite(value);
        ++s->psg_writes;
    }
}

// ---- SN76489 -----------------------------------------------------------------------------
void SoundEngine::PsgWrite(uint8_t v)
{
    if (v & 0x80)
    {
        psg.latched_channel = (v >> 5) & 3;
        psg.latched_volume = (v & 0x10) != 0;
        const uint8_t data = v & 0x0F;
        if (psg.latched_volume)
        {
            psg.atten[psg.latched_channel] = data;
        }
        else if (psg.latched_channel < 3)
        {
            psg.tone_reg[psg.latched_channel] = (psg.tone_reg[psg.latched_channel] & 0x3F0) | data;
        }
        else
        {
            psg.noise_reg = data & 7;
            psg.lfsr = 0x8000;
        }
    }
    else
    {
        if (psg.latched_volume)
            psg.atten[psg.latched_channel] = v & 0x0F;
        else if (psg.latched_channel < 3)
            psg.tone_reg[psg.latched_channel] = (psg.tone_reg[psg.latched_channel] & 0x00F) | ((v & 0x3F) << 4);
        else
        {
            psg.noise_reg = v & 7;
            psg.lfsr = 0x8000;
        }
    }
}

void SoundEngine::PsgTick()
{
    for (int c = 0; c < 3; ++c)
    {
        if (--psg.tone_counter[c] <= 0)
        {
            psg.tone_counter[c] = std::max<int>(1, psg.tone_reg[c]);
            psg.tone_out[c] ^= 1;
        }
    }
    if (--psg.noise_counter <= 0)
    {
        static const int rates[3] = { 0x10, 0x20, 0x40 };
        const int r = psg.noise_reg & 3;
        psg.noise_counter = (r < 3) ? rates[r] : std::max<int>(1, psg.tone_reg[2]);
        psg.noise_out ^= 1;
        if (psg.noise_out)   // the shift register advances on a rising edge
        {
            const bool white = (psg.noise_reg & 4) != 0;
            const unsigned fb = white ? (((psg.lfsr & 1) ^ ((psg.lfsr >> 3) & 1)) & 1) : (psg.lfsr & 1);
            psg.lfsr = static_cast<uint16_t>((psg.lfsr >> 1) | (fb << 15));
        }
    }
}

int SoundEngine::PsgSample() const
{
    double sum = 0.0;
    for (int c = 0; c < 3; ++c)
        sum += (psg.tone_out[c] ? 1.0 : -1.0) * PSG_VOLUME[psg.atten[c]];
    sum += ((psg.lfsr & 1) ? 1.0 : -1.0) * PSG_VOLUME[psg.atten[3]];
    return static_cast<int>(sum * 1300.0);
}

// ---- Commands ----------------------------------------------------------------------------
// What the 68000's VBlank handler does with a queued sound id (see sub_13B6 in system.asm).
void SoundEngine::Command(int id)
{
    id &= 0xFF;
    if (!music_enabled && id >= 0x01 && id <= 0x2F) return;
    if (!sfx_enabled && id >= 0x41 && id <= 0x7A) return;
    std::lock_guard<std::mutex> g(lock);
    pending.push_back(id & 0xFF);
}

void SoundEngine::DeliverCommands()
{
    // The driver clears the command byte when it has taken it.
    // The driver's start-up routine plays a silent song and clears the command byte: wait
    // until it has reached its main loop (about 50 ms) before talking to it.
    if (cpu.cyc < 200000) return;
    while (!pending.empty() && ram[RAM_COMMAND] == 0)
    {
        const int id = pending.front();
        pending.pop_front();
        if (id == 0xFC)
        {
            ram[RAM_FADE] = 0x0F;
            ram[RAM_COMMAND] = 0xFC;
        }
        else if (id >= 0xFD)
        {
            ram[RAM_COMMAND] = static_cast<uint8_t>(id);
        }
        else
        {
            if (id <= 0x40) ram[RAM_VOLUME] = 0x0F;
            ram[RAM_COMMAND] = static_cast<uint8_t>(id);
        }
    }
}

// ---- Emulation loop ----------------------------------------------------------------------
void SoundEngine::Render(int16_t* out, int frames)
{
    std::lock_guard<std::mutex> g(lock);
    if (!valid)
    {
        std::memset(out, 0, sizeof(int16_t) * 2 * frames);
        return;
    }
    for (int f = 0; f < frames; ++f)
    {
        if ((f & 63) == 0) DeliverCommands();

        int fm_l = 0, fm_r = 0;
        for (int clock = 0; clock < 24; ++clock)
        {
            // The Z80 and the YM2612 run side by side: give the Z80 the cycles that
            // correspond to one YM clock before advancing the chip.
            z80_budget += Z80_CYCLES_PER_OPN_CLOCK;
            while (z80_budget >= 1.0)
            {
                const unsigned long before = cpu.cyc;
                z80_step(&cpu);
                z80_budget -= static_cast<double>(cpu.cyc - before);
            }
            // The chip time-multiplexes its six channels over the 24 clocks of a sample.
            Bit16s out_lr[2];
            OPN2_Clock(&ym, out_lr);
            fm_l += out_lr[0];
            fm_r += out_lr[1];
        }

        psg.clock_acc += PSG_CLOCKS_PER_SAMPLE;
        while (psg.clock_acc >= 1.0)
        {
            PsgTick();
            psg.clock_acc -= 1.0;
        }
        static const char* mix = std::getenv("LS_MIX");
        const int p = (mix && mix[0] == 'f') ? 0 : PsgSample();
        if (mix && mix[0] == 'p') { fm_l = 0; fm_r = 0; }
        // The YM2612's DAC rests off zero: a one-pole high-pass (about 4 Hz) takes the offset
        // out, which also avoids a click when a sound starts or stops.
        const double in[2] = { static_cast<double>(fm_l * FM_GAIN + p), static_cast<double>(fm_r * FM_GAIN + p) };
        int outv[2];
        for (int c = 0; c < 2; ++c)
        {
            const double y = (in[c] - dc_prev_in[c] + 0.9995 * dc_prev_out[c]);
            dc_prev_in[c] = in[c];
            dc_prev_out[c] = y;
            outv[c] = std::clamp(static_cast<int>(y * volume.load()), -32768, 32767);
        }
        const int l = outv[0];
        const int r = outv[1];
        out[2 * f] = static_cast<int16_t>(l);
        out[2 * f + 1] = static_cast<int16_t>(r);
    }
}

void SDLCALL SoundEngine::AudioCallback(void* userdata, SDL_AudioStream* stream, int additional, int)
{
    auto* self = static_cast<SoundEngine*>(userdata);
    const int frames = additional / 4;   // 16-bit stereo
    if (frames <= 0) return;
    std::vector<int16_t> buffer(static_cast<std::size_t>(frames) * 2);
    self->Render(buffer.data(), frames);
    SDL_PutAudioStreamData(stream, buffer.data(), frames * 4);

    static const bool debug = std::getenv("LS_SOUND_DEBUG") != nullptr;
    if (debug)
    {
        static unsigned long total = 0, calls = 0;
        static double energy = 0.0;
        static int peak = 0;
        total += frames;
        ++calls;
        for (int16_t v : buffer) { energy += static_cast<double>(v) * v; peak = std::max(peak, std::abs(static_cast<int>(v))); }
        if (calls % 100 == 0)
        {
            SDL_Log("[sound] %lu calls, %lu frames, rms %.0f, peak %d", calls, total,
                    std::sqrt(energy / (2.0 * std::max(1ul, total))), peak);
        }
    }
}

bool SoundEngine::StartOutput()
{
    if (!valid || stream) return stream != nullptr;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = SAMPLE_RATE;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SoundEngine::AudioCallback, this);
    if (!stream) return false;
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Log("Audio output opened: %d Hz stereo, driver %s", SAMPLE_RATE, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return true;
}

void SoundEngine::LogState(const char* tag) const
{
    SDL_Log("[sound %s] pc=%04X sp=%04X halted=%d iff1=%d cmd=%02X ym_writes=%lu psg_writes=%lu bank_writes=%lu bank=%u rom_reads=%lu cyc=%lu",
            tag, cpu.pc, cpu.sp, cpu.halted, cpu.iff1, ram[RAM_COMMAND], ym_writes, psg_writes, bank_writes, bank, rom_reads, cpu.cyc);
}

bool SoundEngine::DumpWav(const std::filesystem::path& wav, int id, double seconds)
{
    if (!valid) return false;
    LogState("start");
    if (const char* pre = std::getenv("LS_SOUND_PRE")) Command(std::atoi(pre));   // test: a command sent first
    Command(id);
    const int total = static_cast<int>(seconds * SAMPLE_RATE);
    std::vector<int16_t> pcm(static_cast<std::size_t>(total) * 2);
    for (int done = 0; done < total; done += 4096)
        Render(pcm.data() + static_cast<std::size_t>(done) * 2, std::min(4096, total - done));
    LogState("end");

    std::ofstream f(wav, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); w32(36 + data_bytes); f.write("WAVEfmt ", 8);
    w32(16); w16(1); w16(2); w32(SAMPLE_RATE); w32(SAMPLE_RATE * 4); w16(4); w16(16);
    f.write("data", 4); w32(data_bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return true;
}
