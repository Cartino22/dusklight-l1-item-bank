# L1 Item Bank

A Dusklight mod that puts a **second pair of X/Y items on L1**.

- **Toggle mode** (default): tap L1 to flip X/Y between your main pair and your second pair.
- **Hold mode**: X/Y show your second pair only while L1 is held.

Pick the mode in Dusklight's **Mods** window → **L1 Item Bank**. There's also an option to turn off the
little "Main / Second item pair" message.

It works with Twilit Essentials: your Z (R1) item is never touched, so you get X, Y, Z, plus a second X and Y.

## Setting up your second pair

1. Load your save, press L1 (the X/Y icons go blank the first time).
2. Open the item wheel and assign items to X and Y as usual. That's your second pair.
3. Press L1 again to get your main pair back.

In Hold mode, keep L1 held while you open the item wheel to edit the second pair. Each save file keeps
its own pairs.

**Switching is paused** during menus, cutscenes, conversations, wolf form, and while Link is mid-way
through using an item. In Toggle mode a press during those moments is ignored; in Hold mode the
switch happens as soon as it's allowed. In Hold mode, letting go of L1 while aiming puts the item away.

---

## Building it (one-time, about 10 minutes, free)

Dusklight mods have to be compiled for each platform. GitHub builds this for you:

1. Go to <https://github.com/new>, name the repository `l1-item-bank`, leave everything else as is,
   and click **Create repository**.
2. On the next page, click **uploading an existing file**.
3. Unzip `l1-item-bank.zip`, open the `l1-item-bank` folder, select **everything inside it**
   (including the `.github` folder) and drag it onto the GitHub page. Click **Commit changes**.
4. Click the **Actions** tab. A "Build" run starts on its own; wait for the green check (around 10
   minutes; the Windows and macOS builds are the slow ones).
5. Click the run, scroll to **Artifacts**, and download **mod-combined**. (If that one is missing,
   download **mod-windows-amd64** instead.) Unzip it to get `l1_item_bank.dusk`.

`mod-combined` works on Windows, macOS, Linux and Android, so the same file works on your phone too.

## Installing

Drag `l1_item_bank.dusk` onto the Dusklight window, or copy it into
`%APPDATA%\TwilitRealm\Dusklight\mods`. Then enable **L1 Item Bank** in the Mods window.

If the Mods window says L1 is also bound to a GameCube button, unbind L1 in **Controller Config**
(by default on a DualSense it's unbound, so you should see "L1 is free").

## Dusklight version

This is built against **Dusklight v2.0.3**. If you're on a different version, change the
`DUSKLIGHT_VERSION` line in `CMakeLists.txt` to match (e.g. `"v2.0.4"`) on GitHub (pencil icon →
**Commit changes**), and the Actions build runs again.

## Removing it

Switch back to your main pair before disabling or deleting the mod. (Disabling it from the Mods
window mid-game does this automatically.)

---

Built from [TwilitRealm/mod-template](https://github.com/TwilitRealm/mod-template). Source: `src/mod.cpp`.
