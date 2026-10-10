# Game modes in skate.'s Throwdowns menu

This page covers how ReSkate's game modes appear in, and launch from, the game's own Throwdowns
menu. It also records what is native, what is still ReSkate's own, and what has and hasn't been
tested. Status as of **v2.0.2-modes.7**.

## What a player sees

1. **The cards.** Throwdowns shows the three stock cards (S.K.A.T.E., Spot Battle, Skate Jam), then
   one card per ReSkate game mode. The row scrolls left and right.
2. **Picking a card.** This sets that mode up, with the player as its leader. The page body then
   becomes a native-style panel:
   - in setup: start, a quick play area, or cancel
   - in a game: end it (leader) or leave it
   - with no game on: join other players' open games, or set up a different mode
3. **Back and Close.** Both return to the stock page. The stock cards keep their own behaviour.

## Architecture

```
mode_rules.h (registry)          modes_card.cpp (native menu)            game_modes.cpp (runtime)
  all_modes, card_order   ──►     one private card per mode       ──►     modes::command("new", …)
  mode_key/name/tagline           panel rows from menu_view()     ◄──     modes::menu_view()
                                                                          referee, HUD, networking
```

- **Registry.** `Extension/Modes/mode_rules.h` is the single list of modes. It defines:
  - `Mode`, `all_modes`, and the menu order `card_order`
  - for each mode, `mode_key` (what `mode new` takes), `mode_name`, `mode_summary` and `mode_tagline` (the card line)

  The test `card_registry` (in `game_mode_rules`) fails if any mode is missing from `card_order`, is
  listed twice, or lacks a key, name or tagline. **To add a mode to the menu, add it to the registry;
  the card is generated from it.**
- **Native menu.** `Extension/UI/NativeMenu/modes_card.cpp` runs on the client thread from
  `tick_native_menu`, under the native model write lock.
  - It finds the Throwdowns page by its card list (three tiles whose buttons name `ThrowdownSkate`,
    `SpotBattle` and `ThrowdownerActive`).
  - For each mode, it adds a **private copy** of Spot Battle's tile, with the mode's name, tagline and a
    stock card's art (`card_art`; unknown modes fall back to the plain Throwdown mark).
  - Each card's native navigation id is cleared, and its button calls back with `card <key>`.
  - The list's clip flag is set so the row scrolls; the original value is restored on release.
- **Panel.** A private copy of the authored compact Throwdown creation panel
  (`Activities_DetailsMenu_ContentResource/*`, `TD_Page_ContentResources/*`). Its rows are native
  LabelButtons that queue `mode <verb>` actions. Those actions run through `modes::command` *after*
  the model lock is released, as the console runs them.
- **Ownership and cleanup.** Every model we create is tracked by `OwnedMenuModels` and destroyed on
  release.
  - Release happens when the page retires, on level load (`prepare_native_menu_level_load`), or after
    any exception.
  - Release restores the page's body, Back button, title, page actions, the card list (stock tiles
    only) and its clip flag.
  - A page generation that failed is not retried, so a broken card cannot loop.
  - Button descriptors are never freed; native buttons may still hold them.

The technique comes from the native 1-Up card in a friend's ReSkate fork: private UI model copies,
owned action descriptors, and exported-record lookup across UI asset domains. That fork's own 1-Up
rules, HUD and placement are **not** included.

## Mode lifecycle

ReSkate's referee phases map onto a challenge lifecycle as follows:

| Challenge state | Game modes |
|---|---|
| Registered / available | in the registry and on a card (`card_registry` test) |
| Selected → initialising | `card <key>` → `mode new <key>` (setup phase, the player leads) |
| Active | countdown → playing (referee in `mode_rules.cpp`, client in `game_modes.cpp`) |
| Completed | results (12 s), recorded on the lobby leaderboard |
| Cancelled | `mode stop` (leader) or `mode leave`; also on session or map change |
| Reset | game state dropped; the menu panel follows `menu_view()` on its next tick |

## Not native yet (the next work)

| Item | Status | What it needs |
|---|---|---|
| Gameplay HUD | ReSkate's own overlay (ImGui), not skate.'s Throwdown HUD | Port the friend's native HUD mounting (`native_hud_selection.h`, HudViewModel `0xe4c44873`, CountdownToGo) per mode |
| Placement | ReSkate's own free-camera placing and our flag | The friend's Spot Battle flag bridge (`one_up_placement.*`) |
| Per-mode art and colours | Borrowed stock art | A native texture bundle like the friend's `assets/one_up/native_assets.bin` (RS1UP001, materialised under `Mods/.reskate-core`). Their source `workspace/` is needed for the tooling. |
| "DLL exchange" | Undefined in this project | A definition from the requester; nothing by that name exists here or in the friend's handoff |

## Tested

- **Automated:** `game_mode_rules` (rules, wire, `card_registry`) passes. 38 of 40 suites pass; the
  two failures (`multiplayer_voice`, `multiplayer_mesh`) are upstream 2.0.2 checks.
- **In game: nothing yet** for the Throwdowns cards. To check:
  1. All 13 cards show and the row scrolls with the pad and the mouse.
  2. Each card sets its mode up and the panel opens.
  3. Start, cancel, end and leave work from the panel.
  4. Back and Close return to the stock page.
  5. The three stock throwdowns still work, including after leaving and reopening.
  6. Reloading the map doesn't leave cards behind or duplicate them.
  7. With 2 players: an open game shows as JOIN on the other player's panel.
- Diagnostics: every line from this feature in `logs/ReSkate.log` starts with `Game modes card`.
