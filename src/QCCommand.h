/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef QC_COMMAND_H
#define QC_COMMAND_H

#include "Chat.h"
#include <set>

namespace QC
{
    bool Enabled;
    bool DryRun;
    bool LogCompleted;

    void LoadConfig(bool reload);

    bool IsQuestEligible(Quest const* quest, Player const* player);

    struct SyncStats
    {
        uint32 completed = 0;
        uint32 accepted = 0;
        uint32 dryRunTotal = 0;
        std::set<uint32> rewardSpellIds;
    };

    SyncStats SyncQuestsToPlayer(Player* source, Player* target);
    bool HandleCatchupCommand(ChatHandler* handler);
}

void AddQCCommandScripts();

#endif
