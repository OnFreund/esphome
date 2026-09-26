#include "lilygo_t5_47_display.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <soc/gpio_struct.h>

#include <cstring>

namespace esphome::lilygo_t5_47 {

static const char *const TAG = "lilygo_t5_47.display";

static constexpr size_t BUFFER_ROW_BYTES = LilygoT547Display::WIDTH / 2;  // 4 bits per pixel
static constexpr size_t PANEL_ROW_BYTES = LilygoT547Display::WIDTH / 4;   // 2 bits per pixel
// Extra no-op bytes clocked out after each row to give the source driver some timing headroom.
static constexpr size_t PANEL_ROW_PADDING_BYTES = 8;

// Each pixel takes a 2-bit command on the data bus; the first pixel of a byte goes on D7/D6.
static constexpr uint8_t PIXEL_DARKEN = 0b01;
static constexpr uint8_t PANEL_BYTE_NOOP = 0x00;
static constexpr uint8_t PANEL_BYTE_DARKEN = 0x55;
static constexpr uint8_t PANEL_BYTE_WHITEN = 0xAA;

// All durations are in ticks of 0.1us.
static constexpr uint32_t ROW_LOW_TICKS = 50;
static constexpr uint32_t CLEAR_FRAME_TICKS = 500;
static constexpr uint8_t CLEAR_CYCLES = 4;
static constexpr uint8_t CLEAR_FRAMES_PER_COLOR = 4;
// Row drive time for each greyscale frame, darkest levels are driven in every frame.
static constexpr uint16_t GREY_FRAME_TICKS[] = {30, 30, 20, 20, 30, 30, 30, 40, 40, 50, 50, 50, 100, 200, 300};
static constexpr uint8_t GREY_LEVELS = 16;

inline void HOT LilygoT547Display::set_data_bus_(uint8_t panel_byte) {
  uint32_t low = this->data_set_low_[panel_byte];
  uint32_t high = this->data_set_high_[panel_byte];
  GPIO.out_w1ts = low;
  GPIO.out_w1tc = this->data_mask_low_ ^ low;
  GPIO.out1_w1ts.val = high;
  GPIO.out1_w1tc.val = this->data_mask_high_ ^ high;
}

inline void HOT LilygoT547Display::wait_ticks_(uint32_t start_cycles, uint32_t ticks) {
  uint32_t cycles = ticks * this->cycles_per_tick_;
  while (arch_get_cpu_cycle_count() - start_cycles < cycles) {
  }
}

void LilygoT547Display::setup() {
  this->init_internal_(BUFFER_ROW_BYTES * HEIGHT);
  if (this->buffer_ == nullptr) {
    this->mark_failed();
    return;
  }

  for (auto *pin : {this->cfg_data_pin_, this->cfg_clock_pin_, this->cfg_strobe_pin_, this->ckv_pin_, this->sth_pin_,
                    this->ckh_pin_}) {
    pin->setup();
    pin->digital_write(false);
  }
  this->cfg_data_ = make_fast_pin(this->cfg_data_pin_);
  this->cfg_clock_ = make_fast_pin(this->cfg_clock_pin_);
  this->cfg_strobe_ = make_fast_pin(this->cfg_strobe_pin_);
  this->ckv_ = make_fast_pin(this->ckv_pin_);
  this->sth_ = make_fast_pin(this->sth_pin_);
  this->ckh_ = make_fast_pin(this->ckh_pin_);

  for (auto *pin : this->data_pins_) {
    pin->setup();
    pin->digital_write(false);
    uint8_t num = pin->get_pin();
    if (num < 32) {
      this->data_mask_low_ |= 1UL << num;
    } else {
      this->data_mask_high_ |= 1UL << (num - 32);
    }
  }
  for (uint32_t value = 0; value < 256; value++) {
    uint32_t low = 0, high = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if ((value & (1 << bit)) == 0)
        continue;
      uint8_t num = this->data_pins_[bit]->get_pin();
      if (num < 32) {
        low |= 1UL << num;
      } else {
        high |= 1UL << (num - 32);
      }
    }
    this->data_set_low_[value] = low;
    this->data_set_high_[value] = high;
  }

  this->cycles_per_tick_ = arch_get_cpu_freq_hz() / 10000000;

  this->config_ = CFG_POWER_DISABLE | CFG_STV | CFG_SCAN_DIRECTION;
  this->push_config_();
}

void LilygoT547Display::dump_config() {
  LOG_DISPLAY("", "LilyGo T5 4.7\" E-Paper", this);
  for (size_t i = 0; i < this->data_pins_.size(); i++) {
    ESP_LOGCONFIG(TAG, "  Data Pin D%u:", static_cast<unsigned>(i));
    LOG_PIN("    ", this->data_pins_[i]);
  }
  LOG_PIN("  Config Data Pin: ", this->cfg_data_pin_);
  LOG_PIN("  Config Clock Pin: ", this->cfg_clock_pin_);
  LOG_PIN("  Config Strobe Pin: ", this->cfg_strobe_pin_);
  LOG_PIN("  CKV Pin: ", this->ckv_pin_);
  LOG_PIN("  STH Pin: ", this->sth_pin_);
  LOG_PIN("  CKH Pin: ", this->ckh_pin_);
  LOG_UPDATE_INTERVAL(this);
}

