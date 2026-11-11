#include "config.hpp"
#include "pose.hpp"
#include "shm.hpp"
#include "stereo.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace xr;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
template <class T> void put(std::array<uint8_t, 186> &b, size_t at, T v) {
  memcpy(b.data() + at, &v, sizeof v);
}
void parity(std::array<uint8_t, 186> &b) {
  b[185] = 0;
  for (size_t i = 113; i < 185; ++i)
    b[185] ^= b[i];
}

void checkRotation(Quat actual, Quat expected, const char *message) {
  auto delta = expected.inverse() * actual;
  check(std::hypot(delta.x, delta.y, delta.z) < 1e-8, message);
}

void checkHeadTracking() {
  Pose p;
  p.valid = true;
  p.timestamp = 1000;
  HeadTracking tracking(250);
  using Change = HeadTracking::Change;
  check(tracking.update(p) == Change::acquired, "tracking acquired");

  p.timestamp += 16;
  p.orientation = Quat::axis({0, 1, 0}, rad(.2));
  tracking.update(p);
  const auto beforeTurn = tracking.rotation();
  const double filteredYaw = euler(beforeTurn)[0];
  check(filteredYaw > 0 && filteredYaw < .05,
        "filter attenuates small motion without a dead zone");
  checkRotation(tracking.orientation(), p.orientation, "raw diagnostic pose");

  Pose duplicate = p;
  duplicate.orientation = Quat::axis({1, 0, 0}, rad(60));
  tracking.update(duplicate);
  --duplicate.timestamp;
  tracking.update(duplicate);
  checkRotation(tracking.rotation(), beforeTurn,
                "duplicate or older pose ignored");

  for (int i = 1; i <= 12; ++i) {
    p.timestamp += 16;
    p.orientation = Quat::axis({0, 1, 0}, rad(2 * i));
    tracking.update(p);
  }
  check(euler(tracking.rotation())[0] > 20, "filter follows deliberate turn");
  const auto frozen = tracking.rotation();
  Pose missing;
  tracking.update(missing);
  checkRotation(tracking.rotation(), frozen,
                "tracking loss freezes projection");
  check(!tracking.recenter(missing), "stale pose cannot recenter");

  p.timestamp += 300;
  p.orientation = Quat::axis({1, 0, 0}, rad(45));
  check(tracking.update(p) == Change::resumed, "tracking resumed");
  checkRotation(tracking.rotation(), frozen,
                "recovery keeps displayed rotation");

  ++p.timestamp;
  ++p.referenceGeneration;
  p.orientation = Quat::axis({0, 0, 1}, rad(-80));
  check(tracking.update(p) == Change::referenceReset,
        "live tracker reset detected");
  checkRotation(tracking.rotation(), frozen, "reset keeps displayed rotation");
  check(tracking.recenter(p), "recenter from current raw pose");
  checkRotation(tracking.rotation(), Quat{}, "recenter snaps immediately");

  p.timestamp += 251;
  p.orientation = Quat::axis({0, 1, 0}, rad(179));
  check(tracking.update(p) == Change::resumed,
        "gap detected without an idle poll");
  checkRotation(tracking.rotation(), Quat{}, "gap keeps displayed rotation");
  p.timestamp += 16;
  p.orientation = Quat::axis({0, 1, 0}, rad(-179));
  tracking.update(p);
  check(euler(tracking.rotation())[0] > 0 && euler(tracking.rotation())[0] < 2,
        "filter crosses yaw wrap along the short arc");
}

