/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "QCCommand.h"
#include "ChatCommand.h"

// ===========================================================================
// Configuration
// ===========================================================================

namespace QC
{

    void LoadConfig(bool reload)
    {
        Enabled = sConfigMgr->GetOption<bool>("QuestCatchup.Enabled", true);
        DryRun = sConfigMgr->GetOption<bool>("QuestCatchup.DryRun", false);
        LogCompleted = sConfigMgr->GetOption<bool>("QuestCatchup.LogCompleted", true);

        if (reload)
            LOG_DEBUG("module.QuestCatchup", "Configuration reloaded. Enabled={}, DryRun={}, LogCompleted={}", Enabled, DryRun, LogCompleted);
        else
            LOG_DEBUG("module.QuestCatchup", "Configuration loaded. Enabled={}, DryRun={}, LogCompleted={}", Enabled, DryRun, LogCompleted);
    }

    bool IsQuestEligible(Quest const* quest, Player const* player)
    {
        if (!quest)
            return false;

        // Gate 1: Level check
        if (quest->GetMinLevel() > 0 && quest->GetMinLevel() > player->GetLevel())
            return false;

        // Gate 2: Race check (0 = any race allowed)
        uint32 allowableRaces = quest->GetAllowableRaces();
        if (allowableRaces && !(allowableRaces & (1 << (player->getRace() - 1))))
            return false;

        // Gate 3: Class check (0 = any class allowed)
        uint32 requiredClasses = quest->GetRequiredClasses();
        if (requiredClasses && !(requiredClasses & (1 << (player->getClass() - 1))))
            return false;

        return true;
    }

    bool HandleCatchupCommand(ChatHandler* handler)
    {
        if (!Enabled)
        {
            handler->SendSysMessage("Quest Catchup module is currently disabled. Contact an administrator.");
            return true;
        }

        Player* source = handler->GetSession()->GetPlayer();
        if (!source)
            return true;

        Player* target = handler->getSelectedPlayer();
        if (!target)
            target = source;

        if (source != target && handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return true;

        // Collect target's completed quests
        std::set<uint32> completedQuests(target->getRewardedQuests().begin(), target->getRewardedQuests().end());

        // Collect target's in-progress quests
        std::set<uint32> inProgressQuests;
        for (auto const& [questId, statusData] : target->getQuestStatusMap())
        {
            if (statusData.Status == QUEST_STATUS_INCOMPLETE)
                inProgressQuests.insert(questId);
        }

        // Count categories
        uint32 targetCompletedCount = 0;
        uint32 targetInProgressCount = 0;
        uint32 sourceHasCount = 0;
        uint32 completedTotal = 0;
        uint32 dryRunTotal = 0;
        uint32 acceptedTotal = 0;
        std::set<uint32> rewardSpellIds;    // Collect reward spell IDs for display

        // Sync only quests the target has directly completed
        for (uint32 questId : completedQuests)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Skip if source already has this quest (completed, rewarded, or in progress)
            if (source->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            {
                ++sourceHasCount;
                continue;
            }

            // Skip if source cannot do this quest
            if (!IsQuestEligible(quest, source))
                continue;

            ++targetCompletedCount;

            // Collect reward spells from quest template (WotLK: single spell + cast spell)
            if (uint32 displaySpell = quest->GetRewSpell())
                rewardSpellIds.insert(displaySpell);
            if (int32 castSpell = quest->GetRewSpellCast())
                rewardSpellIds.insert(static_cast<uint32>(castSpell));

            if (DryRun)
            {
                if (LogCompleted)
                    LOG_DEBUG("module.QuestCatchup", "Dry-run: Would complete quest %u \"%s\"",
                        questId, quest->GetTitle().c_str());
                ++dryRunTotal;
                continue;
            }

            // Complete the quest (sets status to COMPLETE)
            source->CompleteQuest(questId);

            // Grant rewards (XP, items, spells, etc.)
            source->RewardQuest(quest, 0, source, false);

            ++completedTotal;

            // Log if configured
            if (LogCompleted)
                LOG_DEBUG("module.QuestCatchup", "Completed quest %u \"%s\" for player %s",
                    questId, quest->GetTitle().c_str(), source->GetName().c_str());
        }

        // Accept in-progress quests that the source doesn't already have
        for (uint32 questId : inProgressQuests)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Skip if source already has this quest
            if (source->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            {
                ++sourceHasCount;
                continue;
            }

            // Skip if source cannot take this quest
            if (!IsQuestEligible(quest, source))
                continue;

            // Skip if source cannot add it (log full, source item won't fit, etc.)
            if (!source->CanAddQuest(quest, false))
                continue;

            ++targetInProgressCount;

            if (DryRun)
            {
                if (LogCompleted)
                    LOG_DEBUG("module.QuestCatchup", "Dry-run: Would accept quest %u \"%s\"", questId, quest->GetTitle().c_str());
                ++dryRunTotal;
                continue;
            }

            // Accept the quest for source (gives source item, updates quest log, etc.)
            source->AddQuest(quest, source);
            ++acceptedTotal;

            // Log if configured
            if (LogCompleted)
                LOG_DEBUG("module.QuestCatchup", "Accepted quest %u \"%s\" for player %s",
                    questId, quest->GetTitle().c_str(), source->GetName().c_str());
        }

        // Print summary to chat (always visible to player)
        handler->PSendSysMessage("Quest Catchup for {}:", source->GetName().c_str());
        handler->PSendSysMessage("  Target has completed: {} quests", completedQuests.size());
        if (!inProgressQuests.empty())
            handler->PSendSysMessage("  Target has in-progress: {} quests", inProgressQuests.size());
        handler->PSendSysMessage("  Eligible for you: {}", targetCompletedCount + targetInProgressCount);
        handler->PSendSysMessage("  Already have: {}", sourceHasCount);
        if (DryRun)
        {
            handler->PSendSysMessage("  [DRY RUN] Would complete/accept: {} quests", dryRunTotal);
            if (!rewardSpellIds.empty())
            {
                handler->PSendSysMessage("  Spells/Abilities: ");
                for (uint32 spellId : rewardSpellIds)
                {
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                        handler->PSendSysMessage("    - {} (ID: {})", spell->SpellName[0], spellId);
                }
            }
            handler->PSendSysMessage("  Set QuestCatchup.DryRun=false in worldserver.conf to actually complete them.");
        }
        else
        {
            handler->PSendSysMessage("  Completed: {} quests", completedTotal);
            if (acceptedTotal > 0)
                handler->PSendSysMessage("  Accepted in-progress: {} quests", acceptedTotal);
            if (!rewardSpellIds.empty())
            {
                handler->PSendSysMessage("  Gained spells/abilities: ");
                for (uint32 spellId : rewardSpellIds)
                {
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                        handler->PSendSysMessage("    - {} (ID: {})", spell->SpellName[0], spellId);
                }
            }
        }

        // Also log summary to console when configured
        if (LogCompleted)
        {
            LOG_INFO("module.QuestCatchup", "=== Quest Catchup: {} ===", source->GetName().c_str());
            LOG_INFO("module.QuestCatchup", "  Target {} completed: {} quests", target->GetName().c_str(), completedQuests.size());
            if (!inProgressQuests.empty())
                LOG_INFO("module.QuestCatchup", "  Target {} in-progress: {} quests", target->GetName().c_str(), inProgressQuests.size());
            LOG_INFO("module.QuestCatchup", "  Eligible for you: {} quests", targetCompletedCount + targetInProgressCount);
            LOG_INFO("module.QuestCatchup", "  Already have: {} quests", sourceHasCount);
            if (DryRun)
                LOG_INFO("module.QuestCatchup", "  [DRY RUN] Would complete/accept: {} quests", dryRunTotal);
            else
            {
                LOG_INFO("module.QuestCatchup", "  Actually completed: {} quests", completedTotal);
                if (acceptedTotal > 0)
                    LOG_INFO("module.QuestCatchup", "  Accepted in-progress: {} quests", acceptedTotal);
            }
        }

        return true;
    }
}

// ===========================================================================
// Script hooks
// ===========================================================================

using namespace Acore::ChatCommands;

class QuestCatchupWorldScript : public WorldScript
{
public:
    QuestCatchupWorldScript() : WorldScript("QuestCatchupWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        QC::LoadConfig(reload);
    }

