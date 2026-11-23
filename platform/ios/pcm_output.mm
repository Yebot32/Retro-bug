// RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#include "pcm_output.h"

#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>

#include <array>
#include <atomic>
#include <cstring>

namespace rb::platform::ios {
namespace {

class AudioOutput final : public audio::PcmOutput {
 public:
  explicit AudioOutput(audio::PcmFormat format) : format_(format) {}

  ~AudioOutput() override {
    if (queue_) AudioQueueDispose(queue_, true);
  }

  OSStatus Open() {
    AudioStreamBasicDescription desc{};
    desc.mSampleRate = format_.sample_rate;
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kLinearPCMFormatFlagIsPacked;
    if (format_.bits == 16) desc.mFormatFlags |= kLinearPCMFormatFlagIsSignedInteger;
    desc.mBytesPerFrame = format_.channels * format_.bits / 8;
    desc.mBytesPerPacket = desc.mBytesPerFrame;
    desc.mFramesPerPacket = 1;
    desc.mChannelsPerFrame = format_.channels;
    desc.mBitsPerChannel = format_.bits;
    return AudioQueueNewOutput(&desc, Completed, this, nullptr, nullptr, 0, &queue_);
  }

  bool Enqueue(std::span<const std::uint8_t> bytes) override {
    const auto frame_bytes = format_.channels * format_.bits / 8;
    if (bytes.empty() || bytes.size() % frame_bytes != 0) return false;
    for (unsigned i = 0; i < format_.buffers; ++i) {
      auto& slot = slots_[i];
      if (slot.busy.load(std::memory_order_acquire)) continue;
      if (slot.buffer && slot.buffer->mAudioDataBytesCapacity < bytes.size()) {
        if (AudioQueueFreeBuffer(queue_, slot.buffer) != noErr) return false;
        slot.buffer = nullptr;
      }
      if (!slot.buffer && AudioQueueAllocateBuffer(queue_, static_cast<UInt32>(bytes.size()),
                                                   &slot.buffer) != noErr) return false;
      slot.buffer->mUserData = &slot;
      std::memcpy(slot.buffer->mAudioData, bytes.data(), bytes.size());
      slot.buffer->mAudioDataByteSize = static_cast<UInt32>(bytes.size());
      slot.busy.store(true, std::memory_order_release);
      if (AudioQueueEnqueueBuffer(queue_, slot.buffer, 0, nullptr) != noErr) {
        slot.busy.store(false, std::memory_order_release);
        return false;
      }
      StartIfReady();
      return true;
    }
    return false;
  }

  void SetPlaying(bool playing) override {
    playing_ = playing;
    if (playing) StartIfReady();
    else if (started_ && AudioQueuePause(queue_) == noErr) started_ = false;
  }

  void Clear() override {
    if (AudioQueueStop(queue_, true) != noErr) return;
    started_ = false;
    for (auto& slot : slots_) slot.busy.store(false, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
  }

  unsigned TakeCompleted() override { return completed_.exchange(0, std::memory_order_acq_rel); }

 private:
  struct Slot {
    AudioQueueBufferRef buffer = nullptr;
    std::atomic<bool> busy{false};
  };

  static void Completed(void* context, AudioQueueRef, AudioQueueBufferRef buffer) {
    auto* self = static_cast<AudioOutput*>(context);
    auto* slot = static_cast<Slot*>(buffer->mUserData);
    slot->busy.store(false, std::memory_order_release);
    self->completed_.fetch_add(1, std::memory_order_release);
  }

  void StartIfReady() {
    if (!playing_ || started_) return;
    for (const auto& slot : slots_) {
      if (slot.busy.load(std::memory_order_acquire)) {
        started_ = AudioQueueStart(queue_, nullptr) == noErr;
        return;
      }
    }
  }

  audio::PcmFormat format_;
  AudioQueueRef queue_ = nullptr;
  std::array<Slot, 32> slots_{};
  std::atomic<unsigned> completed_{0};
  bool playing_ = false;
  bool started_ = false;
};

}  // namespace

std::unique_ptr<audio::PcmOutput> OpenPcmOutput(audio::PcmFormat format,
                                                const std::function<void(const std::string&)>& log) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault
                       options:AVAudioSessionCategoryOptionMixWithOthers error:&error] ||
        ![session setActive:YES error:&error]) {
      if (log) log("audio: could not activate iOS playback session (" + std::to_string(error.code) + ")");
      return nullptr;
    }
  }
  auto output = std::make_unique<AudioOutput>(format);
  const auto status = output->Open();
  if (status != noErr) {
    if (log) log("audio: could not open iOS AudioQueue (" + std::to_string(status) + ")");
    return nullptr;
  }
  if (log) log("audio: iOS AudioQueue opened at " + std::to_string(format.sample_rate) + " Hz, " +
               std::to_string(format.channels) + " channel(s), " + std::to_string(format.bits) + "-bit PCM");
  return output;
}

}  // namespace rb::platform::ios