void checkOpenTrackSequence() {
  Pose p;
  p.timestamp = 1000;
  OpenTrackSequence sequence(250);
  check(sequence.accept(400, 10000, p), "first frame accepted");
  check(!sequence.accept(400, 10000, p), "duplicate frame rejected");
  check(!sequence.accept(399, 10000, p), "out-of-order frame rejected");
  check(sequence.accept(3, 10000, p) && p.referenceGeneration == 1,
        "counter restart detected even with first packets missing");
  check(sequence.accept(4, 10000, p) && p.referenceGeneration == 1,
        "newest packet retains reset across a receive batch");
  check(sequence.accept(4, 10001, p) && p.referenceGeneration == 2,
        "new sender starts a new reference");
  check(sequence.accept(900, 10001, p), "counter can skip frames");
  p.timestamp += 251;
  check(sequence.accept(40, 10001, p) && p.referenceGeneration == 3,
        "backward counter recovers after timeout");

  OpenTrackSequence wrapping(250);
  check(wrapping.accept(UINT32_MAX, 10000, p), "last counter value accepted");
  check(wrapping.accept(0, 10000, p) && p.referenceGeneration == 0,
        "normal counter wrap keeps the reference");
}

void checkShmFormats() {
  auto rgb = shmFormatInfo(WL_SHM_FORMAT_RGB888);
  auto bgr = shmFormatInfo(WL_SHM_FORMAT_BGR888);
  check(rgb.pixelBytes == 3 && rgb.swapRB, "RGB888 stores blue first");
  check(bgr.pixelBytes == 3 && !bgr.swapRB, "BGR888 stores red first");
  auto xrgb = shmFormatInfo(WL_SHM_FORMAT_XRGB8888);
  auto xbgr = shmFormatInfo(WL_SHM_FORMAT_XBGR8888);
  check(xrgb.pixelBytes == 4 && xrgb.swapRB, "XRGB8888 stores blue first");
  check(xbgr.pixelBytes == 4 && !xbgr.swapRB, "XBGR8888 stores red first");
  check(shmFormatInfo(WL_SHM_FORMAT_RGB565).pixelBytes == 0,
        "unknown upload layout rejected");
  const std::array<uint8_t, 16> source{1, 2, 3, 4,  5,  6,  99, 99,
                                       7, 8, 9, 10, 11, 12, 99, 99};
  std::array<uint8_t, 12> packed{};
  packShmRows(packed.data(), source.data(), 6, 2, 8);
  check(packed ==
            std::array<uint8_t, 12>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        "24-bit row padding removed without changing pixel order");
}

