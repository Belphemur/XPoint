#include <FreeInkApp.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace fui = freeink::ui;

namespace {

class NullDrawTarget final : public fui::DrawTarget {
 public:
  fui::Rect clipRect() const override { return {0, 0, 100, 100}; }
  bool setClipRect(fui::Rect) override { return false; }
  fui::Size measureText(fui::FontId, const char*, fui::TextStyle) const override { return {10, 10}; }
  int16_t lineHeight(fui::FontId) const override { return 10; }
  void fill(fui::Rect, fui::Paint, uint8_t = 0, uint8_t = fui::CornersAll) override {}
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t = 0, uint8_t = fui::CornersAll) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect, const char*, fui::TextStyle) override {}
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint = fui::Paint::solid(fui::Color::Black),
              fui::Rotation = fui::Rotation::None) override {}
};

// ReaderToolbarUi registers ACTION_DISMISS..ACTION_FONT_ROW as a contiguous
// block. The shared UiApp capacity must hold all nine slots; before the
// capacity fix, the last three registrations were silently dropped, so the
// quick sheet's +/- and Family row dispatched no handler.
TEST(UiAppHandlersTest, ReaderToolbarActionBlockDispatchesLastHandler) {
  NullDrawTarget target;
  fui::DeviceContext device;
  device.width = 100;
  device.height = 100;

  fui::FreeInkApp<24, 10> app(target, device);
  std::array<int, 9> hits{};
  auto onAction = [](const fui::ActionEvent& event, void* user) {
    auto* seen = static_cast<std::array<int, 9>*>(user);
    if (event.action >= 1 && event.action <= 9) ++(*seen)[event.action - 1];
  };
  for (fui::ActionId action = 1; action <= 9; ++action) {
    app.on(action, onAction, &hits);
  }

  app.setScreen(
      [](fui::FreeInkApp<24, 10>::ScreenType& screen, void*) {
        for (fui::ActionId action = 1; action <= 9; ++action) {
          const int index = static_cast<int>(action) - 1;
          const int16_t x = static_cast<int16_t>((index % 3) * 10);
          const int16_t y = static_cast<int16_t>((index / 3) * 10);
          screen.frame().hit(fui::Rect{x, y, 10, 10}, action, 0, fui::InputTouch);
        }
      },
      nullptr);
  app.render(fui::InputSnapshot{});

  fui::InputSnapshot tap{};
  tap.touchReleased = true;
  tap.touchX = 25;
  tap.touchY = 25;
  const auto event = app.route(tap);

  EXPECT_EQ(event.action, 9);
  for (int action = 1; action <= 9; ++action) {
    EXPECT_EQ(hits[action - 1], action == 9 ? 1 : 0) << "action " << action;
  }
}

}  // namespace
