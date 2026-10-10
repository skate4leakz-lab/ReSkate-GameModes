#pragma once
// The overlay's side of Hall of Meat (hall_of_meat.h): from the local skater's bail until they get
// up, the bones it hurt over the world, the bail's score card in the bottom left corner, and the
// screen's red edge when a bone breaks. Drawn with skate.'s own icons, logo and UI shapes, read from
// the installed game's data (Engine/Game/Build/20260929/ui_textures.h) and placed in the overlay's
// font atlas like the chat emotes. All of it on the background draw list, under ReSkate's own menus
// and chat, taking no input.
#include <chrono>

struct ImFontAtlas;
struct ImVec2;
typedef unsigned int ImU32;

namespace dingosdk::overlay {
// Startup: starts reading the images in the background, once.
void prepare_hall_of_meat_images() noexcept;
// Each atlas the overlay builds: their room reserved first, waiting up to `wait` for a read still
// running, and filled after the atlas is built.
void reserve_hall_of_meat_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept;
void fill_hall_of_meat_images(ImFontAtlas& atlas) noexcept;
// Every presented frame: whether there is something to draw (so the overlay renders with the menu
// closed), then the drawing.
bool hall_of_meat_pending();
void draw_hall_of_meat();
// Other game pictures read with Hall of Meat's (the race checkpoint's hoop and bolt), drawn on a
// screen quad (top left, top right, bottom right, bottom left) on the background draw list, white
// tinted `tint`. False until they are in the atlas.
enum class GamePicture { checkpoint_ring, checkpoint_bolt };
bool draw_game_picture_quad(GamePicture picture, const ImVec2 (&corners)[4], ImU32 tint) noexcept;
}
