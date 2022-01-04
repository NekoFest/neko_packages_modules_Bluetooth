/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com. Represented by EHIMA -
 * www.ehima.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BTAudioClientLeAudio"

#include "le_audio_software.h"

#include <unordered_map>
#include <vector>

#include "bta/le_audio/codec_manager.h"
#include "client_interface.h"
#include "codec_status.h"
#include "hal_version_manager.h"
#include "osi/include/log.h"
#include "osi/include/properties.h"

namespace {

using ::android::hardware::bluetooth::audio::V2_0::BitsPerSample;
using ::android::hardware::bluetooth::audio::V2_0::ChannelMode;
using ::android::hardware::bluetooth::audio::V2_1::CodecType;
using ::android::hardware::bluetooth::audio::V2_1::Lc3FrameDuration;
using ::android::hardware::bluetooth::audio::V2_1::Lc3Parameters;
using ::android::hardware::bluetooth::audio::V2_1::PcmParameters;
using ::android::hardware::bluetooth::audio::V2_2::AudioLocation;
using ::android::hardware::bluetooth::audio::V2_2::LeAudioConfiguration;
using ::bluetooth::audio::AudioConfiguration_2_2;
using ::bluetooth::audio::BluetoothAudioCtrlAck;
using ::bluetooth::audio::SampleRate_2_1;
using ::bluetooth::audio::SessionType;
using ::bluetooth::audio::SessionType_2_1;
using ::bluetooth::audio::le_audio::LeAudioClientInterface;
using ::bluetooth::audio::le_audio::StreamCallbacks;
using AudioCapabilities_2_2 =
    ::android::hardware::bluetooth::audio::V2_2::AudioCapabilities;
using android::hardware::bluetooth::audio::V2_2::LeAudioCodecCapability;

using ::le_audio::CodecManager;
using ::le_audio::set_configurations::AudioSetConfiguration;
using ::le_audio::set_configurations::CodecCapabilitySetting;
using ::le_audio::set_configurations::SetConfiguration;
using ::le_audio::types::CodecLocation;
using ::le_audio::types::LeAudioLc3Config;

bluetooth::audio::BluetoothAudioSinkClientInterface*
    le_audio_sink_hal_clientinterface = nullptr;
bluetooth::audio::BluetoothAudioSourceClientInterface*
    le_audio_source_hal_clientinterface = nullptr;

static bool is_source_hal_enabled() {
  return le_audio_source_hal_clientinterface != nullptr;
}

static bool is_sink_hal_enabled() {
  return le_audio_sink_hal_clientinterface != nullptr;
}

class LeAudioTransport {
 public:
  LeAudioTransport(void (*flush)(void), StreamCallbacks stream_cb,
                   PcmParameters pcm_config)
      : flush_(std::move(flush)),
        stream_cb_(std::move(stream_cb)),
        remote_delay_report_ms_(0),
        total_bytes_processed_(0),
        data_position_({}),
        pcm_config_(std::move(pcm_config)),
        is_pending_start_request_(false){};

  BluetoothAudioCtrlAck StartRequest() {
    LOG(INFO) << __func__;

    if (stream_cb_.on_resume_(true)) {
      is_pending_start_request_ = true;
      return BluetoothAudioCtrlAck::PENDING;
    }

    return BluetoothAudioCtrlAck::FAILURE;
  }

  BluetoothAudioCtrlAck SuspendRequest() {
    LOG(INFO) << __func__;
    if (stream_cb_.on_suspend_()) {
      flush_();
      return BluetoothAudioCtrlAck::SUCCESS_FINISHED;
    } else {
      return BluetoothAudioCtrlAck::FAILURE;
    }
  }

  void StopRequest() {
    LOG(INFO) << __func__;
    if (stream_cb_.on_suspend_()) {
      flush_();
    }
  }

  bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                               uint64_t* total_bytes_processed,
                               timespec* data_position) {
    VLOG(2) << __func__ << ": data=" << total_bytes_processed_
            << " byte(s), timestamp=" << data_position_.tv_sec << "."
            << data_position_.tv_nsec
            << "s, delay report=" << remote_delay_report_ms_ << " msec.";
    if (remote_delay_report_ns != nullptr) {
      *remote_delay_report_ns = remote_delay_report_ms_ * 1000000u;
    }
    if (total_bytes_processed != nullptr)
      *total_bytes_processed = total_bytes_processed_;
    if (data_position != nullptr) *data_position = data_position_;

    return true;
  }

  void MetadataChanged(const source_metadata_t& source_metadata) {
    auto track_count = source_metadata.track_count;

    if (track_count == 0) {
      LOG(WARNING) << ", invalid number of metadata changed tracks";
      return;
    }

    stream_cb_.on_metadata_update_(source_metadata);
  }

  void SinkMetadataChanged(const sink_metadata_t& sink_metadata) {
    auto track_count = sink_metadata.track_count;

    if (track_count == 0) {
      LOG(WARNING) << ", invalid number of metadata changed tracks";
      return;
    }

    if (stream_cb_.on_sink_metadata_update_)
      stream_cb_.on_sink_metadata_update_(sink_metadata);
  }

  void ResetPresentationPosition() {
    VLOG(2) << __func__ << ": called.";
    remote_delay_report_ms_ = 0;
    total_bytes_processed_ = 0;
    data_position_ = {};
  }

  void LogBytesProcessed(size_t bytes_processed) {
    if (bytes_processed) {
      total_bytes_processed_ += bytes_processed;
      clock_gettime(CLOCK_MONOTONIC, &data_position_);
    }
  }

  void SetRemoteDelay(uint16_t delay_report_ms) {
    LOG(INFO) << __func__ << ": delay_report=" << delay_report_ms << " msec";
    remote_delay_report_ms_ = delay_report_ms;
  }

  const PcmParameters& LeAudioGetSelectedHalPcmConfig() { return pcm_config_; }

  void LeAudioSetSelectedHalPcmConfig(SampleRate_2_1 sample_rate,
                                      BitsPerSample bit_rate,
                                      ChannelMode channel_mode,
                                      uint32_t data_interval) {
    pcm_config_.sampleRate = sample_rate;
    pcm_config_.bitsPerSample = bit_rate;
    pcm_config_.channelMode = channel_mode;
    pcm_config_.dataIntervalUs = data_interval;
  }

  bool IsPendingStartStream(void) { return is_pending_start_request_; }
  void ClearPendingStartStream(void) { is_pending_start_request_ = false; }

 private:
  void (*flush_)(void);
  StreamCallbacks stream_cb_;
  uint16_t remote_delay_report_ms_;
  uint64_t total_bytes_processed_;
  timespec data_position_;
  PcmParameters pcm_config_;
  bool is_pending_start_request_;
};

static void flush_sink() {
  if (!is_sink_hal_enabled()) return;

  le_audio_sink_hal_clientinterface->FlushAudioData();
}