void LilygoT547Display::update() {
#if ESPHOME_VERSION_CODE < VERSION_CODE(2025, 11, 0)
  // Display::clear() can't be overridden before 2025.11 and fills black, so auto-clear to white here instead
  if (this->auto_clear_enabled_) {
    this->fill(display::COLOR_ON);
    this->auto_clear_enabled_ = false;
    this->do_update_();
    this->auto_clear_enabled_ = true;
  } else {
    this->do_update_();
  }
#else
  this->do_update_();
#endif
  this->display_();
}

uint8_t LilygoT547Display::color_to_grey(Color color) {
  // Luminance scaled to 0 (black) .. 15 (white)
  return (color.r * 77 + color.g * 150 + color.b * 29) >> 12;
}

void HOT LilygoT547Display::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
    return;

  uint8_t grey = color_to_grey(color);
  uint8_t *pos = &this->buffer_[y * BUFFER_ROW_BYTES + x / 2];
  if (x & 1) {
    *pos = (*pos & 0x0F) | (grey << 4);
  } else {
    *pos = (*pos & 0xF0) | grey;
  }
}

void LilygoT547Display::fill(Color color) {
  // If clipping is active, fall back to base implementation
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }

  uint8_t grey = color_to_grey(color);
  memset(this->buffer_, (grey << 4) | grey, BUFFER_ROW_BYTES * HEIGHT);
}

void LilygoT547Display::display_() {
  uint32_t start_time = millis();
  this->power_on_();
  this->clear_panel_();
  this->draw_greyscale_();
  this->power_off_();
  ESP_LOGD(TAG, "Refresh took %" PRIu32 "ms", millis() - start_time);
}

void LilygoT547Display::clear_panel_() {
  // Alternate between black and white a few times to remove ghosting of the previous image, ending on white.
  for (uint8_t cycle = 0; cycle < CLEAR_CYCLES; cycle++) {
    for (uint8_t i = 0; i < CLEAR_FRAMES_PER_COLOR; i++)
      this->draw_constant_frame_(PANEL_BYTE_DARKEN, CLEAR_FRAME_TICKS);
    for (uint8_t i = 0; i < CLEAR_FRAMES_PER_COLOR; i++)
      this->draw_constant_frame_(PANEL_BYTE_WHITEN, CLEAR_FRAME_TICKS);
  }
}

void LilygoT547Display::draw_constant_frame_(uint8_t panel_byte, uint32_t row_ticks) {
  this->start_frame_();
  // Rows are pipelined: the data written after each row is latched out on the next one.
  this->write_constant_row_(panel_byte);
  for (int y = 0; y < HEIGHT; y++) {
    this->output_row_(row_ticks);
    this->write_constant_row_(panel_byte);
  }
  this->output_row_(row_ticks);
  this->end_frame_();
  App.feed_wdt();
}

void HOT LilygoT547Display::draw_greyscale_() {
  // The panel is white at this point. Frame n darkens every pixel whose grey level is below
  // GREY_LEVELS - 1 - n, so darker pixels are driven for more (and longer) frames.
  std::array<uint8_t, 256> lut;
  std::array<uint8_t, PANEL_ROW_BYTES> row;
  for (uint8_t frame = 0; frame < GREY_LEVELS - 1; frame++) {
    // Map a buffer byte (two pixels, even pixel in the low nibble) to their two commands
    for (uint32_t value = 0; value < 256; value++) {
      uint8_t even = (value & 0x0F) + frame < GREY_LEVELS - 1 ? PIXEL_DARKEN : 0;
      uint8_t odd = (value >> 4) + frame < GREY_LEVELS - 1 ? PIXEL_DARKEN : 0;
      lut[value] = (even << 2) | odd;
    }

    uint32_t row_ticks = GREY_FRAME_TICKS[frame];
    const uint8_t *src = this->buffer_;
    this->start_frame_();
    this->write_constant_row_(PANEL_BYTE_NOOP);
    for (int y = 0; y < HEIGHT; y++) {
      for (auto &out : row) {
        out = (lut[src[0]] << 4) | lut[src[1]];
        src += 2;
      }
      this->output_row_(row_ticks);
      this->write_row_(row.data());
    }
    this->output_row_(row_ticks);
    this->end_frame_();
    App.feed_wdt();
  }
}

