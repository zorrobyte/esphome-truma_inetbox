#pragma once

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

class EmberPairingButton : public Component, public button::Button, public Parented<EmberOneControl> {
 public:
  void dump_config() override;

 protected:
  void press_action() override;
};

}  // namespace ember_onecontrol
}  // namespace esphome
