// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See frame_rate.h.
#include "frame_rate.h"

#include <hid/input.h>
#include <platform/thermal.h>
#include <report/report.h>
#include <ui/ui.h>
#include <user/settings.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace video
{
    namespace
    {
        // Auto's choice, and why it's there (FrameRateReason), for any thread.
        std::atomic<int> s_rate{ 60 };
        std::atomic<FrameRateReason> s_reason{ FrameRateReason::None };
        // Frame Rate changed (the menu, a reload): Auto starts over at 60.
        std::atomic<bool> s_restart{ true };
        AutoFrameRatePolicy s_policy;  // command processor thread
        // No control touched for kIdleSeconds: Auto shows 30 (nobody is
        // steering, so 60 is power for nothing); any input ends it at once.
        // Whatever Auto had chosen waits in s_rate, and the policy starts over
        // when the input is back (its window would be 30 fps frames).
        std::atomic<bool> s_idle{ false };
        constexpr double kIdleSeconds = 180.0;

        const bool s_log = [] {
            const char* v = std::getenv("NFSMW_AUTO_FPS_LOG");
            return v && v[0] == '1';
        }();

        constexpr const char* kWarmToast = "30 fps: the device is warm";
        constexpr const char* kCantHoldToast = "30 fps: steadier than a 60 this device can't hold";
        constexpr const char* kBackToast = "Back to 60 fps";
        constexpr double kToastSeconds = 4.0;

        bool AutoOn()
        {
            return settings::GetInt(settings::Id::FrameRate) == settings::kFrameRateAuto;
        }

        // " (thermal fair)"; nothing where there's no reading.
        std::string ThermalSuffix(platform::thermal::State state)
        {
            return state == platform::thermal::State::Unknown ? std::string()
                                                              : std::format(" (thermal {})", platform::thermal::Name(state));
        }

        void Announce(const FrameRateDecision& d, platform::thermal::State thermal)
        {
            const double t = report::SessionSeconds();
            const std::string costs = std::format("CP {:.1f} ms, GPU {:.1f} ms a frame", d.cpMs, d.gpuMs);
            const std::string backoff = d.backoff == 0 ? std::string()
                : std::format("; backing off: {:.0f} s cool and {:.0f} s at 30 before a try", AutoFrameRatePolicy::kCoolSec * (1 << d.backoff),
                      AutoFrameRatePolicy::kTrySec * (1 << d.backoff));
            // One toast a session, at the first drop to 30: a player found
            // more of them (each switch, both ways) a distraction mid-race.
            // The toast's text goes in the line too: a log is all a headless
            // run (or a player's report) has of it.
            static bool s_toasted = false;
            const char* toast = d.rate == 60 ? kBackToast : d.reason == FrameRateReason::Hot ? kWarmToast : kCantHoldToast;
            const bool show = d.rate == 30 && !s_toasted;
            const std::string shown = show ? std::format("; toast \"{}\"", toast) : std::string("; no toast (one a session)");
            switch (d.reason)
            {
            case FrameRateReason::Hot:
                fprintf(stderr, "[frame rate] auto 60 -> 30 at %.1f s: thermal %s (%.1f fps over %.1f s, %s)%s%s\n", t,
                    platform::thermal::Name(thermal), d.fps, d.windowSeconds, costs.c_str(), backoff.c_str(), shown.c_str());
                break;
            case FrameRateReason::CantHold60:
                fprintf(stderr, "[frame rate] auto 60 -> 30 at %.1f s: can't hold 60: %.1f fps (median %.1f) over %.1f s, the slow frames "
                    "CP %.1f ms, GPU %.1f ms a frame%s%s%s\n", t, d.fps, d.medianFps, d.windowSeconds, d.slowCpMs, d.slowGpuMs,
                    ThermalSuffix(thermal).c_str(), backoff.c_str(), shown.c_str());
                break;
            case FrameRateReason::Headroom:
                fprintf(stderr, "[frame rate] auto 30 -> 60 at %.1f s: room for 60 (%s at 30; %.0f s at 30, cool for %.0f s)%s%s\n", t,
                    costs.c_str(), d.rateSeconds, d.coolSeconds, ThermalSuffix(thermal).c_str(), shown.c_str());
                break;
            case FrameRateReason::Try:
                fprintf(stderr, "[frame rate] auto 30 -> 60 at %.1f s: a try after %.0f s at 30 (%s; cool for %.0f s)%s%s\n", t,
                    d.rateSeconds, costs.c_str(), d.coolSeconds, ThermalSuffix(thermal).c_str(), shown.c_str());
                break;
            case FrameRateReason::None:
                return;
            }
            if (!show)
                return;
            s_toasted = true;
            ui::Toast(toast, kToastSeconds);
        }
    }

    int FrameRate()
    {
        int32_t setting = settings::GetInt(settings::Id::FrameRate);
        if (setting != settings::kFrameRateAuto)
            return setting;
        return s_idle.load(std::memory_order_relaxed) ? 30 : s_rate.load(std::memory_order_relaxed);
    }

    void NoteFrameCosts(const FrameSample& sample)
    {
        // Here, not in a static initializer: settings can't be used before main().
        static const uint32_t listener = settings::Subscribe([](settings::Id id) {
            if (id != settings::Id::FrameRate)
                return;
            // Where the restarted policy's first update will be: 30 on a hot
            // device (it drops for that at once), so Auto never shows 60 first.
            const platform::thermal::State thermal = platform::thermal::Current();
            const bool hot = thermal == platform::thermal::State::Serious || thermal == platform::thermal::State::Critical;
            s_rate.store(hot ? 30 : 60, std::memory_order_relaxed);
            s_reason.store(hot ? FrameRateReason::Hot : FrameRateReason::None, std::memory_order_relaxed);
            s_idle.store(false, std::memory_order_relaxed);
            s_restart.store(true, std::memory_order_release);
        });
        (void)listener;
        if (!AutoOn())
            return;
        // Idle: 30 until a control is used. The policy isn't fed meanwhile: it
        // would read the 30 fps it didn't choose as a 60 it can't hold.
        const bool idle = hid::SecondsSinceInput() >= kIdleSeconds;
        if (idle != s_idle.load(std::memory_order_relaxed))
        {
            s_idle.store(idle, std::memory_order_relaxed);
            if (!idle)
                s_restart.store(true, std::memory_order_release);
            if (s_log)
                fprintf(stderr, "[frame rate] auto %s at %.1f s: %s\n", idle ? "30" : "back to Auto's rate",
                    report::SessionSeconds(), idle ? "no input for 180 s" : "input");
        }
        if (idle)
            return;
        if (s_restart.exchange(false, std::memory_order_acquire))
            s_policy.Reset();
        const platform::thermal::State thermal = platform::thermal::Current();
        const FrameRateDecision d = s_policy.Update(report::SessionSeconds(), thermal, sample);
        // Every time, not only on a change: a menu change racing this update
        // is put right by the next one.
        s_rate.store(d.rate, std::memory_order_relaxed);
        if (d.changed)
            s_reason.store(d.reason, std::memory_order_relaxed);
        if (s_log)
        {
            const int before = d.changed ? 90 - d.rate : d.rate;  // the rate this update judged
            const std::string cool = before == 30 ? std::format(", cool for {:.0f} s", d.coolSeconds) : std::string();
            fprintf(stderr, "[frame rate] auto at %d: %.1f fps (median %.1f) over %.1f s, CP %.1f ms, GPU %.1f ms a frame (under %.0f fps: "
                "%.1f, %.1f); thermal %s; %.0f s at %d%s; back-off %d; this sample %.0f draws a frame\n", before, d.fps, d.medianFps,
                d.windowSeconds, d.cpMs, d.gpuMs, AutoFrameRatePolicy::kDropFps, d.slowCpMs, d.slowGpuMs,
                platform::thermal::Name(thermal), d.rateSeconds, before, cool.c_str(), d.backoff, double(sample.draws));
        }
        if (d.changed)
            Announce(d, thermal);
    }

    std::string AutoFrameRateNote()
    {
        if (!AutoOn())
            return "";
        if (s_idle.load(std::memory_order_relaxed))
            return "Now 30 fps: no input.";
        if (s_rate.load(std::memory_order_relaxed) == 60)
            return "Now 60 fps.";
        return s_reason.load(std::memory_order_relaxed) == FrameRateReason::Hot ? "Now 30 fps: the device is warm."
                                                                                : "Now 30 fps: the device can't hold 60 here.";
    }

    std::string AutoFrameRatePerfText()
    {
        return AutoOn() ? std::format(" | auto {} fps{}", FrameRate(), s_idle.load(std::memory_order_relaxed) ? " (idle)" : "")
                        : std::string();
    }
}