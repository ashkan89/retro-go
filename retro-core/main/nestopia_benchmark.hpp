#pragma once

#include "nestopia_replay.hpp"

struct NestopiaBenchmark
{
    NestopiaReplay replay;
    std::string manifest;
    unsigned frame = 0, warmupFrames = 0, timedFrames = 0;
    unsigned submitted = 0, deadlines = 0, samples = 0, sideChanges = 0;
    unsigned borrowedRgbFrames = 0, skippedRasterFrames = 0;
    uint64_t executeUs = 0, audioUs = 0, videoUs = 0, acquireUs = 0, blitUs = 0, sideUs = 0;
    uint16_t intervals[1001] = {}; // 100 us bins through 100 ms
    uint32_t maxQueued = 0;
    uint32_t failedAudioStart = 0;
    int64_t loadStart = 0, biosUs = 0, imageUs = 0, bootDone = 0, timedStart = 0;
    int64_t previousEnd = 0, deadlineUs = 0;
    int64_t firstRead = -1, firstIdle = -1, sideStart = -1, sideRead = -1;
    rg_display_transfer_stats_t displayStart = {};
    rg_audio_queue_stats_t audioStart = {};
    double fps = 0;

    static void NST_CALLBACK drive_event(void *data, Nes::Api::Fds::Motor motor)
    {
        auto &bench = *static_cast<NestopiaBenchmark *>(data);
        if (motor == Nes::Api::Fds::MOTOR_READ)
        {
            if (bench.firstRead < 0)
                bench.firstRead = rg_system_timer() - bench.bootDone;
            if (bench.sideStart >= 0 && bench.sideRead < 0)
                bench.sideRead = rg_system_timer() - bench.sideStart;
        }
        else if (motor == Nes::Api::Fds::MOTOR_OFF && bench.firstRead >= 0 && bench.firstIdle < 0)
            bench.firstIdle = rg_system_timer() - bench.bootDone;
    }

    void open(const char *rom)
    {
        manifest = std::string(rom) + ".bench";
        FILE *file = std::fopen(manifest.c_str(), "r");
        if (!file)
            RG_PANIC("Nestopia benchmark could not open the manifest.");
        const bool valid = replay.load(file);
        std::fclose(file);
        if (!valid)
            RG_PANIC("Nestopia benchmark manifest is invalid.");
        loadStart = rg_system_timer();
    }

    void configure(double nativeFps)
    {
        fps = nativeFps;
        warmupFrames = unsigned(fps * replay.warmup + 0.5);
        timedFrames = unsigned(fps * replay.seconds + 0.5);
        deadlineUs = int64_t(1000000.0 / fps + 0.5);
        bootDone = rg_system_timer();
        Nes::Api::Fds::driveCallback.Set(drive_event, this);
    }

    void start_frame(Nes::Api::Fds &fds)
    {
        if (frame == warmupFrames)
        {
            displayStart = rg_display_get_transfer_stats();
            audioStart = rg_audio_get_queue_stats();
            failedAudioStart = rg_audio_get_counters().failedSubmissions;
            timedStart = previousEnd = rg_system_timer();
        }
        const auto *event = replay.at(frame);
        if (event && event->side >= 0)
        {
            const auto start = rg_system_timer();
            if (!nestopia_change_side(fds, event->side))
                RG_PANIC("Nestopia benchmark disk-side change failed.");
            sideStart = start;
            sideRead = -1;
            sideUs += rg_system_timer() - start;
            ++sideChanges;
        }
    }

    bool timed() const { return frame >= warmupFrames; }
    bool profile() const { return timed() && replay.mode != NestopiaReplay::Unprofiled; }
    bool lcd() const { return replay.mode != NestopiaReplay::NoLcd && replay.mode != NestopiaReplay::Core; }
    bool audio() const { return replay.mode != NestopiaReplay::NoAudio && replay.mode != NestopiaReplay::Core; }

    void record(int64_t start, int64_t executed, int64_t audioEnd, int64_t end, unsigned pcm, bool drawn)
    {
        if (!timed())
            return;
        samples += pcm;
        submitted += drawn;
        if (!profile())
            return;
        executeUs += executed - start;
        audioUs += audioEnd - executed;
        videoUs += end - audioEnd;
        const auto interval = end - previousEnd;
        previousEnd = end;
        ++intervals[RG_MIN(interval / 100, 1000)];
        deadlines += interval > deadlineUs + 1000;
        maxQueued = RG_MAX(maxQueued, rg_audio_get_queue_stats().queuedFrames);
    }

    unsigned percentile(unsigned percent) const
    {
        if (replay.mode == NestopiaReplay::Unprofiled)
            return 0;
        unsigned remaining = (timedFrames * percent + 99) / 100;
        for (unsigned bin = 0; bin < RG_COUNT(intervals); ++bin)
        {
            if (intervals[bin] >= remaining)
                return (bin + 1) * 100;
            remaining -= intervals[bin];
        }
        return 0;
    }

