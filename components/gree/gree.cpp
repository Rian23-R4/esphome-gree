#include "gree.h"
#include "esphome/components/remote_base/remote_base.h"

namespace esphome::gree {

static const char *const TAG = "gree.climate";

void GreeClimate::set_model(Model model) {
  if (model == GREE_YAN) {
    // YAN only has a vertical vane; the horizontal swing IR bytes are not defined for this model.
    this->swing_modes_.erase(climate::CLIMATE_SWING_HORIZONTAL);
    this->swing_modes_.erase(climate::CLIMATE_SWING_BOTH);
  }
  if (model == GREE_YX1FF) {
    this->fan_modes_.insert(climate::CLIMATE_FAN_QUIET);   // YX1FF 4 speed
    this->presets_.insert(climate::CLIMATE_PRESET_NONE);   // YX1FF sleep mode
    this->presets_.insert(climate::CLIMATE_PRESET_SLEEP);  // YX1FF sleep mode
  }

  this->model_ = model;
}

void GreeClimate::set_dred_support(bool support) {
  if (support) {
    // Preset mapping (see README): Eco = SE, Away = DRED d2 (50%), Boost = DRED d3 (75%).
    this->presets_.insert(climate::CLIMATE_PRESET_NONE);
    this->presets_.insert(climate::CLIMATE_PRESET_ECO);
    this->presets_.insert(climate::CLIMATE_PRESET_AWAY);
    this->presets_.insert(climate::CLIMATE_PRESET_BOOST);
  }
  this->dred_support_ = support;
}

void GreeClimate::set_mode_bit(uint8_t bit_mask, bool enabled) {
  if (enabled) {
    this->mode_bits_ |= bit_mask;
  } else {
    this->mode_bits_ &= ~bit_mask;
  }
  this->transmit_state();
}

void GreeClimate::control(const climate::ClimateCall &call) {
  if (!this->dred_support_) {
    climate_ir::ClimateIR::control(call);
    return;
  }

  auto new_preset = call.get_preset();
  if (!new_preset.has_value()) {
    climate_ir::ClimateIR::control(call);
    return;
  }

  switch (*new_preset) {
    case climate::CLIMATE_PRESET_ECO:
    case climate::CLIMATE_PRESET_AWAY:
    case climate::CLIMATE_PRESET_BOOST:
    case climate::CLIMATE_PRESET_NONE:
      break;
    default:
      climate_ir::ClimateIR::control(call);
      return;
  }

  // Apply any non-preset fields from the call without transmitting yet.
  auto mode = call.get_mode();
  if (mode.has_value())
    this->mode = *mode;
  auto target_temperature = call.get_target_temperature();
  if (target_temperature.has_value())
    this->target_temperature = *target_temperature;
  auto fan_mode = call.get_fan_mode();
  if (fan_mode.has_value())
    this->fan_mode = fan_mode;
  auto swing_mode = call.get_swing_mode();
  if (swing_mode.has_value())
    this->swing_mode = *swing_mode;
  this->preset = *new_preset;

  if (*new_preset == climate::CLIMATE_PRESET_ECO) {
    this->dred_level_ = GREE_DRED_OFF;
    this->start_eco_sequence_();
  } else if (*new_preset == climate::CLIMATE_PRESET_AWAY) {
    this->dred_level_ = GREE_DRED_D2;
    this->start_dred_sequence_(GREE_DRED_D2);
  } else if (*new_preset == climate::CLIMATE_PRESET_BOOST) {
    this->dred_level_ = GREE_DRED_D3;
    this->start_dred_sequence_(GREE_DRED_D3);
  } else {  // NONE: eco off + DRED off
    this->dred_level_ = GREE_DRED_OFF;
    this->start_none_sequence_();
  }

  this->publish_state();
}

static uint8_t gree_checksum_(const uint8_t state[GREE_STATE_FRAME_SIZE]) {
  return (uint8_t) ((((state[0] & 0x0F) + (state[1] & 0x0F) + (state[2] & 0x0F) + (state[3] & 0x0F) +
                      ((state[4] & 0xF0) >> 4) + ((state[5] & 0xF0) >> 4) + ((state[6] & 0xF0) >> 4) + 0x0A) &
                     0x0F)
                    << 4);
}

void GreeClimate::send_gree_frame_(const uint8_t state[GREE_STATE_FRAME_SIZE]) {
  const Model model = this->model_;

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();
  data->set_carrier_frequency(GREE_IR_FREQUENCY);

  data->mark(GREE_HEADER_MARK);
  if (model == GREE_YAC1FB9) {
    data->space(GREE_YAC1FB9_HEADER_SPACE);
  } else {
    data->space(GREE_HEADER_SPACE);
  }

  for (int i = 0; i < 4; i++) {
    for (uint8_t mask = 1; mask > 0; mask <<= 1) {  // iterate through bit mask
      data->mark(GREE_BIT_MARK);
      bool bit = state[i] & mask;
      data->space(bit ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
    }
  }

  data->mark(GREE_BIT_MARK);
  data->space(GREE_ZERO_SPACE);
  data->mark(GREE_BIT_MARK);
  data->space(GREE_ONE_SPACE);
  data->mark(GREE_BIT_MARK);
  data->space(GREE_ZERO_SPACE);

  data->mark(GREE_BIT_MARK);
  if (model == GREE_YAC1FB9) {
    data->space(GREE_YAC1FB9_MESSAGE_SPACE);
  } else {
    data->space(GREE_MESSAGE_SPACE);
  }

  for (int i = 4; i < 8; i++) {
    for (uint8_t mask = 1; mask > 0; mask <<= 1) {  // iterate through bit mask
      data->mark(GREE_BIT_MARK);
      bool bit = state[i] & mask;
      data->space(bit ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
    }
  }

  data->mark(GREE_BIT_MARK);
  // Trailing gap: the physical remote (and working Pronto buttons) end each
  // frame with a ~20 ms space. Without it, back-to-back frames (the DRED
  // 2-packet burst) merge on the wire and the AC ignores the burst.
  if (model == GREE_YAC1FB9) {
    data->space(GREE_YAC1FB9_MESSAGE_SPACE);
  } else {
    data->space(GREE_MESSAGE_SPACE);
  }

  transmit.perform();
}

void GreeClimate::transmit_dred_state_() {
  // Remote-family state frame (the "A" frame). The level itself is carried by
  // the near-zero "C" frame sent 40 ms later.
  uint8_t a[GREE_STATE_FRAME_SIZE];
  a[0] = this->fan_speed_() | this->operation_mode_();
  a[1] = this->temperature_();
  a[2] = 0x60;
  a[3] = 0x50;
  a[4] = 0x06;
  a[5] = 0x80;
  a[6] = 0x00;
  a[7] = gree_checksum_(a);
  this->send_gree_frame_(a);

  this->set_timeout("dred_c", GREE_DRED_BURST_GAP, [this]() {
    uint8_t c[GREE_STATE_FRAME_SIZE] = {0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x00};
    switch (this->dred_level_) {
      case GREE_DRED_D2:
        c[3] = 0xA4;
        break;
      case GREE_DRED_D3:
        c[0] = 0x01;
        break;
      default:
        break;
    }
    c[7] = gree_checksum_(c);
    this->send_gree_frame_(c);
  });
}

void GreeClimate::start_dred_sequence_(uint8_t level) {
  this->cancel_timeout("dred_c");
  this->cancel_timeout("seq2");
  this->dred_level_ = level;
  // 1) Regular state frame with the Eco bit off (SE and DRED are mutually exclusive).
  this->transmit_state();
  // 2) Wait for the unit to settle, then send the [A, C] burst.
  this->set_timeout("seq1", GREE_DRED_SETTLE_DELAY, [this]() { this->transmit_dred_state_(); });
}

void GreeClimate::start_eco_sequence_() {
  this->cancel_timeout("dred_c");
  this->cancel_timeout("seq2");
  // 1) Turn DRED off first ([A, C-off] burst).
  this->dred_level_ = GREE_DRED_OFF;
  this->transmit_dred_state_();
  // 2) Then the regular state frame with the Eco bit on.
  this->set_timeout("seq2", GREE_DRED_SETTLE_DELAY, [this]() { this->transmit_state(); });
}

void GreeClimate::start_none_sequence_() {
  this->cancel_timeout("dred_c");
  this->cancel_timeout("seq2");
  // 1) DRED off ([A, C-off] burst).
  this->dred_level_ = GREE_DRED_OFF;
  this->transmit_dred_state_();
  // 2) Regular state frame, Eco bit off.
  this->set_timeout("seq2", GREE_DRED_BURST_GAP, [this]() { this->transmit_state(); });
}

void GreeClimate::transmit_state() {
  uint8_t remote_state[GREE_STATE_FRAME_SIZE] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00};
  const Model model = this->model_;

  remote_state[0] = this->fan_speed_() | this->operation_mode_();
  remote_state[1] = this->temperature_();

  if (model == GREE_YAN) {
    remote_state[3] = 0x50;  // bits 4..7 always 0101
    remote_state[4] = this->vertical_swing_();
  }

  if (model == GREE_YX1FF || model == GREE_YAG) {
    remote_state[2] = 0x60;
    remote_state[3] = 0x50;
    remote_state[4] = this->vertical_swing_();
  }

  if (model == GREE_YAG) {
    remote_state[5] = 0x40;

    if (this->vertical_swing_() == GREE_VDIR_SWING || this->horizontal_swing_() == GREE_HDIR_SWING) {
      remote_state[0] |= (1 << 6);
    }
  }

  if (model == GREE_YAC || model == GREE_YAG) {
    remote_state[4] |= (this->horizontal_swing_() << 4);
  }

  if (model == GREE_YAA || model == GREE_YAC || model == GREE_YAC1FB9) {
    remote_state[3] = 0x50;  // bits 4..7 always 0101
    remote_state[6] = 0x20;  // YAA1FB, FAA1FB1, YB1F2 bits 4..7 always 0010

    if (this->vertical_swing_() == GREE_VDIR_SWING) {
      remote_state[0] |= (1 << 6);  // Enable swing by setting bit 6
    } else if (this->vertical_swing_() != GREE_VDIR_AUTO) {
      remote_state[5] = this->vertical_swing_();
    }
  }

  if (model == GREE_YAN || model == GREE_YAA || model == GREE_YAC || model == GREE_YAC1FB9) {
    // Merge the mode bits into remote_state[2]
    // Clear the mode bits (bits 4-7) and OR in the current mode_bits_
    remote_state[2] = (remote_state[2] & 0x0F) | this->mode_bits_;
  }

  if (model == GREE_YX1FF) {
    if (this->fan_speed_() == GREE_FAN_TURBO) {
      remote_state[2] |= GREE_FAN_TURBO_BIT;
    }

    if (this->preset_() == GREE_PRESET_SLEEP) {
      remote_state[0] |= GREE_PRESET_SLEEP_BIT;
    }
  }

  // Calculate the checksum
  if (model == GREE_YAN || model == GREE_YX1FF) {
    remote_state[7] = ((remote_state[0] << 4) + (remote_state[1] << 4) + 0xC0);
  } else {
    remote_state[7] = gree_checksum_(remote_state);
  }

  // Eco (SE): the Econo bit lives in the low nibble of byte 7 and is not part
  // of the checksum.
  if (this->preset_() == GREE_PRESET_ECO) {
    remote_state[7] |= GREE_PRESET_ECO_BIT;
  }

  this->send_gree_frame_(remote_state);
}

uint8_t GreeClimate::operation_mode_() {
  uint8_t operating_mode = GREE_MODE_ON;

  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
      operating_mode |= GREE_MODE_COOL;
      break;
    case climate::CLIMATE_MODE_DRY:
      operating_mode |= GREE_MODE_DRY;
      break;
    case climate::CLIMATE_MODE_HEAT:
      operating_mode |= GREE_MODE_HEAT;
      break;
    case climate::CLIMATE_MODE_HEAT_COOL:
      operating_mode |= GREE_MODE_AUTO;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      operating_mode |= GREE_MODE_FAN;
      break;
    case climate::CLIMATE_MODE_OFF:
    default:
      operating_mode = GREE_MODE_OFF;
      break;
  }

  return operating_mode;
}

uint8_t GreeClimate::fan_speed_() {
  // YX1FF has 4 fan speeds -- we treat low as quiet and turbo as high
  if (this->model_ == GREE_YX1FF) {
    switch (this->fan_mode.value_or(climate::CLIMATE_FAN_ON)) {
      case climate::CLIMATE_FAN_QUIET:
        return GREE_FAN_1;
      case climate::CLIMATE_FAN_LOW:
        return GREE_FAN_2;
      case climate::CLIMATE_FAN_MEDIUM:
        return GREE_FAN_3;
      case climate::CLIMATE_FAN_HIGH:
        return GREE_FAN_TURBO;
      case climate::CLIMATE_FAN_AUTO:
      default:
        return GREE_FAN_AUTO;
    }
  }

  switch (this->fan_mode.value_or(climate::CLIMATE_FAN_ON)) {
    case climate::CLIMATE_FAN_LOW:
      return GREE_FAN_1;
    case climate::CLIMATE_FAN_MEDIUM:
      return GREE_FAN_2;
    case climate::CLIMATE_FAN_HIGH:
      return GREE_FAN_3;
    case climate::CLIMATE_FAN_AUTO:
    default:
      return GREE_FAN_AUTO;
  }
}

uint8_t GreeClimate::horizontal_swing_() {
  switch (this->swing_mode) {
    case climate::CLIMATE_SWING_HORIZONTAL:
    case climate::CLIMATE_SWING_BOTH:
      return GREE_HDIR_SWING;
    default:
      return GREE_HDIR_MANUAL;
  }
}

uint8_t GreeClimate::vertical_swing_() {
  switch (this->swing_mode) {
    case climate::CLIMATE_SWING_VERTICAL:
    case climate::CLIMATE_SWING_BOTH:
      return GREE_VDIR_SWING;
    default:
      return GREE_VDIR_MANUAL;
  }
}

uint8_t GreeClimate::temperature_() {
  return (uint8_t) roundf(clamp<float>(this->target_temperature, GREE_TEMP_MIN, GREE_TEMP_MAX)) - GREE_TEMP_MIN;
}

uint8_t GreeClimate::preset_() {
  // YX1FF has sleep preset
  if (this->model_ == GREE_YX1FF) {
    switch (this->preset.value_or(climate::CLIMATE_PRESET_NONE)) {
      case climate::CLIMATE_PRESET_NONE:
        return GREE_PRESET_NONE;
      case climate::CLIMATE_PRESET_SLEEP:
        return GREE_PRESET_SLEEP;
      default:
        return GREE_PRESET_NONE;
    }
  }

  if (this->dred_support_ && this->preset.value_or(climate::CLIMATE_PRESET_NONE) == climate::CLIMATE_PRESET_ECO) {
    return GREE_PRESET_ECO;
  }

  return GREE_PRESET_NONE;
}

}  // namespace esphome::gree
