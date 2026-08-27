#include "esphome/components/lora_mesh/outbound_airtime.h"

#include <cstdio>
#include <vector>

namespace esphome {
void test_random_set(uint32_t value);
}

using esphome::lora_mesh::LoRaRadio;
using esphome::lora_mesh::LoraMesh;
using esphome::lora_mesh::OutboundAirtime;
using esphome::lora_mesh::OutboundPacketKind;
using esphome::lora_mesh::Packet;
using esphome::lora_mesh::TransmissionOutcome;

static int failures = 0;

#define EXPECT_TRUE(condition) \
  do { \
    if (!(condition)) { \
      ++failures; \
      printf("  FAIL %s:%d: expected true: %s\n", __FILE__, __LINE__, #condition); \
    } \
  } while (0)

#define EXPECT_FALSE(condition) EXPECT_TRUE(!(condition))
#define EXPECT_EQ(left, right) EXPECT_TRUE((left) == (right))

class FakeRadio : public LoRaRadio {
 public:
  TransmissionOutcome transmit_packet(const Packet &packet) override {
    this->attempted.push_back(packet);
    return this->outcome;
  }
  size_t get_max_packet_size() override { return 255; }
  void attach_listener(LoraMesh *) override {}

  std::vector<Packet> attempted;
  TransmissionOutcome outcome{TransmissionOutcome::SUCCESS};
};

struct HelloHooks {
  Packet refreshed;
  Packet attempted;
  size_t refresh_count{0};
  size_t attempt_count{0};
};

static Packet refresh_hello(void *context) {
  auto *hooks = static_cast<HelloHooks *>(context);
  ++hooks->refresh_count;
  return hooks->refreshed;
}

static void hello_attempted(void *context, const Packet &packet) {
  auto *hooks = static_cast<HelloHooks *>(context);
  ++hooks->attempt_count;
  hooks->attempted = packet;
}

static Packet packet_with(uint8_t value) { return Packet{value}; }

static void test_jitter_and_fifo_are_owned_by_airtime_module() {
  FakeRadio radio;
  HelloHooks hooks;
  OutboundAirtime airtime(&radio, &hooks, refresh_hello, hello_attempted);
  EXPECT_TRUE(airtime.enqueue(packet_with(1)));
  EXPECT_TRUE(airtime.enqueue(packet_with(2)));

  esphome::test_random_set(60);
  airtime.drain(1000);
  airtime.drain(1059);
  EXPECT_EQ(radio.attempted.size(), 0u);

  airtime.drain(1060);
  EXPECT_EQ(radio.attempted.size(), 1u);
  EXPECT_EQ(radio.attempted[0][0], 1u);
}

static void test_failed_attempt_is_dropped_without_retry() {
  FakeRadio radio;
  HelloHooks hooks;
  OutboundAirtime airtime(&radio, &hooks, refresh_hello, hello_attempted);
  airtime.set_jitter(0);
  radio.outcome = TransmissionOutcome::TIMEOUT;
  EXPECT_TRUE(airtime.enqueue(packet_with(1)));
  airtime.drain(1000);

  radio.outcome = TransmissionOutcome::SUCCESS;
  airtime.drain(1001);
  EXPECT_EQ(radio.attempted.size(), 1u);
}

static void test_stale_hello_is_refreshed_at_airtime_and_reported() {
  FakeRadio radio;
  HelloHooks hooks;
  hooks.refreshed = packet_with(9);
  OutboundAirtime airtime(&radio, &hooks, refresh_hello, hello_attempted);
  airtime.set_jitter(0);
  EXPECT_TRUE(airtime.enqueue(packet_with(1), OutboundPacketKind::HELLO));
  airtime.invalidate_queued_hello();

  airtime.drain(1000);

  EXPECT_EQ(hooks.refresh_count, 1u);
  EXPECT_EQ(hooks.attempt_count, 1u);
  EXPECT_EQ(radio.attempted.size(), 1u);
  EXPECT_EQ(radio.attempted[0][0], 9u);
  EXPECT_EQ(hooks.attempted[0], 9u);
  EXPECT_FALSE(airtime.has_queued_hello());
}

int main() {
  test_jitter_and_fifo_are_owned_by_airtime_module();
  test_failed_attempt_is_dropped_without_retry();
  test_stale_hello_is_refreshed_at_airtime_and_reported();
  printf("%s outbound_airtime focused tests (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
         failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
