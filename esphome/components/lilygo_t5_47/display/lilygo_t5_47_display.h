#pragma once

#ifdef USE_ESP32

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/macros.h"
#include "esphome/core/version.h"

#include <array>

namespace esphome::lilygo_t5_47 {

/// Driver for the ED047TC1 parallel e-paper panel on the LilyGo T5 4.7" (ESP32 version).
///
/// The panel has no controller: the source/gate drivers are clocked directly. Control and power signals go
/// through a 74HCT4094 shift register, pixel data is bit-banged over an 8-bit bus. Each refresh clears the
/// panel to white and then darkens every pixel over 15 frames of increasing duration (16 grey levels).
class LilygoT547Display : public display::DisplayBuffer {
 public:
  static constexpr int WIDTH = 960;
  static constexpr int HEIGHT = 540;

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_GRAYSCALE; }

  void fill(Color color) override;
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 11, 0)
  /// Clear to white, like real paper.
  void clear() override { this->fill(display::COLOR_ON); }
#endif

  void set_data_pin(uint8_t index, InternalGPIOPin *pin) { this->data_pins_[index] = pin; }
  void set_cfg_data_pin(InternalGPIOPin *pin) { this->cfg_data_pin_ = pin; }
  void set_cfg_clock_pin(InternalGPIOPin *pin) { this->cfg_clock_pin_ = pin; }
  void set_cfg_strobe_pin(InternalGPIOPin *pin) { this->cfg_strobe_pin_ = pin; }
  void set_ckv_pin(InternalGPIOPin *pin) { this->ckv_pin_ = pin; }
  void set_sth_pin(InternalGPIOPin *pin) { this->sth_pin_ = pin; }
  void set_ckh_pin(InternalGPIOPin *pin) { this->ckh_pin_ = pin; }
  void set_test_pattern(bool test_pattern) { this->test_pattern_ = test_pattern; }

 protected:
  /// Direct register access to a GPIO, bypassing the (slow) pin abstraction for the timing-critical paths.
  struct FastPin {
    volatile uint32_t *set_reg;
    volatile uint32_t *clear_reg;
    uint32_t mask;

    void high() const { *this->set_reg = this->mask; }
    void low() const { *this->clear_reg = this->mask; }
    void write(bool value) const { value ? this->high() : this->low(); }
  };

  /// Bits of the 74HCT4094 control register.
  enum ConfigBit : uint8_t {
    CFG_LATCH_ENABLE = 1 << 0,
    CFG_POWER_DISABLE = 1 << 1,
    CFG_POS_POWER_ENABLE = 1 << 2,
    CFG_NEG_POWER_ENABLE = 1 << 3,
    CFG_STV = 1 << 4,
    CFG_SCAN_DIRECTION = 1 << 5,
    CFG_MODE = 1 << 6,
    CFG_OUTPUT_ENABLE = 1 << 7,
  };

  int get_width_internal() override { return WIDTH; }
  int get_height_internal() override { return HEIGHT; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  static FastPin make_fast_pin(InternalGPIOPin *pin);
  static uint8_t color_to_grey(Color color);

  void display_();
  void clear_panel_();
  void draw_greyscale_();
  void draw_test_pattern_();
  void draw_constant_frame_(uint8_t panel_byte, uint32_t row_ticks);

  void power_on_();
  void power_off_();
  void set_config_bits_(uint8_t bits, bool value);
  void push_config_();
  void start_frame_();
  void end_frame_();
  void output_row_(uint32_t high_ticks);
  void write_row_(const uint8_t *panel_bytes);
  void write_constant_row_(uint8_t panel_byte);
  void set_data_bus_(uint8_t panel_byte);
  void pulse_ckv_(uint32_t high_ticks, uint32_t low_ticks);
  void wait_ticks_(uint32_t start_cycles, uint32_t ticks);
  void wait_us_(uint32_t us) { this->wait_ticks_(arch_get_cpu_cycle_count(), us * 10); }

  std::array<InternalGPIOPin *, 8> data_pins_{};
  InternalGPIOPin *cfg_data_pin_{nullptr};
  InternalGPIOPin *cfg_clock_pin_{nullptr};
  InternalGPIOPin *cfg_strobe_pin_{nullptr};
  InternalGPIOPin *ckv_pin_{nullptr};
  InternalGPIOPin *sth_pin_{nullptr};
  InternalGPIOPin *ckh_pin_{nullptr};

  FastPin cfg_data_{};
  FastPin cfg_clock_{};
  FastPin cfg_strobe_{};
  FastPin ckv_{};
  FastPin sth_{};
  FastPin ckh_{};

  /// GPIO register values to put a panel byte on the data bus, for pins 0-31 and 32-39 respectively.
  std::array<uint32_t, 256> data_set_low_{};
  std::array<uint32_t, 256> data_set_high_{};
  uint32_t data_mask_low_{0};
  uint32_t data_mask_high_{0};

  uint32_t cycles_per_tick_{0};  ///< CPU cycles per 0.1us
  uint8_t config_{0};
  bool test_pattern_{false};
};

}  // namespace esphome::lilygo_t5_47

#endif  // USE_ESP32
