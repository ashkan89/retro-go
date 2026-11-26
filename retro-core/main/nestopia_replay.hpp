#pragma once

#include <NstApiFds.hpp>
#include <NstApiMachine.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Masks use the native NES pad bits (A through Right). A disk side of -1
// leaves the drive alone. The mask persists until the next event.
struct NestopiaReplay
{
    enum Mode { Normal, NoLcd, NoAudio, Core, Unprofiled };
    struct Event { unsigned frame, buttons; int side; };
    unsigned warmup = 10, seconds = 60;
    Mode mode = Normal;
    unsigned buttons = 0;
    size_t next = 0;
    std::vector<Event> events;

    bool load(FILE *file)
    {
        char line[128];
        while (std::fgets(line, sizeof(line), file))
        {
            if (!std::strchr(line, '\n') && !std::feof(file))
                return false;
            char key[16], extra, value[16];
            if (std::sscanf(line, "%15s", key) != 1 || key[0] == '#')
                continue;
            if (!std::strcmp(key, "frame"))
            {
                Event event;
                if (std::sscanf(line, "%*s %u %x %d %c", &event.frame, &event.buttons, &event.side, &extra) != 3 ||
                    event.frame > 100000 || event.buttons > 255 || event.side < -1 || event.side > 255 ||
                    events.size() == 1024 || (!events.empty() && events.back().frame >= event.frame))
                    return false;
                events.push_back(event);
            }
            else if (!std::strcmp(key, "mode"))
            {
                if (std::sscanf(line, "%*s %15s %c", value, &extra) != 1)
                    return false;
                const char *names[] = {"normal", "no-lcd", "no-audio", "core", "unprofiled"};
                unsigned index = 0;
                while (index < sizeof(names) / sizeof(names[0]) && std::strcmp(value, names[index]))
                    ++index;
                if (index == sizeof(names) / sizeof(names[0]))
                    return false;
                mode = static_cast<Mode>(index);
            }
            else if (!std::strcmp(key, "warmup") || !std::strcmp(key, "seconds"))
            {
                unsigned count;
                if (std::sscanf(line, "%*s %u %c", &count, &extra) != 1 || count > 600)
                    return false;
                if (key[0] == 'w')
                    warmup = count;
                else if (count)
                    seconds = count;
                else
                    return false;
            }
            else
                return false;
        }
        return !std::ferror(file);
    }

    const Event *at(unsigned frame)
    {
        if (next == events.size() || events[next].frame != frame)
            return nullptr;
        buttons = events[next].buttons;
        return &events[next++];
    }
};

inline bool nestopia_change_side(Nes::Api::Fds &fds, int side)
{
    return side >= 0 && unsigned(side) < fds.GetNumSides() &&
           fds.EjectDisk() >= Nes::RESULT_OK && fds.InsertDisk(side / 2, side % 2) >= Nes::RESULT_OK;
}

inline double nestopia_native_fps(Nes::Api::Machine &machine)
{
    using Machine = Nes::Api::Machine;
    if (machine.GetMode() == Machine::PAL)
        return double(Machine::CLK_PAL_DOT) / Machine::CLK_PAL_VSYNC;
    return double(Machine::CLK_NTSC_DOT) / Machine::CLK_NTSC_VSYNC;
}