int main() {
  try {
    const auto state =
        "connected_device_brand=XREAL\nconnected_device_model=Air\n"
        "sbs_mode_supported=true\nsbs_mode_enabled=true\n";
    check(decodeStereoState(state) == true, "confirmed SBS state");
    check(!decodeStereoState("sbs_mode_enabled=true\n").has_value(),
          "partial state refused");
    check(stereoLayout("auto", false, 3840, 1080, 3840, 1080, 1, 0, true) ==
              StereoLayout::sbs,
          "SBS uses two viewports on matching hardware");
    check(stereoLayout("auto", false, 1920, 1080, 1920, 1080, 1, 0, false) ==
              StereoLayout::mono,
          "2D hardware remains mono");
    check(stereoLayout("auto", false, 3840, 1080, 3840, 1080, 1, 0, false) ==
              StereoLayout::waiting,
          "mismatched hardware and transport wait");
    check(stereoLayout("auto", false, 3840, 1080, 3840, 1080, 2, 0, true) ==
              StereoLayout::waiting,
          "scaled SBS refused");
    check(stereoLayout("mono", false, 3840, 1080, 3840, 1080, 1, 0, true) ==
              StereoLayout::waiting,
          "mono cannot be stretched across physical eyes");
    check(stereoLayout("sbs", true, 1280, 360, 0, 0, 1, 0, std::nullopt) ==
              StereoLayout::sbs,
          "stereo preview works without hardware");
    check(std::abs(eyeOffset(0, 64) + .032) < 1e-9 &&
              std::abs(eyeOffset(1, 64) - .032) < 1e-9,
          "parallel eye spacing in meters");
    std::array<uint8_t, 186> b{};
    b[0] = 5;
    b[1] = 1;
    put<uint64_t>(b, 113, 1000);
    put<float>(b, 121 + 12, 1);
    parity(b);
    auto p = breezy_v5::decode(b, 1000, 250);
    check(p.valid, "valid v5");
    auto bad = b;
    bad[0] = 4;
    check(!breezy_v5::decode(bad, 1000, 250).valid, "ABI version rejected");
    check(!breezy_v5::decode(std::span(b).first(185), 1000, 250).valid,
          "short record rejected");
    bad = b;
    bad[125] ^= 1;
    check(!breezy_v5::decode(bad, 1000, 250).valid, "torn record rejected");
    check(!breezy_v5::decode(b, 1300, 250).valid, "stale rejected");
    check(!breezy_v5::decode(b, 900, 250).valid, "future rejected");
    bad = b;
    bad[1] = 0;
    check(!breezy_v5::decode(bad, 1000, 250).valid, "disabled rejected");
    bad = b;
    put<float>(bad, 121, std::numeric_limits<float>::quiet_NaN());
    parity(bad);
    check(!breezy_v5::decode(bad, 1000, 250).valid, "NaN rejected");
    bad = b;
    put<float>(bad, 121 + 7 * 4, 1);
    put<float>(bad, 121 + 11 * 4, 1);
    parity(bad);
    check(!breezy_v5::decode(bad, 1000, 250).valid,
          "calibration reset rejected");
    // Driver +Z yaw maps to renderer +Y yaw.
    bad = b;
    put<float>(bad, 129, float(std::sin(rad(15))));
    put<float>(bad, 133, float(std::cos(rad(15))));
    parity(bad);
    p = breezy_v5::decode(bad, 1000, 250);
    auto v = p.orientation.rotate({0, 0, -1});
    check(v.x < -.49 && v.z < -.86, "NWU conversion");
    auto reference =
        (Quat::axis({0, 1, 0}, rad(179)) * Quat::axis({1, 0, 0}, rad(80)))
            .normalized();
    auto delta = reference.inverse() * reference;
    v = delta.rotate({0, 0, -1});
    check(std::abs(v.x) < 1e-8 && std::abs(v.y) < 1e-8 && v.z < -.99999,
          "recenter combined rotation");
    delta = Quat::axis({0, 1, 0}, rad(179)).inverse() *
            Quat::axis({0, 1, 0}, rad(-179));
    v = delta.rotate({0, 0, -1});
    check(std::abs(v.x) < .04, "yaw wrap");
    auto q = Quat::axis({0, 1, 0}, rad(20));
    auto inv = axisPolicy(q, true, false, false, true);
    check(inv.rotate({0, 0, -1}).x > 0, "yaw inversion");
    check(axisPolicy(Quat::axis({0, 0, 1}, rad(30)), false, false, false, false)
                  .w > .9999,
          "roll disabled");
    auto level =
        Quat::axis({0, 1, 0}, rad(35)) * Quat::axis({1, 0, 0}, rad(25));
    auto tilted = level * Quat::axis({0, 0, 1}, rad(40));
    checkRotation(axisPolicy(tilted, false, false, false, false), level,
                  "level horizon preserves forward direction");
    std::array<uint8_t, 52> packet{};
    double angle = 30;
    memcpy(packet.data() + 24, &angle, 8);
    auto ot = decodeOpenTrack(packet, 1000);
    check(ot.valid && ot.timestamp == 1000, "OpenTrack decode");
    check(ot.orientation.rotate({0, 0, -1}).x < -.49, "OpenTrack yaw basis");
    check(!decodeOpenTrack(std::span(packet).first(48), 1000).valid,
          "OpenTrack rejects other wire format");
    angle = std::numeric_limits<double>::infinity();
    memcpy(packet.data() + 32, &angle, 8);
    check(!decodeOpenTrack(packet, 1000).valid, "OpenTrack rejects nonfinite");
    checkHeadTracking();
    checkOpenTrackSequence();
    checkShmFormats();
    Config c;
    c.validate();
    c.virtual_output_name = "DP-3;bad";
    bool rejected = false;
    try {
      c.validate();
    } catch (...) {
      rejected = true;
    }
    check(rejected, "unsafe output name rejected");
    std::cout << "Pose ABI, tracking, SHM and config checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
