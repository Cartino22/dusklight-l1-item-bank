// L1 Item Bank: a second set of X/Y/Z items, switched with L1.
//
// The mod never adds a new button slot. It keeps a second (X, Y, Z) set and swaps it into the
// game's own select-item slots, so the HUD, the item wheel, combo items (bomb arrows, etc.) and
// ammo counters all keep working through the game's normal code. Z is select index 2, which is
// where Twilit Essentials' Custom Z Button (and Dawnlight's Z slot) keep the third item.
//
// Persistence: both sets are written to the save slot only when the game itself writes a save
// (snapshot taken when the save data is captured, stored once the slot is known), so the stored
// sets always match what the card holds. On load, the live X/Y/Z are matched against the stored
// sets to work out which set is on the buttons.

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/save.h"
#include "mods/svc/ui.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "m_Do/m_Do_controller_pad.h"

#include <dolphin/pad.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(SaveService, svc_save);
IMPORT_SERVICE(UiService, svc_ui);

// Runs when the game copies its state into a save buffer (manual saves and autosave).
DEFINE_HOOK(&dSv_info_c::memory_to_card, MemoryToCard);

namespace {

// ---------------------------------------------------------------------------------------------
// Constants

constexpr int kBankSlots = 3;        // X, Y and Z (select indexes 0, 1 and 2)
constexpr int kLegacyBankSlots = 2;  // v2 saves stored X and Y only
constexpr u8 kEmptySlot = 0xFF;      // "no item" for select/mix slot indexes
constexpr int kSdlLeftShoulder = 9;  // SDL3 SDL_GAMEPAD_BUTTON_LEFT_SHOULDER (L1 / LB)
constexpr u32 kModeAiming = 0x20000000;  // set by the bow/clawshot/boomerang/... aiming procs
constexpr u32 kModeItemsUsable = 0x4;    // the state where X/Y may change Link's held item
constexpr const char* kBlobName = "pairs";
constexpr uint32_t kBlobMagic = 0x4C314942;  // "L1IB"
constexpr uint32_t kBlobVersion = 3;
constexpr uint32_t kLegacyBlobVersion = 2;

enum Mode : int64_t {
    MODE_TOGGLE = 0,
    MODE_HOLD = 1,
};

const char* const kModeOptions[] = {"Toggle (tap L1)", "Hold (while L1 is held)"};

// ---------------------------------------------------------------------------------------------
// State

struct Pair {
    u8 select[kBankSlots] = {kEmptySlot, kEmptySlot, kEmptySlot};
    u8 mix[kBankSlots] = {kEmptySlot, kEmptySlot, kEmptySlot};