// Sink transport implementation for Le Audio
class LeAudioSinkTransport
    : public bluetooth::audio::IBluetoothSinkTransportInstance {
 public:
  LeAudioSinkTransport(SessionType_2_1 session_type, StreamCallbacks stream_cb)
      : IBluetoothSinkTransportInstance(session_type,
                                        (AudioConfiguration_2_2){}) {
    transport_ =
        new LeAudioTransport(flush_sink, std::move(stream_cb),
                             {SampleRate_2_1::RATE_16000, ChannelMode::STEREO,
                              BitsPerSample::BITS_16, 0});
  };

  ~LeAudioSinkTransport() { delete transport_; }

  BluetoothAudioCtrlAck StartRequest() override {
    return transport_->StartRequest();
  }

  BluetoothAudioCtrlAck SuspendRequest() override {
    return transport_->SuspendRequest();
  }

  void StopRequest() override { transport_->StopRequest(); }

  bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                               uint64_t* total_bytes_read,
                               timespec* data_position) override {
    return transport_->GetPresentationPosition(remote_delay_report_ns,
                                               total_bytes_read, data_position);
  }

  void MetadataChanged(const source_metadata_t& source_metadata) override {
    transport_->MetadataChanged(source_metadata);
  }

  void SinkMetadataChanged(const sink_metadata_t& sink_metadata) override {
    transport_->SinkMetadataChanged(sink_metadata);
  }

  void ResetPresentationPosition() override {
    transport_->ResetPresentationPosition();
  }

  void LogBytesRead(size_t bytes_read) override {
    transport_->LogBytesProcessed(bytes_read);
  }

  void SetRemoteDelay(uint16_t delay_report_ms) {
    transport_->SetRemoteDelay(delay_report_ms);
  }

  const PcmParameters& LeAudioGetSelectedHalPcmConfig() {
    return transport_->LeAudioGetSelectedHalPcmConfig();
  }

  void LeAudioSetSelectedHalPcmConfig(SampleRate_2_1 sample_rate,
                                      BitsPerSample bit_rate,
                                      ChannelMode channel_mode,
                                      uint32_t data_interval) {
    transport_->LeAudioSetSelectedHalPcmConfig(sample_rate, bit_rate,
                                               channel_mode, data_interval);
  }

  bool IsPendingStartStream(void) { return transport_->IsPendingStartStream(); }
  void ClearPendingStartStream(void) { transport_->ClearPendingStartStream(); }

 private:
  LeAudioTransport* transport_;
};

static void flush_source() {
  if (le_audio_source_hal_clientinterface == nullptr) return;

  le_audio_source_hal_clientinterface->FlushAudioData();
}

class LeAudioSourceTransport
    : public bluetooth::audio::IBluetoothSourceTransportInstance {
 public:
  LeAudioSourceTransport(StreamCallbacks stream_cb)
      : IBluetoothSourceTransportInstance(
            SessionType_2_1::LE_AUDIO_SOFTWARE_DECODED_DATAPATH,
            (AudioConfiguration_2_2){}) {
    transport_ =
        new LeAudioTransport(flush_source, std::move(stream_cb),
                             {SampleRate_2_1::RATE_16000, ChannelMode::MONO,
                              BitsPerSample::BITS_16, 0});
  };

  ~LeAudioSourceTransport() { delete transport_; }

  BluetoothAudioCtrlAck StartRequest() override {
    return transport_->StartRequest();
  }

  BluetoothAudioCtrlAck SuspendRequest() override {
    return transport_->SuspendRequest();
  }

  void StopRequest() override { transport_->StopRequest(); }

  bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                               uint64_t* total_bytes_written,
                               timespec* data_position) override {
    return transport_->GetPresentationPosition(
        remote_delay_report_ns, total_bytes_written, data_position);
  }

  void MetadataChanged(const source_metadata_t& source_metadata) override {
    transport_->MetadataChanged(source_metadata);
  }

  void SinkMetadataChanged(const sink_metadata_t& sink_metadata) override {
    transport_->SinkMetadataChanged(sink_metadata);
  }

  void ResetPresentationPosition() override {
    transport_->ResetPresentationPosition();
  }

  void LogBytesWritten(size_t bytes_written) override {
    transport_->LogBytesProcessed(bytes_written);
  }

  void SetRemoteDelay(uint16_t delay_report_ms) {
    transport_->SetRemoteDelay(delay_report_ms);
  }

  const PcmParameters& LeAudioGetSelectedHalPcmConfig() {
    return transport_->LeAudioGetSelectedHalPcmConfig();
  }

  void LeAudioSetSelectedHalPcmConfig(SampleRate_2_1 sample_rate,
                                      BitsPerSample bit_rate,
                                      ChannelMode channel_mode,
                                      uint32_t data_interval) {
    transport_->LeAudioSetSelectedHalPcmConfig(sample_rate, bit_rate,
                                               channel_mode, data_interval);
  }

  bool IsPendingStartStream(void) { return transport_->IsPendingStartStream(); }
  void ClearPendingStartStream(void) { transport_->ClearPendingStartStream(); }

 private:
  LeAudioTransport* transport_;
};

