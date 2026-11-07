/*
 * Copyright (C) 2021-2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "QtiPerfdClient"

#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>
#include <android/binder_manager.h>
#include <utils/Log.h>

#include <atomic>
#include <mutex>
#include <string>

/*
 * Translation layer for the QCOM perf client API used by closed-source
 * blobs (the CamX camera stack). Instead of discarding the requests (or
 * forwarding QCOM perf-lock opcodes that only the retired perfd daemon
 * understood), the Xiaomi camera hint IDs observed on device are mapped
 * to the named hints in powerhint.json through the Power HAL extension.
 * The perf-lock opcodes/values requested by the blobs are deliberately
 * ignored: powerhint.json is what decides the actual boosts.
 */

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;

namespace {

// Xiaomi CamX hint IDs, captured live from perf_hint_renew calls:
//   0x1337 camera open, 0x1338 still capture, 0x1339 video recording,
//   0x1364 session keepalive (~0.5 renew/s), 0x1366 streaming keepalive
//   (~5 renew/s).
// Only the open and capture events are translated: cameraservers already
// sends CAMERA_STREAMING_LOW on its own for the whole camera session
// (and ends it on close), so the streaming/video keepalives would only
// collide with that request in the power HAL without adding anything.
constexpr int kHintCamLaunch = 0x1337;
constexpr int kHintCamSnapshot = 0x1338;

// Callers renew faster than this; when a duration is not given, extend
// for this long and let the renewals keep it refreshed.
constexpr int kDefaultDurationMs = 3000;

// Still capture requests a full camera launch boost from CamX, but the
// stock intent is a short pulse; keep it bounded.
constexpr int kSnapshotDurationMs = 1000;

std::mutex gPowerExtLock;
std::shared_ptr<IPowerExt> gPowerExt;
std::atomic<int> gNextHandle{1};

std::shared_ptr<IPowerExt> getPowerExt() {
    std::lock_guard<std::mutex> lock(gPowerExtLock);
    if (!gPowerExt) {
        const std::string instance = std::string(IPower::descriptor) + "/default";
        ndk::SpAIBinder powerBinder(AServiceManager_checkService(instance.c_str()));
        ndk::SpAIBinder extPowerBinder;
        if (powerBinder.get() == nullptr ||
            AIBinder_getExtension(powerBinder.get(), extPowerBinder.getR()) != STATUS_OK ||
            extPowerBinder.get() == nullptr) {
            return nullptr;
        }
        gPowerExt = IPowerExt::fromBinder(extPowerBinder);
    }
    return gPowerExt;
}

void sendBoost(const char* boost, int durationMs) {
    std::shared_ptr<IPowerExt> powerExt = getPowerExt();
    if (powerExt == nullptr) {
        ALOGE("Power HAL extension unavailable, dropping boost %s", boost);
        return;
    }
    if (!powerExt->setBoost(boost, durationMs).isOk()) {
        ALOGE("setBoost(%s, %d ms) failed, dropping cached Power HAL extension", boost,
              durationMs);
        // The HAL may have restarted: drop the stale proxy so the next call
        // reconnects.
        std::lock_guard<std::mutex> lock(gPowerExtLock);
        gPowerExt.reset();
    }
}

}  // namespace

extern "C" {

void perf_event(int arg1, char* arg2, int arg3, int* arg4) {}

int perf_get_feedback(int arg1, char* arg2) {
    return 233;
}

int perf_get_feedback_extn(int arg1, char* arg2, unsigned int arg3, char* arg4) {
    return 233;
}

void perf_hint(int arg1, char* arg2, int arg3, int arg4) {}

int perf_hint_renew(int handle, int hintId, const char* displays, int duration, int numDisplays,
                    int numArgs, int argList[]) {
    const char* boost = nullptr;
    int durationMs = duration;
    switch (hintId) {
        case kHintCamLaunch:
            boost = "CAMERA_LAUNCH";
            break;
        case kHintCamSnapshot:
            boost = "CAMERA_LAUNCH";
            durationMs = kSnapshotDurationMs;
            break;
        default:
            // Unknown or intentionally-untranslated hint IDs keep the
            // previous no-op behavior, but the caller still gets a valid
            // handle to renew and rel with.
            break;
    }
    if (boost != nullptr) {
        sendBoost(boost, durationMs > 0 ? durationMs : kDefaultDurationMs);
    }
    return handle != 0 ? handle : gNextHandle.fetch_add(1);
}

int perf_lock_acq(int handle, int duration, int list[], int numArgs) {
    // Perf-lock opcode lists are Xiaomi-specific and are not used here:
    // the hint renewals above already carry the power intent. Return a
    // valid handle so callers keep their renew/rel bookkeeping.
    return handle != 0 ? handle : gNextHandle.fetch_add(1);
}

void perf_lock_cmd(int arg1) {}

int perf_lock_rel(int handle) {
    // Every boost issued here is duration-bounded and refreshed by
    // renewals, so releasing a handle needs no EndHint in the HAL.
    return handle != 0 ? handle : 233;
}

int perf_lock_use_profile(int handle, int profile) {
    return handle != 0 ? handle : 233;
}

void perf_wait_get_prop(char* arg1, char* arg2) {}

}