    bool operator==(const Pair& other) const {
        for (int i = 0; i < kBankSlots; ++i) {
            if (select[i] != other.select[i] || mix[i] != other.mix[i]) {
                return false;
            }
        }
        return true;
    }
};

struct Snapshot {
    Pair main;
    Pair second;
    bool secondActive = false;
};

struct PairsBlob {
    uint32_t magic;
    uint32_t version;
    uint8_t secondActive;
    uint8_t mainSelect[kBankSlots];
    uint8_t mainMix[kBankSlots];
    uint8_t secondSelect[kBankSlots];
    uint8_t secondMix[kBankSlots];
    uint8_t reserved[3];
};
static_assert(sizeof(PairsBlob) == 24, "PairsBlob layout changed");

// Version 1.0.0 of the mod stored X and Y only.
struct LegacyPairsBlob {
    uint32_t magic;
    uint32_t version;
    uint8_t secondActive;
    uint8_t mainSelect[kLegacyBankSlots];
    uint8_t mainMix[kLegacyBankSlots];
    uint8_t secondSelect[kLegacyBankSlots];
    uint8_t secondMix[kLegacyBankSlots];
    uint8_t reserved[3];
};
static_assert(sizeof(LegacyPairsBlob) == 20, "LegacyPairsBlob layout changed");

Pair s_stored;               // the pair currently sitting outside the game's X/Y slots
bool s_secondActive = false; // true while the second set is on the buttons
bool s_stateLoaded = false;  // our state matches the active save slot
bool s_needSync = true;      // re-match the live X/Y against the save slot once Link exists

Snapshot s_pendingSave;      // captured with the game's save data, stored once it's written
bool s_hasPendingSave = false;
int s_pendingSaveSlot = -1;  // the file the captured save data is going to

bool s_l1WasDown = false;     // previous frame's L1, for edge detection
bool s_l1Suppressed = false;  // L1 went down while blocked; ignore until released

ConfigVarHandle s_modeVar = 0;
ConfigVarHandle s_notifyVar = 0;
SaveObserverHandle s_saveObserver = 0;

using GetGamepadButtonFn = bool (*)(SDL_Gamepad*, int);
GetGamepadButtonFn s_getGamepadButton = nullptr;

// ---------------------------------------------------------------------------------------------
// Config

Mode current_mode() {
    int64_t value = MODE_TOGGLE;
    if (s_modeVar != 0) {
        svc_config->get_int(mod_ctx, s_modeVar, &value);
    }
    return value == MODE_HOLD ? MODE_HOLD : MODE_TOGGLE;
}

bool notify_enabled() {
    bool value = true;
    if (s_notifyVar != 0) {
        svc_config->get_bool(mod_ctx, s_notifyVar, &value);
    }
    return value;
}

// ---------------------------------------------------------------------------------------------
// Game slots

Pair read_live_pair() {
    Pair pair;
    for (int i = 0; i < kBankSlots; ++i) {
        pair.select[i] = dComIfGs_getSelectItemIndex(i);
        pair.mix[i] = dComIfGs_getMixItemIndex(i);
    }
    return pair;
}

u8 item_in_slot(u8 slot) {
    return slot == kEmptySlot ? static_cast<u8>(dItemNo_NONE_e) : dComIfGs_getItem(slot, false);
}

bool is_bomb(u8 item) {
    return item == dItemNo_NORMAL_BOMB_e || item == dItemNo_WATER_BOMB_e ||
           item == dItemNo_POKE_BOMB_e;
}

// The combinations the game itself accepts (dSv_player_item_c::getItem).
bool is_valid_combo(u8 a, u8 b) {
    const auto pairs = [a, b](u8 base, bool (*partner)(u8)) {
        return (a == base && partner(b)) || (b == base && partner(a));
    };
    return pairs(dItemNo_BOW_e,
               [](u8 item) { return is_bomb(item) || item == dItemNo_HAWK_EYE_e; }) ||
           pairs(dItemNo_FISHING_ROD_1_e, [](u8 item) {
               return item == dItemNo_BEE_CHILD_e || item == dItemNo_ZORAS_JEWEL_e ||
                      item == dItemNo_WORM_e;
           });
}

// Inventory can change while a pair is stored (bombs run out, bait gets used). The game only
// repairs the live X/Y, so check a stored pair before it goes back on the buttons. An invalid
// combo would otherwise trip the game's "uncertain combination" error path.
Pair sanitize(Pair pair) {
    for (int i = 0; i < kBankSlots; ++i) {
        if (item_in_slot(pair.select[i]) == dItemNo_NONE_e) {
            pair.select[i] = kEmptySlot;
        }
        if (pair.mix[i] == kEmptySlot) {
            continue;
        }
        const u8 selectItem = item_in_slot(pair.select[i]);
        const u8 mixItem = item_in_slot(pair.mix[i]);
        if (pair.select[i] != kEmptySlot && is_valid_combo(selectItem, mixItem)) {
            continue;
        }
        // Same repair the game makes when a bomb bag runs dry: keep the bow (or rod) on the
        // button and drop the combination.
        if (mixItem == dItemNo_BOW_e || mixItem == dItemNo_FISHING_ROD_1_e) {
            pair.select[i] = pair.mix[i];
        }
        pair.mix[i] = kEmptySlot;
    }
    return pair;
}

void write_live_pair(const Pair& pair) {
    // Write every slot first, so nothing that reacts to a slot change (the HUD, Twilit
    // Essentials' duplicate-item handling) ever sees a half-swapped set, e.g. the new X next to
    // the old Y.
    dSv_player_status_a_c& status = g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();
    for (int i = 0; i < kBankSlots; ++i) {
        status.setMixItemIndex(i, pair.mix[i]);
        status.setSelectItemIndex(i, pair.select[i]);
    }
    // Then commit through the same call the item wheel uses, so the play-side items and any
    // mod hooks on it (Twilit Essentials' Z button) update as they would after a normal assign.
    for (int i = 0; i < kBankSlots; ++i) {
        dComIfGs_setSelectItemIndex(i, pair.select[i]);
    }
}

Snapshot snapshot() {
    const Pair live = read_live_pair();
    Snapshot snap;
    snap.secondActive = s_secondActive;
    snap.main = s_secondActive ? s_stored : live;
    snap.second = s_secondActive ? live : s_stored;
    return snap;
}

// ---------------------------------------------------------------------------------------------
// Save persistence

void store_snapshot(const Snapshot& snap) {
    PairsBlob blob{};
    blob.magic = kBlobMagic;
    blob.version = kBlobVersion;
    blob.secondActive = snap.secondActive ? 1 : 0;
    for (int i = 0; i < kBankSlots; ++i) {
        blob.mainSelect[i] = snap.main.select[i];
        blob.mainMix[i] = snap.main.mix[i];
        blob.secondSelect[i] = snap.second.select[i];
        blob.secondMix[i] = snap.second.mix[i];
    }
    if (svc_save->set_blob(mod_ctx, kBlobName, &blob, sizeof(blob)) != MOD_OK) {
        svc_log->warn(mod_ctx, "could not store the item sets with this save");
    }
}

// Works out which pair is on X/Y right now, using the pairs stored with the save.
void sync_with_save() {
    const Pair live = read_live_pair();
    Pair main;
    Pair second;
    bool savedSecondActive = false;

    PairsBlob blob{};
    size_t size = sizeof(blob);
    const bool haveBlob = svc_save->get_blob(mod_ctx, kBlobName, &blob, &size) == MOD_OK;
    if (haveBlob && size == sizeof(PairsBlob) && blob.magic == kBlobMagic &&
        blob.version == kBlobVersion)
    {
        for (int i = 0; i < kBankSlots; ++i) {
            main.select[i] = blob.mainSelect[i];
            main.mix[i] = blob.mainMix[i];
            second.select[i] = blob.secondSelect[i];
            second.mix[i] = blob.secondMix[i];
        }
        savedSecondActive = blob.secondActive != 0;
    } else if (haveBlob && size == sizeof(LegacyPairsBlob)) {
        // Saved by 1.0.0, which left Z alone: both sets start out sharing the current Z item.
        LegacyPairsBlob legacy{};
        std::memcpy(&legacy, &blob, sizeof(legacy));
        if (legacy.magic != kBlobMagic || legacy.version != kLegacyBlobVersion) {
            s_secondActive = false;
            s_stored = Pair{};
            return;
        }
        for (int i = 0; i < kLegacyBankSlots; ++i) {
            main.select[i] = legacy.mainSelect[i];
            main.mix[i] = legacy.mainMix[i];
            second.select[i] = legacy.secondSelect[i];
            second.mix[i] = legacy.secondMix[i];
        }
        for (int i = kLegacyBankSlots; i < kBankSlots; ++i) {
            main.select[i] = second.select[i] = live.select[i];
            main.mix[i] = second.mix[i] = live.mix[i];
        }
        savedSecondActive = legacy.secondActive != 0;
    } else {
        // A file that has never been saved with the mod: the buttons hold the main set.
        s_secondActive = false;
        s_stored = Pair{};
        return;
    }
    const Pair& savedActive = savedSecondActive ? second : main;
    const Pair& savedOther = savedSecondActive ? main : second;

    if (live == savedActive) {
        s_secondActive = savedSecondActive;
        s_stored = savedOther;
    } else if (live == savedOther) {
        // E.g. the mod was disabled mid-game and put the other pair back.
        s_secondActive = !savedSecondActive;
        s_stored = savedActive;
    } else {
        // X/Y were edited since that save (without the mod running): they belong to the pair
        // that was on the buttons, and the other pair is unchanged.
        s_secondActive = savedSecondActive;
        s_stored = savedOther;
    }
}

void on_new_save(ModContext*, uint32_t, void*) {
    s_secondActive = false;
    s_stored = Pair{};
    s_needSync = false;
    s_stateLoaded = true;
    s_hasPendingSave = false;
}

void on_save_loaded(ModContext*, uint32_t, void*) {
    s_needSync = true;
    s_stateLoaded = false;
    s_hasPendingSave = false;
}

void on_save_written(ModContext*, uint32_t slot, void*) {
    // The slot that was just written is now the current slot, even when the player saved to a
    // different file than the one they loaded.
    if (s_hasPendingSave && static_cast<int>(slot) == s_pendingSaveSlot) {
        store_snapshot(s_pendingSave);
        s_hasPendingSave = false;
    }
}

HookAction before_memory_to_card(ModContext*, void* args, void*, void*) {
    if (s_stateLoaded) {
        s_pendingSave = snapshot();
        s_pendingSaveSlot = mods::arg<int>(args, 2);  // (this, card_ptr, dataNum)
        s_hasPendingSave = true;
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Input

bool l1_down() {
    if (s_getGamepadButton == nullptr) {
        return false;
    }
    const s32 index = PADGetIndexForPort(PAD_1);
    if (index < 0) {
        return false;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
    return gamepad != nullptr && s_getGamepadButton(gamepad, kSdlLeftShoulder);
}

// Which GameCube button (if any) L1 is also bound to in Dusklight's controller config.
const char* l1_conflict_name() {
    u32 count = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(PAD_1, &count);
    if (mappings == nullptr) {
        return nullptr;
    }
    for (u32 i = 0; i < count; ++i) {
        if (mappings[i].nativeButton == static_cast<u32>(kSdlLeftShoulder)) {
            return PADGetButtonName(mappings[i].padButton);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// When swapping is allowed

// True when swapping X/Y now could surprise the game or the player: menus, cutscenes,
// conversations, minigames with rental items, wolf form, or Link mid-way through using an item.
bool swap_blocked() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || dComIfGp_getLinkPlayer() == nullptr) {
        return true;
    }

    bool uiVisible = false;
    if (svc_ui->is_any_document_visible(mod_ctx, &uiVisible) != MOD_OK || uiVisible) {
        return true;  // Dusklight's own menus
    }

    if (dComIfGp_isPauseFlag() || dMeter2Info_getPauseStatus() != 0 ||
        dMeter2Info_getWindowStatus() != 0 ||        // item wheel, collection, map, ...
        dComIfGp_event_runCheck() ||                 // cutscenes and scripted events
        dMeter2Info_isShopTalkFlag() || dMsgObject_isTalkNowCheck() ||
        dMeter2Info_getMiniGameItemSetFlag() != 0)  // minigames that lend X/Y items
    {
        return true;
    }

    if (link->checkWolf()) {
        return true;
    }

    // Iron boots can only be taken off with the button they're on.
    if (link->checkEquipHeavyBoots()) {
        return true;
    }

    // With an item out, only swap in the state where the game itself lets X/Y change Link's
    // held item (daAlink_c::checkItemChangeFromButton), and never while aiming: shots take their
    // ammo from whatever is on the button at that moment.
    if (link->mEquipItem != dItemNo_NONE_e) {
        const bool itemChangeAllowed = link->checkModeFlg(kModeItemsUsable) &&
                                       !link->checkModeFlg(kModeAiming) &&
                                       !link->checkEquipAnime() &&
                                       !link->checkBoomerangThrowAnime() &&
                                       !link->checkCopyRodThrowAnime();
        if (!itemChangeAllowed) {
            return true;
        }
    }
    return false;
}

// Looser test for putting the main pair back when the mod is disabled. That always happens from
// Dusklight's Mods window, so the menu check above would never pass; what matters here is that
// the item wheel isn't holding its own copy of X/Y and Link isn't mid-way through using an item.
bool restore_allowed() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || dMeter2Info_getWindowStatus() != 0 ||
        dMeter2Info_getMiniGameItemSetFlag() != 0 || link->checkEquipHeavyBoots() ||
        dComIfGp_event_runCheck() || dMsgObject_isTalkNowCheck() || dMeter2Info_isShopTalkFlag())
    {
        return false;
    }
    if (link->mEquipItem != dItemNo_NONE_e &&
        (link->checkModeFlg(kModeAiming) || link->checkEquipAnime() ||
            link->checkBoomerangThrowAnime() || link->checkCopyRodThrowAnime()))
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Swapping

void notify_switch() {
    if (!notify_enabled()) {
        return;
    }
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.title_rml = "L1 Item Bank";
    toast.body_rml = s_secondActive ? "Second item set" : "Main item set";
    toast.duration_ms = 1200;
    svc_ui->push_toast(mod_ctx, &toast);
}

void swap_banks(bool notify) {
    const Pair live = read_live_pair();
    write_live_pair(sanitize(s_stored));
    s_stored = live;
    s_secondActive = !s_secondActive;
    if (notify) {
        notify_switch();
    }
}

// ---------------------------------------------------------------------------------------------
// Settings panel (Mods window → L1 Item Bank)

UiElementHandle s_conflictText = 0;
UiElementHandle s_statusText = 0;

void refresh_panel_text() {
    if (s_conflictText != 0) {
        const char* conflict = l1_conflict_name();
        char text[160];
        if (conflict != nullptr) {
            std::snprintf(text, sizeof(text),
                "Heads up: L1 is also bound to GameCube \"%s\" in Controller Config. "
                "Unbind it there, or both will fire.",
                conflict);
        } else {
            std::snprintf(text, sizeof(text), "L1 is free in Controller Config.");
        }
        svc_ui->elem_set_text(mod_ctx, s_conflictText, text);
    }
    if (s_statusText != 0) {
        const char* status = !s_stateLoaded  ? "Load a save to use the second set."
                             : s_secondActive ? "Second set is on X/Y/Z right now."
                                              : "Main set is on X/Y/Z right now.";
        svc_ui->elem_set_text(mod_ctx, s_statusText, status);
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    s_conflictText = 0;
    s_statusText = 0;

    svc_ui->pane_add_section(mod_ctx, panel, "L1 Item Bank");

    UiControlDesc mode = UI_CONTROL_DESC_INIT;
    mode.kind = UI_CONTROL_DROPDOWN;
    mode.label = "L1 mode";
    mode.binding = UI_BINDING_CONFIG_VAR;
    mode.config_var = s_modeVar;
    mode.options = kModeOptions;
    mode.option_count = sizeof(kModeOptions) / sizeof(kModeOptions[0]);
    mode.tooltip = "Toggle: tap L1 to flip sets. Hold: second set only while L1 is held.";
    ModResult result = svc_ui->pane_add_control(mod_ctx, panel, &mode, nullptr);
    if (result != MOD_OK) {
        return result;
    }

    UiControlDesc notify = UI_CONTROL_DESC_INIT;
    notify.kind = UI_CONTROL_TOGGLE;
    notify.label = "Show a message when switching";
    notify.binding = UI_BINDING_CONFIG_VAR;
    notify.config_var = s_notifyVar;
    result = svc_ui->pane_add_control(mod_ctx, panel, &notify, nullptr);
    if (result != MOD_OK) {
        return result;
    }

    svc_ui->pane_add_section(mod_ctx, panel, "How to use");
    svc_ui->pane_add_text(mod_ctx, panel,
        "Switch to the second set with L1, then open the item wheel and assign items to X, Y "
        "and Z as usual. Those become your second set. Switch back and your main set returns. "
        "In Hold mode, keep L1 held while opening the item wheel to edit the second set. "
        "Both sets are kept with each save file when you save.",
        nullptr);
    svc_ui->pane_add_text(mod_ctx, panel,
        "Switching waits while you aim, use an item, or are in a menu or cutscene.", nullptr);

    svc_ui->pane_add_section(mod_ctx, panel, "Status");
    svc_ui->pane_add_text(mod_ctx, panel, "", &s_statusText);
    svc_ui->pane_add_text(mod_ctx, panel, "", &s_conflictText);
    refresh_panel_text();
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    refresh_panel_text();
    return MOD_OK;
}

ModResult register_config() {
    ConfigVarDesc mode = CONFIG_VAR_DESC_INIT;
    mode.name = "mode";
    mode.type = CONFIG_VAR_INT;
    mode.default_int = MODE_TOGGLE;
    ModResult result = svc_config->register_var(mod_ctx, &mode, &s_modeVar);
    if (result != MOD_OK) {
        return result;
    }

    ConfigVarDesc notify = CONFIG_VAR_DESC_INIT;
    notify.name = "notify";
    notify.type = CONFIG_VAR_BOOL;
    notify.default_bool = true;
    return svc_config->register_var(mod_ctx, &notify, &s_notifyVar);
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    void* address = nullptr;
    if (svc_hook->resolve(mod_ctx, "SDL_GetGamepadButton", &address, nullptr) != MOD_OK ||
        address == nullptr)
    {
        return mods::set_error(error, MOD_UNAVAILABLE, "could not find SDL_GetGamepadButton");
    }
    s_getGamepadButton = reinterpret_cast<GetGamepadButtonFn>(address);

    ModResult result = register_config();
    if (result != MOD_OK) {
        return mods::set_error(error, result, "could not register settings");
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "could not register settings panel");
    }

    result = mods::hook::add_pre<MemoryToCard>(before_memory_to_card);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "could not hook the game's save routine");
    }

    result = svc_save->observe_saves(
        mod_ctx, on_new_save, on_save_loaded, on_save_written, nullptr, &s_saveObserver);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "could not observe saves");
    }