// Instance of Le Audio to provide call-in APIs for Bluetooth Audio Hal
LeAudioSinkTransport* le_audio_sink = nullptr;
LeAudioSourceTransport* le_audio_source = nullptr;
}  // namespace

namespace bluetooth {
namespace audio {
namespace le_audio {

std::unordered_map<SampleRate_2_1, uint8_t> sampling_freq_map{
    {SampleRate_2_1::RATE_8000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq8000Hz},
    {SampleRate_2_1::RATE_16000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq16000Hz},
    {SampleRate_2_1::RATE_24000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq24000Hz},
    {SampleRate_2_1::RATE_32000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq32000Hz},
    {SampleRate_2_1::RATE_44100,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq44100Hz},
    {SampleRate_2_1::RATE_48000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq48000Hz},
    {SampleRate_2_1::RATE_88200,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq88200Hz},
    {SampleRate_2_1::RATE_96000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq96000Hz},
    {SampleRate_2_1::RATE_176400,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq176400Hz},
    {SampleRate_2_1::RATE_192000,
     ::le_audio::codec_spec_conf::kLeAudioSamplingFreq192000Hz}};

std::unordered_map<Lc3FrameDuration, uint8_t> frame_duration_map{
    {Lc3FrameDuration::DURATION_7500US,
     ::le_audio::codec_spec_conf::kLeAudioCodecLC3FrameDur7500us},
    {Lc3FrameDuration::DURATION_10000US,
     ::le_audio::codec_spec_conf::kLeAudioCodecLC3FrameDur10000us}};

std::unordered_map<uint32_t, uint16_t> octets_per_frame_map{
    {30, ::le_audio::codec_spec_conf::kLeAudioCodecLC3FrameLen30},
    {40, ::le_audio::codec_spec_conf::kLeAudioCodecLC3FrameLen40},
    {120, ::le_audio::codec_spec_conf::kLeAudioCodecLC3FrameLen120}};

std::unordered_map<AudioLocation, uint32_t> audio_location_map{
    {AudioLocation::UNKNOWN,
     ::le_audio::codec_spec_conf::kLeAudioLocationMonoUnspecified},
    {AudioLocation::FRONT_LEFT,
     ::le_audio::codec_spec_conf::kLeAudioLocationFrontLeft},
    {AudioLocation::FRONT_RIGHT,
     ::le_audio::codec_spec_conf::kLeAudioLocationFrontRight},
    {static_cast<AudioLocation>(AudioLocation::FRONT_LEFT |
                                AudioLocation::FRONT_RIGHT),
     ::le_audio::codec_spec_conf::kLeAudioLocationFrontLeft |
         ::le_audio::codec_spec_conf::kLeAudioLocationFrontRight}};

bool halConfigToCodecCapabilitySetting(
    LeAudioCodecCapability halConfig, CodecCapabilitySetting& codecCapability) {
  if (halConfig.codecType != CodecType::LC3) {
    LOG(WARNING) << "Unsupported codecType: " << toString(halConfig.codecType);
    return false;
  }

  Lc3Parameters halLc3Config = halConfig.capabilities;
  AudioLocation supportedChannel = halConfig.supportedChannel;

  if (sampling_freq_map.find(halLc3Config.samplingFrequency) ==
          sampling_freq_map.end() ||
      frame_duration_map.find(halLc3Config.frameDuration) ==
          frame_duration_map.end() ||
      octets_per_frame_map.find(halLc3Config.octetsPerFrame) ==
          octets_per_frame_map.end() ||
      audio_location_map.find(supportedChannel) == audio_location_map.end()) {
    LOG(ERROR) << __func__ << ": Failed to convert HAL format to stack format"
               << "\nsample rate = " << (uint8_t)halLc3Config.samplingFrequency
               << "\nframe duration = " << (uint8_t)halLc3Config.frameDuration
               << "\noctets per frame= " << halLc3Config.octetsPerFrame
               << "\naudio location = " << (uint8_t)supportedChannel;

    return false;
  }

  codecCapability = {
      .id = ::le_audio::set_configurations::LeAudioCodecIdLc3,
      .config = LeAudioLc3Config(
          {.sampling_frequency =
               sampling_freq_map[halLc3Config.samplingFrequency],
           .frame_duration = frame_duration_map[halLc3Config.frameDuration],
           .octets_per_codec_frame =
               octets_per_frame_map[halLc3Config.octetsPerFrame],
           .audio_channel_allocation = audio_location_map[supportedChannel]})};

  return true;
}

std::vector<AudioSetConfiguration> get_offload_capabilities() {
  LOG(INFO) << __func__;
  std::vector<AudioSetConfiguration> offload_capabilities;
  std::vector<AudioCapabilities_2_2> le_audio_hal_capabilities =
      audio::BluetoothAudioSinkClientInterface::GetAudioCapabilities_2_2(
          SessionType_2_1::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  std::string strCapabilityLog;

  for (auto halCapability : le_audio_hal_capabilities) {
    CodecCapabilitySetting encodeCapability;
    CodecCapabilitySetting decodeCapability;
    LeAudioCodecCapability halEncodeConfig =
        halCapability.leAudioCapabilities().encodeCapability;
    LeAudioCodecCapability halDecodeConfig =
        halCapability.leAudioCapabilities().decodeCapability;
    AudioSetConfiguration audioSetConfig = {.name = "offload capability"};
    strCapabilityLog.clear();

    if (halConfigToCodecCapabilitySetting(halEncodeConfig, encodeCapability)) {
      audioSetConfig.confs.push_back(SetConfiguration(
          ::le_audio::types::kLeAudioDirectionSink, halEncodeConfig.deviceCount,
          halEncodeConfig.deviceCount * halEncodeConfig.channelCountPerDevice,
          encodeCapability));
      strCapabilityLog = " Encode Capability: " + toString(halEncodeConfig);
    }

    if (halConfigToCodecCapabilitySetting(halDecodeConfig, decodeCapability)) {
      audioSetConfig.confs.push_back(SetConfiguration(
          ::le_audio::types::kLeAudioDirectionSource,
          halDecodeConfig.deviceCount,
          halDecodeConfig.deviceCount * halDecodeConfig.channelCountPerDevice,
          decodeCapability));
      strCapabilityLog += " Decode Capability: " + toString(halDecodeConfig);
    }

    if (!audioSetConfig.confs.empty()) {
      offload_capabilities.push_back(audioSetConfig);
      LOG(INFO) << __func__
                << ": Supported codec capability =" << strCapabilityLog;

    } else {
      LOG(INFO) << __func__
                << ": Unknown codec capability =" << toString(halCapability);
    }
  }

  return offload_capabilities;
}

LeAudioClientInterface* LeAudioClientInterface::interface = nullptr;
LeAudioClientInterface* LeAudioClientInterface::Get() {
  if (osi_property_get_bool(BLUETOOTH_AUDIO_HAL_PROP_DISABLED, false)) {
    LOG(ERROR) << __func__ << ": BluetoothAudio HAL is disabled";
    return nullptr;
  }

  if (LeAudioClientInterface::interface == nullptr)
    LeAudioClientInterface::interface = new LeAudioClientInterface();

  return LeAudioClientInterface::interface;
}

static SampleRate_2_1 le_audio_sample_rate2audio_hal(uint32_t sample_rate_2_1) {
  switch (sample_rate_2_1) {
    case 8000:
      return SampleRate_2_1::RATE_8000;
    case 16000:
      return SampleRate_2_1::RATE_16000;
    case 24000:
      return SampleRate_2_1::RATE_24000;
    case 32000:
      return SampleRate_2_1::RATE_32000;
    case 44100:
      return SampleRate_2_1::RATE_44100;
    case 48000:
      return SampleRate_2_1::RATE_48000;
    case 88200:
      return SampleRate_2_1::RATE_88200;
    case 96000:
      return SampleRate_2_1::RATE_96000;
    case 176400:
      return SampleRate_2_1::RATE_176400;
    case 192000:
      return SampleRate_2_1::RATE_192000;
  };
  return SampleRate_2_1::RATE_UNKNOWN;
}

static BitsPerSample le_audio_bit_rate2audio_hal(uint8_t bits_per_sample) {
  switch (bits_per_sample) {
    case 16:
      return BitsPerSample::BITS_16;
    case 24:
      return BitsPerSample::BITS_24;
    case 32:
      return BitsPerSample::BITS_32;
  };
  return BitsPerSample::BITS_UNKNOWN;
}

static ChannelMode le_audio_channel_mode2audio_hal(uint8_t channels_count) {
  switch (channels_count) {
    case 1:
      return ChannelMode::MONO;
    case 2:
      return ChannelMode::STEREO;
  }
  return ChannelMode::UNKNOWN;
}

void LeAudioClientInterface::Sink::Cleanup() {
  LOG(INFO) << __func__ << " sink";
  StopSession();
  delete le_audio_sink_hal_clientinterface;
  le_audio_sink_hal_clientinterface = nullptr;
  delete le_audio_sink;
  le_audio_sink = nullptr;
}

void LeAudioClientInterface::Sink::SetPcmParameters(
    const PcmParameters& params) {
  le_audio_sink->LeAudioSetSelectedHalPcmConfig(
      le_audio_sample_rate2audio_hal(params.sample_rate),
      le_audio_bit_rate2audio_hal(params.bits_per_sample),
      le_audio_channel_mode2audio_hal(params.channels_count),
      params.data_interval_us);
}

// Update Le Audio delay report to BluetoothAudio HAL
void LeAudioClientInterface::Sink::SetRemoteDelay(uint16_t delay_report_ms) {
  LOG(INFO) << __func__ << ": delay_report_ms=" << delay_report_ms << " ms";
  le_audio_sink->SetRemoteDelay(delay_report_ms);
}

void LeAudioClientInterface::Sink::StartSession() {
  LOG(INFO) << __func__;
  if (HalVersionManager::GetHalVersion() ==
      BluetoothAudioHalVersion::VERSION_2_1) {
    AudioConfiguration_2_1 audio_config;
    audio_config.pcmConfig(le_audio_sink->LeAudioGetSelectedHalPcmConfig());
    if (!le_audio_sink_hal_clientinterface->UpdateAudioConfig_2_1(
            audio_config)) {
      LOG(ERROR) << __func__ << ": cannot update audio config to HAL";
      return;
    }
    le_audio_sink_hal_clientinterface->StartSession_2_1();
    return;
  }
  AudioConfiguration_2_2 audio_config;
  if (le_audio_sink_hal_clientinterface->GetTransportInstance()
          ->GetSessionType_2_1() ==
      SessionType_2_1::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH) {
    LeAudioConfiguration le_audio_config = {};
    audio_config.leAudioConfig(le_audio_config);
  } else {
    audio_config.pcmConfig(le_audio_sink->LeAudioGetSelectedHalPcmConfig());
  }
  if (!le_audio_sink_hal_clientinterface->UpdateAudioConfig_2_2(audio_config)) {
    LOG(ERROR) << __func__ << ": cannot update audio config to HAL";
    return;
  }
  le_audio_sink_hal_clientinterface->StartSession_2_2();
}

void LeAudioClientInterface::Sink::ConfirmStreamingRequest() {
  LOG(INFO) << __func__;
  if (!le_audio_sink->IsPendingStartStream()) {
    LOG(WARNING) << ", no pending start stream request";
    return;
  }

  le_audio_sink->ClearPendingStartStream();
  le_audio_sink_hal_clientinterface->StreamStarted(
      BluetoothAudioCtrlAck::SUCCESS_FINISHED);
}

void LeAudioClientInterface::Sink::CancelStreamingRequest() {
  LOG(INFO) << __func__;
  if (!le_audio_sink->IsPendingStartStream()) {
    LOG(WARNING) << ", no pending start stream request";
    return;
  }

  le_audio_sink->ClearPendingStartStream();
  le_audio_sink_hal_clientinterface->StreamStarted(
      BluetoothAudioCtrlAck::FAILURE);
}

void LeAudioClientInterface::Sink::StopSession() {
  LOG(INFO) << __func__ << " sink";
  le_audio_sink->ClearPendingStartStream();
  le_audio_sink_hal_clientinterface->EndSession();
}

size_t LeAudioClientInterface::Sink::Read(uint8_t* p_buf, uint32_t len) {
  return le_audio_sink_hal_clientinterface->ReadAudioData(p_buf, len);
}

void LeAudioClientInterface::Source::Cleanup() {
  LOG(INFO) << __func__ << " source";
  StopSession();
  delete le_audio_source_hal_clientinterface;
  le_audio_source_hal_clientinterface = nullptr;
  delete le_audio_source;
  le_audio_source = nullptr;
}

void LeAudioClientInterface::Source::SetPcmParameters(
    const PcmParameters& params) {
  le_audio_source->LeAudioSetSelectedHalPcmConfig(
      le_audio_sample_rate2audio_hal(params.sample_rate),
      le_audio_bit_rate2audio_hal(params.bits_per_sample),
      le_audio_channel_mode2audio_hal(params.channels_count),
      params.data_interval_us);
}

void LeAudioClientInterface::Source::SetRemoteDelay(uint16_t delay_report_ms) {
  LOG(INFO) << __func__ << ": delay_report_ms=" << delay_report_ms << " ms";
  le_audio_source->SetRemoteDelay(delay_report_ms);
}

void LeAudioClientInterface::Source::StartSession() {
  LOG(INFO) << __func__;
  if (!is_source_hal_enabled()) return;

  if (HalVersionManager::GetHalVersion() ==
      BluetoothAudioHalVersion::VERSION_2_1) {
    AudioConfiguration_2_1 audio_config;
    audio_config.pcmConfig(le_audio_source->LeAudioGetSelectedHalPcmConfig());
    if (!le_audio_source_hal_clientinterface->UpdateAudioConfig_2_1(
            audio_config)) {
      LOG(ERROR) << __func__ << ": cannot update audio config to HAL";
      return;
    }
    le_audio_source_hal_clientinterface->StartSession_2_1();
    return;
  }

  AudioConfiguration_2_2 audio_config;
  audio_config.pcmConfig(le_audio_source->LeAudioGetSelectedHalPcmConfig());
  if (!le_audio_source_hal_clientinterface->UpdateAudioConfig_2_2(
          audio_config)) {
    LOG(ERROR) << __func__ << ": cannot update audio config to HAL";
    return;
  }
  le_audio_source_hal_clientinterface->StartSession_2_2();
}

void LeAudioClientInterface::Source::ConfirmStreamingRequest() {
  LOG(INFO) << __func__;
  if (!le_audio_source->IsPendingStartStream()) {
    LOG(WARNING) << ", no pending start stream request";
    return;
  }

  le_audio_source->ClearPendingStartStream();
  le_audio_source_hal_clientinterface->StreamStarted(
      BluetoothAudioCtrlAck::SUCCESS_FINISHED);
}

void LeAudioClientInterface::Source::CancelStreamingRequest() {
  LOG(INFO) << __func__;
  if (!le_audio_source->IsPendingStartStream()) {
    LOG(WARNING) << ", no pending start stream request";
    return;
  }

  le_audio_source->ClearPendingStartStream();
  le_audio_source_hal_clientinterface->StreamStarted(
      BluetoothAudioCtrlAck::FAILURE);
}

void LeAudioClientInterface::Source::StopSession() {
  LOG(INFO) << __func__ << " source";
  le_audio_source->ClearPendingStartStream();
  le_audio_source_hal_clientinterface->EndSession();
}

size_t LeAudioClientInterface::Source::Write(const uint8_t* p_buf,
                                             uint32_t len) {
  return le_audio_source_hal_clientinterface->WriteAudioData(p_buf, len);
}

LeAudioClientInterface::Sink* LeAudioClientInterface::GetSink(
    StreamCallbacks stream_cb,
    bluetooth::common::MessageLoopThread* message_loop) {
  if (sink_ == nullptr) {
    sink_ = new Sink();
  } else {
    LOG(WARNING) << __func__ << ", Sink is already acquired";
    return nullptr;
  }

  LOG(INFO) << __func__;

  SessionType_2_1 session_type =
      SessionType_2_1::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH;
  if (CodecManager::GetInstance()->GetCodecLocation() != CodecLocation::HOST) {
    session_type = SessionType_2_1::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH;
  }

  le_audio_sink = new LeAudioSinkTransport(session_type, std::move(stream_cb));
  le_audio_sink_hal_clientinterface =
      new bluetooth::audio::BluetoothAudioSinkClientInterface(le_audio_sink,
                                                              message_loop);
  if (!le_audio_sink_hal_clientinterface->IsValid()) {
    LOG(WARNING) << __func__
                 << ": BluetoothAudio HAL for Le Audio is invalid?!";
    delete le_audio_sink_hal_clientinterface;
    le_audio_sink_hal_clientinterface = nullptr;
    delete le_audio_sink;
    le_audio_sink = nullptr;
    delete sink_;
    sink_ = nullptr;

    return nullptr;
  }

  return sink_;
}

bool LeAudioClientInterface::IsSinkAcquired() { return sink_ != nullptr; }

bool LeAudioClientInterface::ReleaseSink(LeAudioClientInterface::Sink* sink) {
  if (sink != sink_) {
    LOG(WARNING) << __func__ << ", can't release not acquired sink";
    return false;
  }

  if (le_audio_sink_hal_clientinterface && le_audio_sink) sink->Cleanup();

  delete (sink_);
  sink_ = nullptr;

  return true;
}

LeAudioClientInterface::Source* LeAudioClientInterface::GetSource(
    StreamCallbacks stream_cb,
    bluetooth::common::MessageLoopThread* message_loop) {
  if (source_ == nullptr) {
    source_ = new Source();
  } else {
    LOG(WARNING) << __func__ << ", Source is already acquired";
    return nullptr;
  }

  LOG(INFO) << __func__;

  le_audio_source = new LeAudioSourceTransport(std::move(stream_cb));
  le_audio_source_hal_clientinterface =
      new bluetooth::audio::BluetoothAudioSourceClientInterface(le_audio_source,
                                                                message_loop);
  if (!le_audio_source_hal_clientinterface->IsValid()) {
    LOG(WARNING) << __func__
                 << ": BluetoothAudio HAL for Le Audio is invalid?!";
    delete le_audio_source_hal_clientinterface;
    le_audio_source_hal_clientinterface = nullptr;
    delete le_audio_source;
    le_audio_source = nullptr;
    delete source_;
    source_ = nullptr;

    return nullptr;
  }

  return source_;
}

bool LeAudioClientInterface::IsSourceAcquired() { return source_ != nullptr; }

bool LeAudioClientInterface::ReleaseSource(
    LeAudioClientInterface::Source* source) {
  if (source != source_) {
    LOG(WARNING) << __func__ << ", can't release not acquired source";
    return false;
  }

  if (le_audio_source_hal_clientinterface && le_audio_source) source->Cleanup();

  delete (source_);
  source_ = nullptr;

  return true;
}

}  // namespace le_audio
}  // namespace audio
}  // namespace bluetooth
