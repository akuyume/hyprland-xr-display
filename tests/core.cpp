#include "config.hpp"
#include "pose.hpp"
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
int main() {
  try {
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
    auto oldDelta = Quat::axis({0, 1, 0}, rad(20));
    auto recovered = Quat::axis({1, 0, 0}, rad(-10));
    auto newReference = recovered * oldDelta.inverse();
    auto retained = newReference.inverse() * recovered;
    check(std::abs(retained.y - oldDelta.y) < 1e-8, "restart continuity");
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
    std::cout << "Pose ABI, OpenTrack, config and quaternion checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
