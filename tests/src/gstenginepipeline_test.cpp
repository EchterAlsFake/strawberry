/*
 * Strawberry Music Player
 * Copyright 2026, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <QtGlobal>

#ifndef Q_OS_WIN32
#  include <unistd.h>
#  include <csignal>
#  include <thread>
#endif

#include "gtest_include.h"

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QDataStream>

#include "engine/gststartup.h"
#include "engine/gstenginepipeline.h"

#include "test_utils.h"

using namespace Qt::Literals::StringLiterals;

// Declared at file scope so that GstEnginePipeline's "friend class GstEnginePipelineTest;" resolves to this class.
class GstEnginePipelineTest {
 public:
  static bool IsNativeDsd(const GstEnginePipeline &pipeline) { return pipeline.native_dsd_; }

  static bool HasProcessing(const GstEnginePipeline &pipeline) {
    return pipeline.volume_ || pipeline.volume_sw_ || pipeline.volume_fading_ || pipeline.volume_ebur128_ || pipeline.audiopanorama_ || pipeline.equalizer_;
  }

  // Substitute a capture sink after construction, before opening any real audio device.
  static GstElement *CaptureOutput(GstEnginePipeline &pipeline, const char *caps_string) {
    GstElement *sink = gst_element_factory_make("appsink", nullptr);
    if (!sink) return nullptr;
    GstCaps *caps = gst_caps_from_string(caps_string);
    g_object_set(G_OBJECT(sink), "caps", caps, "sync", FALSE, "wait-on-eos", FALSE, nullptr);
    gst_caps_unref(caps);
    GstPad *old_pad = gst_element_get_static_pad(pipeline.audiosink_, "sink");
    GstPad *peer = gst_pad_get_peer(old_pad);
    gst_pad_unlink(peer, old_pad);
    gst_object_unref(old_pad);
    gst_bin_remove(GST_BIN(pipeline.audiobin_), pipeline.audiosink_);
    gst_bin_add(GST_BIN(pipeline.audiobin_), sink);
    GstPad *new_pad = gst_element_get_static_pad(sink, "sink");
    const GstPadLinkReturn result = gst_pad_link(peer, new_pad);
    gst_object_unref(new_pad);
    gst_object_unref(peer);
    pipeline.audiosink_ = sink;
    return result == GST_PAD_LINK_OK ? sink : nullptr;
  }

  // Waits until the GStreamer pipeline itself is in the state, without processing the bus messages, so the cached state in the GstEnginePipeline is not updated.
  static bool WaitForPipelineState(GstEnginePipeline &pipeline, const GstState state, const int timeout_msec = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_msec) {
      GstState current_state = GST_STATE_NULL;
      if (gst_element_get_state(pipeline.pipeline_, &current_state, nullptr, 100 * GST_MSECOND) == GST_STATE_CHANGE_SUCCESS && current_state == state) {
        return true;
      }
    }
    return false;
  }

  static void StartPlaybackAfterWarmup(GstEnginePipeline &pipeline) {
    pipeline.StartPlaybackAfterWarmup();
  }

  static GstState TargetState(const GstEnginePipeline &pipeline) {
    return pipeline.target_state_.load();
  }

  // Runs the current warm-up timer callback now.
  static void DeviceWarmupElapsed(GstEnginePipeline &pipeline) {
    pipeline.DeviceWarmupElapsed(pipeline.device_warmup_generation_.load());
  }

  // Sets whether the pipeline is buffering, like a buffering message from the queue does.
  static void SetBuffering(GstEnginePipeline &pipeline, const bool buffering) {
    pipeline.buffering_ = buffering;
  }
};

namespace {

// Processes Qt events, and the default GLib context, which receives the pipeline bus messages, Qt only processes it when it uses the GLib event dispatcher.
void ProcessEvents() {
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  while (g_main_context_iteration(nullptr, FALSE)) {}
}

// Processes events until the condition is true, returns false on timeout.
template <typename Condition>
bool WaitFor(Condition condition, const int timeout_msec = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeout_msec) return false;
    ProcessEvents();
  }
  return true;
}

// Processes events for the duration.
void ProcessEventsFor(const int duration_msec) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < duration_msec) {
    ProcessEvents();
  }
}

#ifdef HAVE_ALSA

// A short, deterministic stereo DSF with planar 4096-byte blocks.
bool WriteDsf(const QString &path, const int bits_per_sample, QByteArray &expected) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  constexpr int blocks = 3;
  constexpr int block_size = 4096;
  constexpr int bytes_per_channel = blocks * block_size;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.writeRawData("DSD ", 4);
  stream << quint64(28) << quint64(28 + 52 + 12 + 2 * bytes_per_channel) << quint64(0);
  stream.writeRawData("fmt ", 4);
  stream << quint64(52) << quint32(1) << quint32(0) << quint32(2) << quint32(2)
         << quint32(2822400) << quint32(bits_per_sample) << quint64(bytes_per_channel * 8) << quint32(block_size) << quint32(0);
  stream.writeRawData("data", 4);
  stream << quint64(12 + 2 * bytes_per_channel);

  auto source_byte = [](const int channel, const int index) { return static_cast<unsigned char>((index * 29 + channel * 103) & 255); };
  auto reverse_bits = [](unsigned char value) {
    unsigned char result = 0;
    for (int bit = 0; bit < 8; ++bit) {
      result = static_cast<unsigned char>((result << 1) | (value & 1));
      value >>= 1;
    }
    return result;
  };
  for (int block = 0; block < blocks; ++block) {
    for (int channel = 0; channel < 2; ++channel) {
      for (int i = 0; i < block_size; ++i) stream << quint8(source_byte(channel, block * block_size + i));
    }
  }
  // DSD_U32_BE packs four consecutive time-ordered bytes of each channel into each word.
  for (int i = 0; i < bytes_per_channel; i += 4) {
    for (int channel = 0; channel < 2; ++channel) {
      for (int byte = 0; byte < 4; ++byte) {
        const unsigned char value = source_byte(channel, i + byte);
        expected.append(static_cast<char>(bits_per_sample == 1 ? reverse_bits(value) : value));
      }
    }
  }
  return stream.status() == QDataStream::Ok;
}

QByteArray CapturedAudio(GstElement *sink) {
  QByteArray result;
  while (GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      result.append(reinterpret_cast<const char*>(map.data), static_cast<qsizetype>(map.size));
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
  }
  return result;
}

TEST(GstEnginePipelineTest, NativeDsfPreservesEveryBitWithBothPlaybins) {

  GstStartup::Initialize();
  if (!gst_registry_check_feature_version(gst_registry_get(), "dsdconvert", 1, 24, 0) ||
      !gst_registry_check_feature_version(gst_registry_get(), "alsasink", 1, 24, 0) ||
      !gst_registry_check_feature_version(gst_registry_get(), "avdemux_dsf", 1, 24, 0)) {
    GTEST_SKIP() << "GStreamer native DSD plugins are not installed";
  }

  for (const bool playbin3 : {false, true}) {
    for (const int bits_per_sample : {1, 8}) {
      SCOPED_TRACE(::testing::Message() << "playbin3=" << playbin3 << " DSF bit order=" << bits_per_sample);
      QTemporaryDir directory;
      const QUrl url = QUrl::fromLocalFile(directory.filePath(u"native.DSF"_s));
      QByteArray expected;
      ASSERT_TRUE(WriteDsf(url.toLocalFile(), bits_per_sample, expected));
      GstEnginePipeline pipeline;
      pipeline.set_output_device(u"alsasink"_s, u"hw:CARD=Pro,DEV=0"_s);
      pipeline.set_playbin3_enabled(playbin3);
      pipeline.set_device_warmup_duration_ms(0);
      // All processing options must be bypassed for native DSD.
      pipeline.set_fading_enabled(true);
      pipeline.set_equalizer_enabled(true);
      pipeline.set_stereo_balancer_enabled(true);
      pipeline.set_replaygain(true, 0, 6.0, 6.0, true);
      pipeline.set_ebur128_loudness_normalization(true);
      pipeline.set_channels(true, 1);
      pipeline.set_bs2b_enabled(true);
      QString error;
      ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();
      ASSERT_TRUE(GstEnginePipelineTest::IsNativeDsd(pipeline));
      EXPECT_TRUE(pipeline.exclusive_mode());
      EXPECT_FALSE(GstEnginePipelineTest::HasProcessing(pipeline));
      GstElement *sink = GstEnginePipelineTest::CaptureOutput(pipeline, "audio/x-dsd,format=DSDU32BE,layout=interleaved,reversed-bytes=false,rate=352800,channels=2");
      ASSERT_NE(nullptr, sink);
      bool ended = false;
      QObject::connect(&pipeline, &GstEnginePipeline::EndOfStreamReached, &pipeline, [&ended]() { ended = true; });
      QObject::connect(&pipeline, &GstEnginePipeline::Error, &pipeline, [&error](int, int, int, const QString &message, const QString &) { error = message; });
      pipeline.SetVolume(1);
      pipeline.SetMute(true);
      pipeline.Play(false, 0);
      ASSERT_TRUE(WaitFor([&]() { return ended || !error.isEmpty(); })) << "Timed out playing DSF";
      ASSERT_TRUE(error.isEmpty()) << error.toStdString();
      const QByteArray actual = CapturedAudio(sink);
      ASSERT_EQ(expected.size(), actual.size());
      EXPECT_TRUE(expected == actual) << "Native DSD bits changed";
    }
  }

}

TEST(GstEnginePipelineTest, NativeDsfRequiresDirectAlsaAndCanBeDisabled) {

  GstStartup::Initialize();
  const QUrl url = QUrl::fromLocalFile(u"/tmp/test.dsf"_s);
  for (const QString &output : {u"fakesink"_s, u"alsasink"_s}) {
    GstEnginePipeline pipeline;
    pipeline.set_output_device(output, output == u"alsasink"_s ? QVariant(u"plughw:0,0"_s) : QVariant());
    QString error;
    ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();
    EXPECT_FALSE(GstEnginePipelineTest::IsNativeDsd(pipeline));
  }
  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"alsasink"_s, u"hw:Pro,0"_s);
  pipeline.set_native_dsd_enabled(false);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error));
  EXPECT_FALSE(GstEnginePipelineTest::IsNativeDsd(pipeline));

}

TEST(GstEnginePipelineTest, PcmDsdBoundariesDoNotReuseTheAudioBin) {

  GstStartup::Initialize();
  if (!gst_registry_check_feature_version(gst_registry_get(), "dsdconvert", 1, 24, 0) ||
      !gst_registry_check_feature_version(gst_registry_get(), "alsasink", 1, 24, 0)) {
    GTEST_SKIP() << "GStreamer native DSD plugins are not installed";
  }
  const QUrl pcm = QUrl::fromLocalFile(u"/tmp/test.flac"_s);
  const QUrl dsd = QUrl::fromLocalFile(u"/tmp/test.dsf"_s);
  for (const bool native : {false, true}) {
    const QUrl current = native ? dsd : pcm;
    const QUrl next = native ? pcm : dsd;
    GstEnginePipeline pipeline;
    pipeline.set_output_device(u"alsasink"_s, u"hw:Pro,0"_s);
    QString error;
    ASSERT_TRUE(pipeline.InitFromUrl(current, current, current.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();
    pipeline.PrepareNextUrl(next, next, next.toEncoded(), 0, 0);
    EXPECT_FALSE(pipeline.HasNextUrl());
    pipeline.PrepareNextUrl(current, current, current.toEncoded(), 0, 0);
    EXPECT_TRUE(pipeline.HasNextUrl());
    pipeline.PrepareNextUrl(next, next, next.toEncoded(), 0, 0);
    EXPECT_FALSE(pipeline.HasNextUrl());
  }

}

TEST(GstEnginePipelineTest, BitPerfectOutputRejectsMixingDevices) {

  GstStartup::Initialize();
  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_bit_perfect(true);
  const QUrl url = QUrl::fromLocalFile(u"/tmp/test.wav"_s);
  QString error;
  EXPECT_FALSE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error));
  EXPECT_TRUE(error.contains("hw:"_L1));

}

TEST(GstEnginePipelineTest, BitPerfectPcmPreservesRateChannelsAndSamples) {

  GstStartup::Initialize();
  for (const int bits : {16, 24}) {
    for (const bool playbin3 : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "PCM bits=" << bits << " playbin3=" << playbin3);
      QTemporaryDir directory;
      const QUrl url = QUrl::fromLocalFile(directory.filePath(u"pcm.wav"_s));
      QFile file(url.toLocalFile());
      ASSERT_TRUE(file.open(QIODevice::WriteOnly));
      constexpr int frames = 4096;
      constexpr int channels = 2;
      const int data_size = frames * channels * bits / 8;
      QDataStream stream(&file);
      stream.setByteOrder(QDataStream::LittleEndian);
      stream.writeRawData("RIFF", 4);
      stream << quint32(36 + data_size);
      stream.writeRawData("WAVEfmt ", 8);
      stream << quint32(16) << quint16(1) << quint16(channels) << quint32(96000)
             << quint32(96000 * channels * bits / 8) << quint16(channels * bits / 8) << quint16(bits);
      stream.writeRawData("data", 4);
      stream << quint32(data_size);
      QByteArray expected;
      QDataStream expected_stream(&expected, QIODevice::WriteOnly);
      expected_stream.setByteOrder(QDataStream::LittleEndian);
      for (int i = 0; i < frames * channels; ++i) {
        const quint32 sample = static_cast<quint32>(i) * 4799U;
        for (int byte = 0; byte < bits / 8; ++byte) stream << quint8((sample >> (8 * byte)) & 255U);
        expected_stream << quint32(sample << (32 - bits));
      }
      file.close();

      GstEnginePipeline pipeline;
      pipeline.set_output_device(u"alsasink"_s, u"hw:Pro,0"_s);
      pipeline.set_bit_perfect(true);
      pipeline.set_playbin3_enabled(playbin3);
      pipeline.set_device_warmup_duration_ms(0);
      pipeline.set_equalizer_enabled(true);
      pipeline.set_stereo_balancer_enabled(true);
      pipeline.set_fading_enabled(true);
      pipeline.set_replaygain(true, 0, 6.0, 6.0, true);
      pipeline.set_ebur128_loudness_normalization(true);
      pipeline.set_channels(true, 1);
      pipeline.set_bs2b_enabled(true);
      QString error;
      ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 12.0, error)) << error.toStdString();
      EXPECT_FALSE(GstEnginePipelineTest::IsNativeDsd(pipeline));
      EXPECT_FALSE(GstEnginePipelineTest::HasProcessing(pipeline));
      EXPECT_TRUE(pipeline.exclusive_mode());
      GstElement *sink = GstEnginePipelineTest::CaptureOutput(pipeline, "audio/x-raw,format=S32LE,rate=96000,channels=2,layout=interleaved");
      ASSERT_NE(nullptr, sink);
      bool ended = false;
      QObject::connect(&pipeline, &GstEnginePipeline::EndOfStreamReached, &pipeline, [&ended]() { ended = true; });
      QObject::connect(&pipeline, &GstEnginePipeline::Error, &pipeline, [&error](int, int, int, const QString &message, const QString &) { error = message; });
      pipeline.SetVolume(1);
      pipeline.SetMute(true);
      pipeline.Play(false, 0);
      ASSERT_TRUE(WaitFor([&]() { return ended || !error.isEmpty(); }));
      ASSERT_TRUE(error.isEmpty()) << error.toStdString();
      const QByteArray actual = CapturedAudio(sink);
      ASSERT_EQ(expected.size(), actual.size());
      EXPECT_TRUE(actual == expected) << "PCM samples changed during integer widening";
    }
  }

}

TEST(GstEnginePipelineTest, BitPerfectPcmRejectsResamplingMixingAndPrecisionLoss) {

  GstStartup::Initialize();
  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());
  for (const char *caps : {"audio/x-raw,format=S8", "audio/x-raw,rate=12345", "audio/x-raw,channels=7"}) {
    SCOPED_TRACE(caps);
    GstEnginePipeline pipeline;
    pipeline.set_output_device(u"alsasink"_s, u"hw:Pro,0"_s);
    pipeline.set_bit_perfect(true);
    pipeline.set_device_warmup_duration_ms(0);
    QString error;
    ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();
    ASSERT_NE(nullptr, GstEnginePipelineTest::CaptureOutput(pipeline, caps));
    QObject::connect(&pipeline, &GstEnginePipeline::Error, &pipeline, [&error](int, int, int, const QString &message, const QString &) { error = message; });
    pipeline.Play(false, 0);
    EXPECT_TRUE(WaitFor([&]() { return !error.isEmpty(); })) << "Output silently changed the PCM format";
    EXPECT_TRUE(error.contains("Bit-perfect"_L1)) << error.toStdString();
  }

}

TEST(GstEnginePipelineTest, DisablingNativeDsdStillPlaysDsfAsPcm) {

  GstStartup::Initialize();
  QTemporaryDir directory;
  const QUrl url = QUrl::fromLocalFile(directory.filePath(u"fallback.dsf"_s));
  QByteArray expected;
  ASSERT_TRUE(WriteDsf(url.toLocalFile(), 1, expected));
  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"alsasink"_s, u"hw:Pro,0"_s);
  pipeline.set_native_dsd_enabled(false);
  pipeline.set_device_warmup_duration_ms(0);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();
  EXPECT_FALSE(GstEnginePipelineTest::IsNativeDsd(pipeline));
  GstElement *sink = GstEnginePipelineTest::CaptureOutput(pipeline, "audio/x-raw");
  ASSERT_NE(nullptr, sink);
  bool ended = false;
  QObject::connect(&pipeline, &GstEnginePipeline::EndOfStreamReached, &pipeline, [&ended]() { ended = true; });
  QObject::connect(&pipeline, &GstEnginePipeline::Error, &pipeline, [&error](int, int, int, const QString &message, const QString &) { error = message; });
  pipeline.Play(false, 0);
  ASSERT_TRUE(WaitFor([&]() { return ended || !error.isEmpty(); }));
  ASSERT_TRUE(error.isEmpty()) << error.toStdString();
  EXPECT_FALSE(CapturedAudio(sink).isEmpty());

}

#endif  // HAVE_ALSA

// Regression test: pausing while playback was waiting for the audio device warm-up was ignored, and the warm-up timer then started playback anyway.
TEST(GstEnginePipelineTest, PauseDuringDeviceWarmupHoldsPlayback) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(500);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  pipeline.Play(false, 0);
  EXPECT_TRUE(pipeline.play_pending());

  // Pause while the pipeline is prerolling or waiting for the warm-up.
  ASSERT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PAUSED; }));
  ASSERT_NE(GST_STATE_PLAYING, pipeline.state());
  pipeline.PauseBeforePlaying();
  EXPECT_TRUE(pipeline.paused_before_playing());

  // The warm-up has elapsed, but playback is held.
  ProcessEventsFor(1000);
  EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());

  // Unpausing starts playback.
  pipeline.UnpauseBeforePlaying();
  EXPECT_FALSE(pipeline.paused_before_playing());
  EXPECT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PLAYING; }));

}

// Regression test: without a warm-up, playback goes straight to PLAYING, and pausing before the pipeline was PLAYING was ignored.
TEST(GstEnginePipelineTest, PauseDuringDirectStartHoldsPlayback) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(0);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Pause while going to PLAYING is in progress.
  pipeline.Play(false, 0);
  ASSERT_NE(GST_STATE_PLAYING, pipeline.state());
  EXPECT_TRUE(pipeline.play_pending());
  pipeline.PauseBeforePlaying();

  ProcessEventsFor(1000);
  EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());

  // Unpausing starts playback.
  pipeline.UnpauseBeforePlaying();
  EXPECT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PLAYING; }));

}

// Regression test: playback was not pending anymore after the warm-up delay elapsed, before the pipeline was PLAYING, so pausing then was ignored.
TEST(GstEnginePipelineTest, PlayPendingUntilPlaying) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  for (const int device_warmup_duration_ms : {0, 300}) {
    GstEnginePipeline pipeline;
    pipeline.set_output_device(u"fakesink"_s, QVariant());
    pipeline.set_device_warmup_duration_ms(device_warmup_duration_ms);
    QString error;
    ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

    pipeline.Play(false, 0);
    bool always_pending = true;
    EXPECT_TRUE(WaitFor([&pipeline, &always_pending]() {
      if (pipeline.state() == GST_STATE_PLAYING) return true;
      // Pausing is not possible while buffering, so playback is not pending then.
      if (!pipeline.play_pending() && !pipeline.is_buffering()) always_pending = false;
      return false;
    }));
    EXPECT_TRUE(always_pending) << "Warm-up " << device_warmup_duration_ms << " ms";
  }

}

// Regression test: when the pipeline was PLAYING before the pause requested in PauseBeforePlaying() was applied, unpausing before the pause was applied did nothing, since the state was still PLAYING, so playback stayed paused.
TEST(GstEnginePipelineTest, UnpauseBeforePauseIsAppliedResumesPlayback) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(0);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Let the pipeline reach PLAYING without processing the state messages, then pause while going to PLAYING is still in progress for the pipeline.
  pipeline.Play(false, 0);
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PLAYING));
  ASSERT_NE(GST_STATE_PLAYING, pipeline.state());
  ASSERT_TRUE(pipeline.play_pending());
  pipeline.PauseBeforePlaying();
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PAUSED));

  // Process the bus messages one by one until the PLAYING state message is processed, the PAUSED state message is processed after it.
  // The test drives the GLib default context on the pipeline's thread, so the bus messages are handled directly on all platforms, they are only posted as Qt events when a separate GLib thread drives the context.
  QElapsedTimer timer;
  timer.start();
  while (pipeline.state() != GST_STATE_PLAYING && timer.elapsed() < 10000) {
    g_main_context_iteration(nullptr, FALSE);
  }
  ASSERT_EQ(GST_STATE_PLAYING, pipeline.state());
  // Still paused, so the engine reports it as paused, and unpauses it.
  EXPECT_TRUE(pipeline.paused_before_playing());

  // Unpause before the PAUSED state message is processed.
  pipeline.UnpauseBeforePlaying();

  // The queued PAUSED state message from the pause is processed before the PLAYING state message from unpausing, playback is still pending then, so pausing is not missed.
  timer.restart();
  while (pipeline.state() != GST_STATE_PAUSED && timer.elapsed() < 10000) {
    g_main_context_iteration(nullptr, FALSE);
  }
  ASSERT_EQ(GST_STATE_PAUSED, pipeline.state());
  EXPECT_TRUE(pipeline.play_pending());

  // The pause is applied, then playback is resumed, and playback is pending until PLAYING is received, so pausing in between is not missed.
  bool always_pending = true;
  EXPECT_TRUE(WaitFor([&pipeline, &always_pending]() {
    if (pipeline.state() == GST_STATE_PLAYING) return true;
    if (!pipeline.play_pending() && !pipeline.is_buffering()) always_pending = false;
    return false;
  }));
  EXPECT_TRUE(always_pending);
  ProcessEventsFor(500);
  EXPECT_EQ(GST_STATE_PLAYING, pipeline.state());

}

// Regression test: a request to go to PLAYING which was superseded by a request to go to PAUSED, like a pause while going back to PLAYING after buffering, stayed pending, so the paused pipeline was reported as playing.
TEST(GstEnginePipelineTest, SupersededPlayingRequestIsNotPending) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(0);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  pipeline.Play(false, 0);
  ASSERT_TRUE(pipeline.play_pending());
  pipeline.SetState(GST_STATE_PAUSED);

  ProcessEventsFor(1000);
  EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());
  EXPECT_FALSE(pipeline.play_pending());

}

// Regression test: the PLAYING state message from before pausing cleared the request to go to PLAYING when unpausing, so playback was not pending after the PAUSED state message from the pause, and pausing was missed.
TEST(GstEnginePipelineTest, StaleStateMessagesKeepPlaybackPending) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(0);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Let the pipeline reach PLAYING, then PAUSED, without processing the state messages, and unpause before any of them are processed.
  pipeline.Play(false, 0);
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PLAYING));
  pipeline.PauseBeforePlaying();
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PAUSED));
  pipeline.UnpauseBeforePlaying();
  EXPECT_TRUE(pipeline.play_pending());

  // Process the state messages one by one, playback is pending until the PLAYING state message from unpausing is processed.
  // Unpausing requests PLAYING with a queued call to the pipeline, which is only delivered by iterating the GLib context when Qt uses the GLib event dispatcher, so also deliver the queued calls to the pipeline, they don't process bus messages.
  bool seen_playing = false;
  bool seen_paused_after_playing = false;
  bool always_pending = true;
  QElapsedTimer timer;
  timer.start();
  while (!(seen_paused_after_playing && pipeline.state() == GST_STATE_PLAYING) && timer.elapsed() < 10000) {
    QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
    g_main_context_iteration(nullptr, FALSE);
    if (pipeline.state() == GST_STATE_PLAYING) {
      seen_playing = true;
    }
    else if (seen_playing && pipeline.state() == GST_STATE_PAUSED) {
      seen_paused_after_playing = true;
      if (!pipeline.play_pending() && !pipeline.is_buffering()) always_pending = false;
    }
  }
  EXPECT_TRUE(seen_paused_after_playing);
  EXPECT_TRUE(always_pending);
  EXPECT_EQ(GST_STATE_PLAYING, pipeline.state());

}

// Regression test: a state change cancelled the warm-up timer, but playback stayed pending until the timer elapsed, so the paused pipeline was reported as playing.
TEST(GstEnginePipelineTest, CancelledWarmupIsNotPending) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(2000);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Wait until the pipeline is prerolled and waiting for the warm-up.
  pipeline.Play(false, 0);
  ASSERT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PAUSED; }));
  ASSERT_TRUE(pipeline.play_pending());

  // Pause, like a normal pause, which cancels the warm-up, playback is not pending, also before the warm-up timer elapsed.
  pipeline.SetState(GST_STATE_PAUSED);
  EXPECT_FALSE(pipeline.play_pending());
  ProcessEventsFor(500);
  EXPECT_FALSE(pipeline.play_pending());
  EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());

  // Resuming starts a new warm-up, the old warm-up timer elapses before it, and does not clear it.
  GstEnginePipelineTest::StartPlaybackAfterWarmup(pipeline);
  EXPECT_TRUE(pipeline.play_pending());
  ProcessEventsFor(1700);
  EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());
  EXPECT_TRUE(pipeline.play_pending());
  EXPECT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PLAYING; }));

}

// Regression test: a buffering message from the queue while waiting for the warm-up made playback not pending, although the warm-up still starts playback, so pausing then was ignored, this made CancelledWarmupIsNotPending fail when the buffering finished after the preroll.
TEST(GstEnginePipelineTest, WarmupIsPendingWhileBuffering) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  pipeline.set_device_warmup_duration_ms(2000);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Wait until the pipeline is prerolled, waiting for the warm-up, and no longer buffering.
  pipeline.Play(false, 0);
  ASSERT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PAUSED && pipeline.play_pending(); }));

  // Buffering while waiting for the warm-up, playback is still pending, the events are not processed in between, so no buffering message changes it.
  GstEnginePipelineTest::SetBuffering(pipeline, true);
  EXPECT_TRUE(pipeline.play_pending());

  // The warm-up starts playback.
  GstEnginePipelineTest::SetBuffering(pipeline, false);
  GstEnginePipelineTest::DeviceWarmupElapsed(pipeline);
  EXPECT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PLAYING; }));

}

// Regression test: when unpausing quickly with a warm-up, before the pause was applied, the warm-up timer did not start playback when the PLAYING state message from before the pause was processed but not the PAUSED state message, so playback stayed paused.
TEST(GstEnginePipelineTest, WarmupAfterQuickUnpauseResumesPlayback) {

  GstStartup::Initialize();

  TemporaryResource res(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(res.open());
  const QUrl url = QUrl::fromLocalFile(res.fileName());

  GstEnginePipeline pipeline;
  pipeline.set_output_device(u"fakesink"_s, QVariant());
  // The warm-up timers don't elapse during the test, the test elapses the warm-up itself, so it controls the order of the state messages.
  pipeline.set_device_warmup_duration_ms(600000);
  QString error;
  ASSERT_TRUE(pipeline.InitFromUrl(url, url, url.toEncoded(), 0, 0, 0.0, error)) << error.toStdString();

  // Wait for the preroll, elapse the warm-up, which requests PLAYING, then apply the request without processing the bus messages, and let the pipeline reach PLAYING.
  pipeline.Play(false, 0);
  ASSERT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PAUSED && pipeline.play_pending(); }));
  GstEnginePipelineTest::DeviceWarmupElapsed(pipeline);
  ASSERT_EQ(GST_STATE_PLAYING, GstEnginePipelineTest::TargetState(pipeline));
  ASSERT_EQ(GST_STATE_PAUSED, pipeline.state());
  QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PLAYING));

  // Pause and unpause before the state messages are processed, unpausing starts a new warm-up.
  pipeline.PauseBeforePlaying();
  ASSERT_TRUE(GstEnginePipelineTest::WaitForPipelineState(pipeline, GST_STATE_PAUSED));
  ASSERT_EQ(GST_STATE_PAUSED, pipeline.state());
  pipeline.UnpauseBeforePlaying();
  EXPECT_TRUE(pipeline.play_pending());

  // Process the PLAYING state message from before the pause, but not the PAUSED state message, then the warm-up elapses.
  QElapsedTimer timer;
  timer.start();
  while (pipeline.state() != GST_STATE_PLAYING && timer.elapsed() < 10000) {
    g_main_context_iteration(nullptr, FALSE);
  }
  ASSERT_EQ(GST_STATE_PLAYING, pipeline.state());
  GstEnginePipelineTest::DeviceWarmupElapsed(pipeline);

  // The PAUSED state message is processed, playback is still pending then, and playback is resumed.
  // Process the bus messages one by one, and deliver the queued calls to the pipeline, which request PLAYING.
  bool seen_paused = false;
  bool always_pending = true;
  timer.restart();
  while (!(seen_paused && pipeline.state() == GST_STATE_PLAYING) && timer.elapsed() < 10000) {
    QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
    g_main_context_iteration(nullptr, FALSE);
    if (pipeline.state() == GST_STATE_PAUSED) {
      seen_paused = true;
      if (!pipeline.play_pending() && !pipeline.is_buffering()) always_pending = false;
    }
  }
  EXPECT_TRUE(seen_paused);
  EXPECT_TRUE(always_pending);
  EXPECT_EQ(GST_STATE_PLAYING, pipeline.state());

}

#ifndef Q_OS_WIN32
// Regression test: when the seek to the offset failed when starting playback, like for a stream which is not seekable, playback never started, and pausing and unpausing did not start it either.
TEST(GstEnginePipelineTest, FailedStartupSeekStartsPlayback) {

  GstStartup::Initialize();

  QFile wav_file(u":/audio/strawberry.wav"_s);
  ASSERT_TRUE(wav_file.open(QIODevice::ReadOnly));
  const QByteArray wav_data = wav_file.readAll();
  ASSERT_FALSE(wav_data.isEmpty());

  // A pipe is not seekable, so the seek to the offset fails.
  int pipe_fds[2] = { -1, -1 };
  ASSERT_EQ(0, pipe(pipe_fds));

  // Write the file to the pipe, writing fails when the pipe is closed for reading, SIGPIPE is ignored, so it fails with EPIPE instead.
  std::signal(SIGPIPE, SIG_IGN);
  std::thread writer([write_fd = pipe_fds[1], wav_data]() {
    qint64 written = 0;
    while (written < wav_data.size()) {
      const ssize_t result = write(write_fd, wav_data.constData() + written, static_cast<size_t>(wav_data.size() - written));
      if (result <= 0) break;
      written += result;
    }
    close(write_fd);
  });

  // The pipeline is tested in a lambda, so a fatal assertion only returns from the lambda, and the writer is still stopped and joined below.
  const auto test_playback = [&pipe_fds]() {
    const QByteArray gst_url = "fd://" + QByteArray::number(pipe_fds[0]);
    const QUrl url(QString::fromLatin1(gst_url));
    GstEnginePipeline pipeline;
    pipeline.set_output_device(u"fakesink"_s, QVariant());
    pipeline.set_device_warmup_duration_ms(0);
    QString error;
    ASSERT_TRUE(pipeline.InitFromUrl(url, url, gst_url, 0, 0, 0.0, error)) << error.toStdString();

    // Start playback from an offset, and pause while it is prerolling, the pause holds playback when the seek fails, and unpausing starts it.
    pipeline.Play(false, 1 * GST_SECOND);
    EXPECT_TRUE(pipeline.play_pending());
    pipeline.PauseBeforePlaying();
    ProcessEventsFor(1000);
    EXPECT_EQ(GST_STATE_PAUSED, pipeline.state());
    pipeline.UnpauseBeforePlaying();
    EXPECT_TRUE(WaitFor([&pipeline]() { return pipeline.state() == GST_STATE_PLAYING; }));
    EXPECT_FALSE(pipeline.play_pending());
  };
  test_playback();

  // Close the read end of the pipe, so the writer stops if it has not written everything.
  close(pipe_fds[0]);
  writer.join();

}
#endif

}  // namespace
