// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <utility>
#include <vector>

#include "prosperoweb/core/gamepad_mapper.h"

using namespace prosperoweb::core;

namespace {

void check_layout(Mode mode, const std::vector<std::pair<Button, Action>>& expected) {
  const GamepadMapper m;
  for (const auto& [button, action] : expected) {
    CAPTURE(static_cast<int>(button));
    CHECK(m.map(button, true, mode) == action);
    CHECK(m.map(button, false, mode) == Action::None);
  }
}

}  // namespace

TEST_SUITE("gamepad_mapper") {
  TEST_CASE("page mode") {
    check_layout(Mode::Page, {
                                 {Button::Cross, Action::Click},
                                 {Button::Circle, Action::Back},
                                 {Button::Triangle, Action::FocusAddressBar},
                                 {Button::Square, Action::OpenOsk},
                                 {Button::L1, Action::PrevTab},
                                 {Button::R1, Action::NextTab},
                                 {Button::L2, Action::PageUp},
                                 {Button::R2, Action::PageDown},
                                 {Button::Up, Action::ScrollUp},
                                 {Button::Down, Action::ScrollDown},
                                 {Button::Left, Action::ScrollLeft},
                                 {Button::Right, Action::ScrollRight},
                                 {Button::Options, Action::OpenMenu},
                                 {Button::Share, Action::Reload},
                                 {Button::L3, Action::ToggleChrome},
                                 {Button::R3, Action::ToggleFullscreen},
                                 {Button::Touchpad, Action::Click},
                                 {Button::PS, Action::None},
                             });
  }

  TEST_CASE("chrome mode") {
    check_layout(Mode::Chrome, {
                                   {Button::Up, Action::NavigateUp},
                                   {Button::Down, Action::NavigateDown},
                                   {Button::Left, Action::NavigateLeft},
                                   {Button::Right, Action::NavigateRight},
                                   {Button::Cross, Action::Activate},
                                   {Button::Circle, Action::ToggleChrome},
                                   {Button::Triangle, Action::FocusAddressBar},
                                   {Button::Square, Action::CloseTab},
                                   {Button::L1, Action::PrevTab},
                                   {Button::R1, Action::NextTab},
                                   {Button::Options, Action::OpenMenu},
                                   {Button::Share, Action::Reload},
                                   {Button::Touchpad, Action::NewTab},
                                   {Button::R3, Action::ToggleFullscreen},
                                   {Button::L2, Action::None},
                                   {Button::PS, Action::None},
                               });
  }

  TEST_CASE("osk mode") {
    check_layout(Mode::Osk, {
                                {Button::Up, Action::OskMoveUp},
                                {Button::Down, Action::OskMoveDown},
                                {Button::Left, Action::OskMoveLeft},
                                {Button::Right, Action::OskMoveRight},
                                {Button::Cross, Action::OskSelect},
                                {Button::Circle, Action::OskCancel},
                                {Button::Options, Action::OskConfirm},
                                {Button::Square, Action::OskBackspace},
                                {Button::Triangle, Action::OskSpace},
                                {Button::L2, Action::OskShift},
                                {Button::L1, Action::OskCaretLeft},
                                {Button::R1, Action::OskCaretRight},
                                {Button::Touchpad, Action::OskNextLayout},
                                {Button::R2, Action::None},
                                {Button::PS, Action::None},
                            });
  }

  TEST_CASE("osk caret and layout buttons do not change other modes") {
    const GamepadMapper m;
    CHECK(m.map(Button::L1, true, Mode::Page) == Action::PrevTab);
    CHECK(m.map(Button::R1, true, Mode::Page) == Action::NextTab);
    CHECK(m.map(Button::Touchpad, true, Mode::Page) == Action::Click);
    CHECK(m.map(Button::L1, true, Mode::Chrome) == Action::PrevTab);
    CHECK(m.map(Button::Touchpad, true, Mode::Chrome) == Action::NewTab);
    CHECK(m.map(Button::L2, true, Mode::Osk) == Action::OskShift);
  }
}