    void finish(Nes::Api::Machine &machine, int coreSampleRate, bool spriteLimit, int cropX, int cropY, int frameskip)
    {
        const auto end = rg_system_timer();
        const auto display = rg_display_get_transfer_stats();
        const auto sound = rg_audio_get_queue_stats();
        const auto failedAudio = rg_audio_get_counters().failedSubmissions - failedAudioStart;
        const auto changed = display.changedFrames - displayStart.changedFrames;
        const auto unchanged = display.unchangedUpdates - displayStart.unchangedUpdates;
        const double elapsed = (end - timedStart) / 1000000.0;
        const auto *screen = rg_display_get_info();
        const auto *sink = rg_audio_get_driver();
#ifdef ESP_PLATFORM
        const auto internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const auto psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
        const unsigned long internal = 0, psram = 0;
#endif
        Nes::Api::Fds::driveCallback.Unset();
        // Power-off resets the PPU, including its raster. Finish any borrow
        // before that reset, outside both the gameplay and save timings.
        rg_display_sync(true);
        const auto saveStart = rg_system_timer();
        const auto powerResult = machine.Power(false);
        const auto saveUs = rg_system_timer() - saveStart;
        std::string output = manifest + ".v2.csv";
        const bool header = !rg_storage_exists(output.c_str());
        FILE *csv = std::fopen(output.c_str(), "a");
        if (!csv)
            RG_PANIC("Nestopia benchmark could not open the result file.");
        if (header)
            std::fputs("mode,cpu_mhz,native_fps,core_hz,sink_hz,sink,scaling,filter,viewport_w,viewport_h,sprite_limit,crop_x,crop_y,frameskip,"
                       "warmup_frames,frames,wall_s,emulated_fps,submitted,changed_transfers,unchanged_updates,"
                       "changed_transfer_fps,p50_us,p95_us,p99_us,deadline_misses,skipped_submissions,pcm_frames,"
                       "execute_us,audio_submit_us,video_frontend_us,ppa_scale_us,lcd_drain_us,i2s_write_us,"
                       "i2s_queue_capacity,i2s_queue_max_sampled,i2s_full_waits,i2s_write_errors,i2s_dma_gap_events,failed_audio_submissions,"
                       "free_internal_bytes,free_psram_bytes,init_us,bios_file_us,image_load_us,warmup_us,disk_changes,disk_action_us,"
                       "boot_to_first_drive_read_us,boot_to_first_drive_idle_us,last_side_to_drive_read_us,"
                       "power_off_us,power_off_result,source_acquire_us,rgb_blit_us,borrowed_rgb_frames,skipped_raster_frames\n", csv);
        std::fprintf(csv, "%d,%d,%.6f,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%.6f,%.3f,%u,%lu,%lu,%.3f,"
                     "%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lld,%lld,%lld,%lld,%u,%llu,%lld,%lld,%lld,",
                     replay.mode, rg_system_get_cpu_speed(), fps, coreSampleRate, rg_audio_get_sample_rate(), sink ? sink : "",
                     rg_display_get_scaling(), rg_display_get_filter(), screen->viewport.width, screen->viewport.height,
                     spriteLimit, cropX, cropY, frameskip,
                     warmupFrames, timedFrames, elapsed, timedFrames / elapsed, submitted,
                     (unsigned long)changed, (unsigned long)unchanged, changed / elapsed,
                     percentile(50), percentile(95), percentile(99), deadlines, timedFrames - submitted, samples,
                     (unsigned long long)executeUs, (unsigned long long)audioUs, (unsigned long long)videoUs,
                     (unsigned long)(display.ppaTimeUs - displayStart.ppaTimeUs),
                     (unsigned long)(display.drainTimeUs - displayStart.drainTimeUs),
                     (unsigned long)(sound.dmaWriteUs - audioStart.dmaWriteUs),
                     (unsigned long)sound.capacityFrames, (unsigned long)maxQueued,
                     (unsigned long)(sound.producerWaits - audioStart.producerWaits),
                     (unsigned long)(sound.writeErrors - audioStart.writeErrors),
                     (unsigned long)(sound.dmaGapEvents - audioStart.dmaGapEvents),
                     (unsigned long)failedAudio,
                     (unsigned long)internal, (unsigned long)psram,
                     (long long)(bootDone - loadStart), (long long)biosUs, (long long)imageUs,
                     (long long)(timedStart - bootDone), sideChanges, (unsigned long long)sideUs,
                     (long long)firstRead, (long long)firstIdle, (long long)sideRead);
        std::fprintf(csv, "%lld,%d,%llu,%llu,%u,%u\n", (long long)saveUs, int(powerResult),
                     (unsigned long long)acquireUs, (unsigned long long)blitUs,
                     borrowedRgbFrames, skippedRasterFrames);
        const bool writeFailed = std::ferror(csv);
        if (std::fclose(csv) || writeFailed)
            RG_PANIC("Nestopia benchmark could not close the result file.");
        rg_storage_commit();
    }
};
