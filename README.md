# mod-quest-catchup

## Description

AzerothCore WotLK module that provides a `.qcatchup` (alias `.qc`) command for quickly syncing quest progress from a targeted player (or self) to the executing player.

### Features

- **Target-based quest sync**: Click on a player to target them, then run `.qc` to sync their completed quests and accept their in-progress quests to you
- **Eligibility filtering**: Only quests that match your level, race, and class are synced
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
2. Re-run CMake and rebuild:

```bash
cmake -B build
cmake --build build --target modules -j$(nproc)
```

3. Restart the worldserver

### Usage

1. **Target a player** — Click on the player's character to set them as your target
2. **Run the command** — Type `.qc` or `.qcatchup` in chat

- **With a target**: Syncs all eligible completed quests from the targeted player to you
- **Without a target**: Syncs your own completed quests to yourself (useful for re-granting rewards)

> **Note**: The command uses your **in-game target cursor**, not the command argument. Type `.qc` while targeting the desired player — do not include the name in the command.

### Configuration

Copy `conf/mod-quest-catchup.conf.dist` from the module directory to your modules config folder (e.g. `conf/modules/mod-quest-catchup.conf`):

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

**In-game chat (dry-run):**

```
Quest Catchup for Goku:
  Target has completed: 51 quests
  Target has in-progress: 7 quests
  Eligible for you: 56
  Already have: 0
  [DRY RUN] Would complete/accept: 56 quests
  Spells/Abilities: 
    - Swift White Steed (ID: 33388)
    - Ebon Gryphon (ID: 33389)
  Set QuestCatchup.DryRun=false in worldserver.conf to actually complete them.
```

**In-game chat (live mode):**

```
Quest Catchup for Goku:
  Target has completed: 51 quests
  Target has in-progress: 7 quests
  Eligible for you: 56
  Already have: 0
  Completed: 51 quests
  Accepted in-progress: 5 quests
  Gained spells/abilities:
    - Swift White Steed (ID: 33388)
    - Ebon Gryphon (ID: 33389)
```

**Server console (live mode):**

```
=== Quest Catchup: Goku ===
  Target Goku completed: 62 quests
  Target Goku in-progress: 7 quests
  Eligible for you: 55 quests
  Already have: 0 quests
  Actually completed: 55 quests
  Accepted in-progress: 5 quests
```

Both modes list spells/abilities in the in-game chat output.

### License

This module is licensed under the MIT License. See [LICENSE](LICENSE) for details.