    // observe_saves does not replay a slot that was already loaded before the mod was enabled,
    // so match against the current slot once Link exists.
    s_needSync = true;
    s_stateLoaded = false;

    // Don't treat an L1 that is already down (e.g. while enabling the mod) as a fresh press.
    s_l1WasDown = l1_down();
    s_l1Suppressed = s_l1WasDown;

    svc_log->info(mod_ctx, "L1 Item Bank ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    const bool down = l1_down();
    const bool pressed = down && !s_l1WasDown;
    s_l1WasDown = down;

    if (daAlink_getAlinkActorClass() == nullptr) {
        return MOD_OK;  // title screen, file select, or between scenes
    }

    if (s_needSync) {
        sync_with_save();
        s_needSync = false;
        s_stateLoaded = true;
    }
    if (!s_stateLoaded) {
        return MOD_OK;
    }

    const bool blocked = swap_blocked();

    if (current_mode() == MODE_TOGGLE) {
        // A press that started in a menu or cutscene is ignored until L1 is released.
        if (blocked && down) {
            s_l1Suppressed = true;
        }
        if (!down) {
            s_l1Suppressed = false;
        }
        if (pressed && !blocked && !s_l1Suppressed) {
            swap_banks(true);
        }
    } else {
        // Hold: the second pair is wanted exactly while L1 is down. Apply changes only when it is
        // safe; if L1 was released while blocked, the main pair comes back once it's allowed.
        if (down != s_secondActive && !blocked) {
            swap_banks(true);
        }
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // Disabling the mod mid-game: put the main pair back on X/Y if it's safe to. If not, the
    // pairs stored with the save still let a later session work out which pair is showing.
    if (s_stateLoaded && s_secondActive && restore_allowed()) {
        swap_banks(false);
        // Record that the main pair is back, so a later session matches pairs correctly even if
        // X/Y get edited while the mod is off.
        store_snapshot(snapshot());
    }
    if (s_saveObserver != 0) {
        svc_save->unobserve_saves(mod_ctx, s_saveObserver);
        s_saveObserver = 0;
    }
    return MOD_OK;
}

}  // extern "C"
