#ifndef _BEHAVIOURS_H_
#define _BEHAVIOURS_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The behaviour scripts of the original game (assets/text/behaviours.txt, written by
// landstalker_tools/scripts/behaviour_export.py). An entity's "behaviour" number selects a
// script: a list of commands, each a command byte followed by its parameters.
//
// Command bytes (sizes in bytes, parameter = the byte after the command):
//   00 Pause(ticks) 2        01 MoveTimed(ticks) 2      11 MoveRelative(ticks) 2
//   02..07 Turn CW/CCW/NE/SE/SW/NW 1                    08..0D set direction (same) 1
//   0E/0F make visible / invisible 1                    12 GoTo(signed offset) 2
//   13 MoveUntilCollision 1  14 TurnRandom 1  15 SetDirRandom 1  17..1D turn, immediate 1
//   1E Freeze 1  1F..22 speed 1/2/4/8 1  23 Turn180 1  24/25 set 180 1
//   2A MoveRandomTime(min, max) 3    41 SetFlag / 42 WaitFlagSet / 43 ClearFlag /
//   46 WaitFlagClear (flag byte, bit) 3    44 Hide 1  45 ShowWhenClear 1  3E HideSprite (ends)
//   4E TriggerCutscene(action id) 2    62 PlaySound(id) 2
// Directions are the entity's two orientation bits: 0 NE (-y), 1 SE (+x), 2 SW (+y), 3 NW (-x).
class Behaviours
{
public:
    explicit Behaviours(const std::filesystem::path& txt)
    {
        std::ifstream f(txt);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int index = 0;
            std::string hex;
            if (!(ls >> index >> hex)) continue;
            std::vector<uint8_t> bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
                bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            scripts[index] = std::move(bytes);
        }
    }

    // Scripted controller sequences (assets/text/playback.txt): lists of (buttons, frames).
    // Buttons: 1 up, 2 down, 4 left, 8 right, 0x10 B, 0x20 C, 0x40 A. 254 frames = for good.
    using Playback = std::vector<std::pair<int, int>>;
    void LoadPlayback(const std::filesystem::path& txt)
    {
        std::ifstream f(txt);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            int index = 0;
            if (!(ls >> index)) continue;
            Playback seq;
            std::string tok;
            while (ls >> tok)
            {
                const auto c = tok.find(':');
                if (c == std::string::npos) continue;
                seq.push_back({ std::stoi(tok.substr(0, c), nullptr, 16), std::stoi(tok.substr(c + 1)) });
            }
            playbacks[index] = std::move(seq);
        }
    }
    const Playback* PlaybackSequence(int index) const
    {
        auto it = playbacks.find(index);
        return it == playbacks.end() ? nullptr : &it->second;
    }

    // Does the script take part in a story event (waits for a flag, starts a cutscene or turns
    // the entity hostile / friendly)? Such enemies stay dormant until the script says so.
    bool IsScripted(int index) const
    {
        const auto* s = Script(index);
        if (!s) return false;
        static const std::map<int, int> sizes = {
            { 0x00, 2 }, { 0x01, 2 }, { 0x11, 2 }, { 0x12, 2 }, { 0x2A, 3 }, { 0x41, 3 }, { 0x42, 3 }, { 0x43, 3 },
            { 0x46, 3 }, { 0x4B, 3 }, { 0x4E, 2 }, { 0x62, 2 }, { 0x34, 2 }, { 0x64, 2 }, { 0x36, 2 }, { 0x57, 2 },
            { 0x50, 2 }, { 0x5D, 2 }, { 0x4F, 2 }, { 0x30, 2 }, { 0x31, 2 }, { 0x3A, 2 }, { 0x3B, 2 }, { 0x3C, 2 },
            { 0x3D, 2 }, { 0x4C, 2 }, { 0x47, 2 }, { 0x5A, 2 }, { 0x5F, 2 }, { 0x60, 2 }, { 0x61, 2 }, { 0x63, 2 },
            { 0x65, 2 }, { 0x66, 2 }, { 0x48, 3 }, { 0x59, 3 }, { 0x5B, 5 } };
        for (std::size_t i = 0; i < s->size();)
        {
            const int c = (*s)[i];
            if (c == 0x42 || c == 0x46 || c == 0x4E || c == 0x64 || c == 0x51 || c == 0x52) return true;
            auto it = sizes.find(c);
            i += it == sizes.end() ? 1 : it->second;
        }
        return false;
    }

    // A "director": a script that starts a cutscene and then sets a flag (e.g. the waking-up scene
    // of the mayor's house). Gives that flag, or -1: once it is set the scene does not run again
    // when the room is entered again.
    int DirectorFlag(int index) const
    {
        const auto* s = Script(index);
        if (!s) return -1;
        static const std::map<int, int> sizes = {
            { 0x00, 2 }, { 0x01, 2 }, { 0x11, 2 }, { 0x12, 2 }, { 0x2A, 3 }, { 0x41, 3 }, { 0x42, 3 }, { 0x43, 3 },
            { 0x46, 3 }, { 0x4B, 3 }, { 0x4E, 2 }, { 0x62, 2 }, { 0x34, 2 }, { 0x64, 2 }, { 0x36, 2 }, { 0x57, 2 },
            { 0x50, 2 }, { 0x5D, 2 }, { 0x4F, 2 }, { 0x30, 2 }, { 0x31, 2 }, { 0x3A, 2 }, { 0x3B, 2 }, { 0x3C, 2 },
            { 0x3D, 2 }, { 0x4C, 2 }, { 0x47, 2 }, { 0x5A, 2 }, { 0x5F, 2 }, { 0x60, 2 }, { 0x61, 2 }, { 0x63, 2 },
            { 0x65, 2 }, { 0x66, 2 }, { 0x48, 3 }, { 0x59, 3 }, { 0x5B, 5 } };
        bool cutscene = false;
        int flag = -1;
        for (std::size_t i = 0; i < s->size();)
        {
            const int c = (*s)[i];
            if (c == 0x4E || c == 0x64) cutscene = true;
            if (c == 0x41 && cutscene && i + 2 < s->size()) flag = (*s)[i + 1] * 8 + (*s)[i + 2];
            auto it = sizes.find(c);
            i += it == sizes.end() ? 1 : it->second;
        }
        return flag;
    }

    // nullptr if the behaviour number has no script.
    const std::vector<uint8_t>* Script(int index) const
    {
        auto it = scripts.find(index);
        return it == scripts.end() ? nullptr : &it->second;
    }

private:
    std::map<int, std::vector<uint8_t>> scripts;
    std::map<int, Playback> playbacks;
};

#endif // _BEHAVIOURS_H_
