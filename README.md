# mod-quest-catchup

## Description

AzerothCore WotLK module that provides `.qcatchup` (alias `.qc`) for syncing quest progress from a targeted player to you, and `.qsync` for group-wide everyone-to-everyone sync. It's a little op/cheaty but we've all leveled alts and abhor the grind sometimes. 

### Features

- **Target-based quest sync**: Click on a player to target them, then run `.qc` to sync their completed quests and accept their in-progress quests to you
- **Group-wide quest sync (`.qsync`)**: Syncs eligible quests from every raid/party member to every other member — one command for the whole group
- **Dry-run override (`.qsync doveride`)**: Forces `.qsync` to run live even if `QuestCatchup.DryRun=1`, while still printing the detailed grouped summary
- **Eligibility filtering**: Only quests that match your level, race, and class are synced
- **One-time progression only**: Daily, weekly, monthly, seasonal, and repeatable quests are excluded to prevent duplicate rewards and XP abuse
- **Reward-aware**: Grants XP, items, and spells/abilities from the synced quests
- **Spell tracking**: Lists all spells and abilities gained from completed quests
- **Dry-run mode**: Preview eligible quests without committing any changes
- **Configurable**: Full control over behavior via `worldserver.conf`

### Requirements

- AzerothCore WotLK (3.3.5a)
- CMake build system
- A C++17 compatible compiler

### Installation

1. Place this module inside the `modules/mod-quest-catchup/` directory of your AzerothCore installation
```
git clone https://www.github.com/Jellypowered/mod-quest-catchup.git
```
2. Re-run CMake and rebuild:

```bash
cmake -B build
cmake --build build --target modules -j$(nproc)
```
or from your \<AcoreDir\> 
```
./acore.sh compiler build
``` 
3. Restart the worldserver

### Usage

#### `.qc` / `.qcatchup` — single target sync

1. **Target a player** — Click on the player's character to set them as your target
2. **Run the command** — Type `.qc` or `.qcatchup` in chat

- **With a target**: Syncs all eligible completed quests from the targeted player to you
- **Without a target**: Shows "You must select a player target to use this command."

> **Note**: The command uses your **in-game target cursor**, not the command argument. Type `.qc` while targeting the desired player — do not include the name in the command.

#### `.qsync` — group-wide sync (everyone to everyone)

1. **Be in a party or raid** — Solo players cannot use this command
2. **Run the command** — Type `.qsync` in chat

- Iterates through all group members, syncing each member's completed/in-progress quests to every other member
- Excludes daily, weekly, monthly, seasonal, and repeatable quests from catch-up
- Silent during execution (no per-quest spam)
- Prints one clean summary at the end with totals and spells gained
- Offline members are skipped silently

> **Note**: If a party member is level 80 and another is level 30, only quests eligible for the level 30 player will be synced. Eligibility is checked per-receiver, so high-level quests won't be forced onto low-level alts.

#### `.qsync doveride` — live sync even when DryRun is enabled

If `QuestCatchup.DryRun = 1` in config, you can still run a live sync with:

```txt
.qsync doveride
```

This behaves like `.qsync` with dry-run disabled, but still prints the detailed grouped summary so you can see exactly which bots were accepted/completed per quest.

#### Understanding `Total syncs`

`Total syncs` is the number of source→target comparisons performed by `.qsync`.
It is an everyone-to-everyone operation, excluding self-syncs.

Formula:

```txt
Total syncs = group size × (group size - 1)
```

Examples:

- **5-player party**: `5 × 4 = 20`
- **10-player raid**: `10 × 9 = 90`
- **25-player raid**: `25 × 24 = 600`
- **40-player raid**: `40 × 39 = 1560`

Example with a 5-player party (`A, B, C, D, E`):

```txt
A→B A→C A→D A→E
B→A B→C B→D B→E
C→A C→B C→D C→E
D→A D→B D→C D→E
E→A E→B E→C E→D
```

That is 20 total sync comparisons.

### Configuration

Copy `conf/mod-quest-catchup.conf.dist` from the module directory to your modules config folder (e.g. `<AcoreDir>/env/dist/etc/modules/mod-quest-catchup.conf`):

```ini
########################################
# QuestCatchup Configuration
########################################
#
#    QuestCatchup.Enabled
#        Description: Enable or disable the Quest Catchup module.
#                     0 = Disabled (command will not work).
#                     1 = Enabled.
#        Default:     1
#
#    QuestCatchup.DryRun
#        Description: Enable dry-run mode to preview eligible quests without committing changes.
#                     0 = Live mode (complete quests and grant rewards).
#                     1 = Dry-run mode (only list eligible quests).
#        Default:     0
#
#    QuestCatchup.LogCompleted
#        Description: Log completed quests to console.
#                     0 = Disabled.
#                     1 = Enabled.
#        Default:     1
#

QuestCatchup.Enabled = 1
QuestCatchup.DryRun = 0
QuestCatchup.LogCompleted = 1
```

### Logging

To enable detailed per-quest logging (useful for troubleshooting which quests are eligible or skipped), add this line to `worldserver.conf`:

```ini
Logger.QuestCatchup=4,Console Server
```

Log levels: `0` = off, `1` = error, `2` = warn, `3` = info, `4` = debug, `5` = trace.

### Example Output

**`.qc` — single target (live mode):**

```
Quest Catchup for Goku:
  Target has completed: 51 quests
  Target has in-progress: 7 quests
  Eligible for you: 56
  Completed: 51 quests
  Accepted in-progress: 5 quests
  Gained spells/abilities:
    - Swift White Steed (ID: 33388)
    - Ebon Gryphon (ID: 33389)
```

**`.qc` — no target selected:**

```
You must select a player target to use this command.
```

**`.qsync` — raid sync (live mode):**

```
Quest Sync (Raid of 8):
  Members: 8
  Total syncs: 56
  Quests completed: 342
  Quests accepted: 48
  Spells gained:
    - Swift White Steed (ID: 33388)
    - Ebon Gryphon (ID: 33389)
    - Mount (ID: 16843)
```

**`.qsync` — dry-run summary:**

```
Quest Sync (Raid of 40):
  Members: 40
  Total syncs: 1560
  [DRY RUN] Raw sync matches: 385
  [DRY RUN] Unique bot quest actions: 15
    Completes: 15
  [Complete]
    [5545] [A Bundle of Trouble] [Bots: Bella, Fresca, Healadin, Helen, Igothealz, Jelly, Karen, Kate, Oldman, Shockade, Shooter, Sneaky, Vendor, Vera]
    [6261] [Dungar Longdrink] [Bots: Audin]
  Set QuestCatchup.DryRun=false in worldserver.conf or use .qsync doveride to actually perform those actions.
```

**`.qsync` — solo player:**

```
You are not in a group.
```

### License

This module is licensed under the MIT License. See [LICENSE](LICENSE) for details.