    void OnBeforeWorldInitialized() override
    {
        if (QC::Enabled)
        {
            LOG_INFO("server.loading", " ");
            LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
            LOG_INFO("server.loading", "║                                                          ║");
            LOG_INFO("server.loading", "║               Quest Catchup Module                       ║");
            LOG_INFO("server.loading", "║                                                          ║");
            LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
            LOG_INFO("server.loading", "║             Helps your alts sync quest progress on new   ║");
            LOG_INFO("server.loading", "║                   alts. No more lagging behind!          ║");
            LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
            LOG_INFO("server.loading", "║                  Author: Jellypowered                    ║");
            LOG_INFO("server.loading", "║               Licensed under MIT License                 ║");
            LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");
            LOG_INFO("server.loading", " ");
            LOG_INFO("module.QuestCatchup", "Quest Catch-Up Config loaded with options:");
            LOG_INFO("module.QuestCatchup", "  Enabled={}, DryRun={}, LogCompleted={}",
                QC::Enabled, QC::DryRun, QC::LogCompleted);
            LOG_INFO("server.loading", " ");
        }
    }
};

class QuestCatchupCommandScript : public CommandScript
{
public:
    QuestCatchupCommandScript() : CommandScript("QuestCatchupCommandScript") { }

    static bool HandleCatchupCommand(ChatHandler* handler)
    {
        return QC::HandleCatchupCommand(handler);
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "qcatchup", HandleCatchupCommand, SEC_PLAYER, Console::No },
            { "qc",       HandleCatchupCommand, SEC_PLAYER, Console::No },
        };
        return commandTable;
    }
};

// ===========================================================================
// Loader
// ===========================================================================

void AddQCCommandScripts()
{
    new QuestCatchupWorldScript();
    new QuestCatchupCommandScript();
}
