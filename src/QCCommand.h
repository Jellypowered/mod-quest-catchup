/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef QC_COMMAND_H
#define QC_COMMAND_H

#include "Chat.h"

namespace QC
{
    bool Enabled;
    bool DryRun;
    bool LogCompleted;

    void LoadConfig(bool reload);

    bool IsQuestEligible(Quest const* quest, Player const* player);
    bool HandleCatchupCommand(ChatHandler* handler);
}

void AddQCCommandScripts();

#endif