void LilygoT547Display::power_on_() {
  this->set_config_bits_(CFG_SCAN_DIRECTION, true);
  this->set_config_bits_(CFG_POWER_DISABLE, false);
  this->push_config_();
  this->wait_us_(100);
  this->set_config_bits_(CFG_NEG_POWER_ENABLE, true);
  this->push_config_();
  this->wait_us_(500);
  this->set_config_bits_(CFG_POS_POWER_ENABLE, true);
  this->push_config_();
  this->wait_us_(100);
  this->set_config_bits_(CFG_STV, true);
  this->push_config_();
  this->sth_.high();
}

void LilygoT547Display::power_off_() {
  this->set_config_bits_(CFG_POS_POWER_ENABLE, false);
  this->push_config_();
  this->wait_us_(10);
  this->set_config_bits_(CFG_NEG_POWER_ENABLE, false);
  this->push_config_();
  this->wait_us_(100);
  this->set_config_bits_(CFG_POWER_DISABLE, true);
  this->push_config_();
  this->set_config_bits_(CFG_STV, false);
  this->push_config_();
}

void LilygoT547Display::set_config_bits_(uint8_t bits, bool value) {
  if (value) {
    this->config_ |= bits;
  } else {
    this->config_ &= ~bits;
  }
}

void HOT LilygoT547Display::push_config_() {
  this->cfg_strobe_.low();
  // Most significant bit (output enable) is shifted in first
  for (int bit = 7; bit >= 0; bit--) {
    this->cfg_clock_.low();
    this->cfg_data_.write(this->config_ & (1 << bit));
    this->cfg_clock_.high();
  }
  this->cfg_strobe_.high();
}

void LilygoT547Display::start_frame_() {
  this->set_config_bits_(CFG_MODE, true);
  this->push_config_();
  this->pulse_ckv_(10, 10);

  // Clock the start pulse into the gate driver, this is timing sensitive
  this->set_config_bits_(CFG_STV, false);
  this->push_config_();
  this->wait_us_(1);
  this->pulse_ckv_(100, 100);
  this->set_config_bits_(CFG_STV, true);
  this->push_config_();
  this->pulse_ckv_(100, 0);

  this->set_config_bits_(CFG_OUTPUT_ENABLE, true);
  this->push_config_();
  this->pulse_ckv_(10, 10);
}

void LilygoT547Display::end_frame_() {
  this->set_config_bits_(CFG_OUTPUT_ENABLE, false);
  this->push_config_();
  this->set_config_bits_(CFG_MODE, false);
  this->push_config_();
  this->pulse_ckv_(10, 10);
  this->pulse_ckv_(10, 10);
}

void HOT LilygoT547Display::output_row_(uint32_t high_ticks) {
  // Latch the previously written row into the source driver outputs, then drive it for the requested time.
  this->set_config_bits_(CFG_LATCH_ENABLE, true);
  this->push_config_();
  this->set_config_bits_(CFG_LATCH_ENABLE, false);
  this->push_config_();
  this->pulse_ckv_(high_ticks, ROW_LOW_TICKS);
}

void HOT LilygoT547Display::write_row_(const uint8_t *panel_bytes) {
  this->sth_.low();
  for (size_t i = 0; i < PANEL_ROW_BYTES; i++) {
    this->set_data_bus_(panel_bytes[i]);
    this->ckh_.high();
    this->ckh_.low();
  }
  this->set_data_bus_(PANEL_BYTE_NOOP);
  for (size_t i = 0; i < PANEL_ROW_PADDING_BYTES; i++) {
    this->ckh_.high();
    this->ckh_.low();
  }
  this->sth_.high();
}

void HOT LilygoT547Display::write_constant_row_(uint8_t panel_byte) {
  this->set_data_bus_(panel_byte);
  this->sth_.low();
  for (size_t i = 0; i < PANEL_ROW_BYTES; i++) {
    this->ckh_.high();
    this->ckh_.low();
  }
  this->set_data_bus_(PANEL_BYTE_NOOP);
  for (size_t i = 0; i < PANEL_ROW_PADDING_BYTES; i++) {
    this->ckh_.high();
    this->ckh_.low();
  }
  this->sth_.high();
}

void HOT LilygoT547Display::pulse_ckv_(uint32_t high_ticks, uint32_t low_ticks) {
  {
    // The high time sets how long a row is driven, so keep interrupts from stretching it
    InterruptLock lock;
    uint32_t start = arch_get_cpu_cycle_count();
    this->ckv_.high();
    this->wait_ticks_(start, high_ticks);
    this->ckv_.low();
  }
  this->wait_ticks_(arch_get_cpu_cycle_count(), low_ticks);
}

LilygoT547Display::FastPin LilygoT547Display::make_fast_pin(InternalGPIOPin *pin) {
  uint8_t num = pin->get_pin();
  if (num < 32)
    return {&GPIO.out_w1ts, &GPIO.out_w1tc, 1UL << num};
  return {&GPIO.out1_w1ts.val, &GPIO.out1_w1tc.val, 1UL << (num - 32)};
}

}  // namespace esphome::lilygo_t5_47

#endif  // USE_ESP32
