/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Chat.h"
#include "Config.h"
#include "Group.h"
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

    SyncStats SyncQuestsToPlayer(Player* source, Player* target)
    {
        SyncStats stats;

        // Collect source's completed quests
        std::set<uint32> completedQuests(source->getRewardedQuests().begin(), source->getRewardedQuests().end());

        // Collect source's in-progress quests
        std::set<uint32> inProgressQuests;
        for (auto const& [questId, statusData] : source->getQuestStatusMap())
        {
            if (statusData.Status == QUEST_STATUS_INCOMPLETE)
                inProgressQuests.insert(questId);
        }

        // Sync completed quests
        for (uint32 questId : completedQuests)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Skip if target already completed or was rewarded this quest
            uint8 targetStatus = target->GetQuestStatus(questId);
            if (targetStatus == QUEST_STATUS_COMPLETE || targetStatus == QUEST_STATUS_REWARDED)
                continue;

            // Skip if target cannot do this quest
            if (!IsQuestEligible(quest, target))
                continue;

            // Collect reward spells
            if (uint32 displaySpell = quest->GetRewSpell())
                stats.rewardSpellIds.insert(displaySpell);
            if (int32 castSpell = quest->GetRewSpellCast())
                stats.rewardSpellIds.insert(static_cast<uint32>(castSpell));

            if (DryRun)
            {
                ++stats.dryRunTotal;
                continue;
            }

            // Complete the quest
            target->CompleteQuest(questId);

            // Grant rewards
            target->RewardQuest(quest, 0, target, false);

            ++stats.completed;

            if (LogCompleted)
                LOG_DEBUG("module.QuestCatchup", "Completed quest %u \"%s\" for player %s (source: %s)",
                    questId, quest->GetTitle().c_str(), target->GetName().c_str(), source->GetName().c_str());
        }

        // Accept in-progress quests
        for (uint32 questId : inProgressQuests)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Skip if target already has this quest
            if (target->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                continue;

            // Skip if target cannot take this quest
            if (!IsQuestEligible(quest, target))
                continue;

            // Skip if target cannot add it
            if (!target->CanAddQuest(quest, false))
                continue;

            if (DryRun)
            {
                ++stats.dryRunTotal;
                continue;
            }

            target->AddQuest(quest, target);
            ++stats.accepted;

            if (LogCompleted)
                LOG_DEBUG("module.QuestCatchup", "Accepted quest %u \"%s\" for player %s (source: %s)",
                    questId, quest->GetTitle().c_str(), target->GetName().c_str(), source->GetName().c_str());
        }

        return stats;
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
        {
            handler->SendSysMessage("You must select a player target to use this command.");
            return true;
        }

        if (source != target && handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return true;

        SyncStats stats = SyncQuestsToPlayer(target, source);

        // Print summary to chat (always visible to player)
        handler->PSendSysMessage("Quest Catchup for {}:", source->GetName().c_str());
        handler->PSendSysMessage("  Target has completed: {} quests", target->getRewardedQuests().size());
        handler->PSendSysMessage("  Target has in-progress: {} quests", target->getQuestStatusMap().size());

        if (DryRun)
        {
            handler->PSendSysMessage("  Eligible for you: {} quests", stats.dryRunTotal);
            handler->PSendSysMessage("  [DRY RUN] Would complete/accept: {} quests", stats.dryRunTotal);
            if (!stats.rewardSpellIds.empty())
            {
                handler->PSendSysMessage("  Spells/Abilities: ");
                for (uint32 spellId : stats.rewardSpellIds)
                {
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                        handler->PSendSysMessage("    - {} (ID: {})", spell->SpellName[0], spellId);
                }
            }
            handler->PSendSysMessage("  Set QuestCatchup.DryRun=false in worldserver.conf to actually complete them.");
        }
        else
        {
            handler->PSendSysMessage("  Eligible for you: {} quests", stats.completed + stats.accepted);
            handler->PSendSysMessage("  Completed: {} quests", stats.completed);
            if (stats.accepted > 0)
                handler->PSendSysMessage("  Accepted in-progress: {} quests", stats.accepted);
            if (!stats.rewardSpellIds.empty())
            {
                handler->PSendSysMessage("  Gained spells/abilities: ");
                for (uint32 spellId : stats.rewardSpellIds)
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
            LOG_INFO("module.QuestCatchup", "  Target {} completed: {} quests", target->GetName().c_str(), target->getRewardedQuests().size());
            LOG_INFO("module.QuestCatchup", "  Target {} in-progress: {} quests", target->GetName().c_str(), target->getQuestStatusMap().size());
            LOG_INFO("module.QuestCatchup", "  Eligible for you: {} quests", stats.completed + stats.accepted);
            if (DryRun)
                LOG_INFO("module.QuestCatchup", "  [DRY RUN] Would complete/accept: {} quests", stats.dryRunTotal);
            else
            {
                LOG_INFO("module.QuestCatchup", "  Actually completed: {} quests", stats.completed);
                if (stats.accepted > 0)
                    LOG_INFO("module.QuestCatchup", "  Accepted in-progress: {} quests", stats.accepted);
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

    inline static bool HandleSyncCommand(ChatHandler* handler)
    {
        if (!QC::Enabled)
        {
            handler->SendSysMessage("Quest Catchup module is currently disabled. Contact an administrator.");
            return true;
        }

        Player* source = handler->GetSession()->GetPlayer();
        if (!source)
            return true;

        Group* group = source->GetGroup();
        if (!group)
        {
            handler->SendSysMessage("You are not in a group.");
            return true;
        }

        // Gather online members
        std::vector<Player*> onlineMembers;
        uint32 skippedOffline = 0;
        group->DoForAllMembers([&](Player* member)
        {
            if (member)
                onlineMembers.push_back(member);
            else
                ++skippedOffline;
        });

        if (onlineMembers.size() < 2)
        {
            handler->SendSysMessage("No group members available to sync.");
            return true;
        }

        // Execute syncs with throttle
        uint32 totalSyncs = 0;
        uint32 totalCompleted = 0;
        uint32 totalAccepted = 0;
        uint32 totalDryRun = 0;
        std::set<uint32> allRewardSpellIds;

        for (Player* member : onlineMembers)
        {
            for (Player* target : onlineMembers)
            {
                if (target == member)
                    continue;

                QC::SyncStats stats = QC::SyncQuestsToPlayer(member, target);

                ++totalSyncs;
                totalCompleted += stats.completed;
                totalAccepted += stats.accepted;
                totalDryRun += stats.dryRunTotal;
                for (uint32 spellId : stats.rewardSpellIds)
                    allRewardSpellIds.insert(spellId);
            }
        }

        // Print final summary
        if (group->isRaidGroup())
            handler->PSendSysMessage("Quest Sync (Raid of {}):", onlineMembers.size());
        else
            handler->PSendSysMessage("Quest Sync (Party of {}):", onlineMembers.size());

        if (skippedOffline > 0)
            handler->PSendSysMessage("  Members: {} ({} online, {} skipped)", onlineMembers.size() + skippedOffline, onlineMembers.size(), skippedOffline);
        else
            handler->PSendSysMessage("  Members: {}", onlineMembers.size());

        handler->PSendSysMessage("  Total syncs: {}", totalSyncs);

        if (QC::DryRun)
        {
            handler->PSendSysMessage("  [DRY RUN] Would complete/accept: {} quests", totalDryRun);
        }
        else
        {
            handler->PSendSysMessage("  Quests completed: {}", totalCompleted);
            if (totalAccepted > 0)
                handler->PSendSysMessage("  Quests accepted: {}", totalAccepted);
        }

        if (!allRewardSpellIds.empty())
        {
            handler->PSendSysMessage("  Spells gained:");
            for (uint32 spellId : allRewardSpellIds)
            {
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                    handler->PSendSysMessage("    - {} (ID: {})", spell->SpellName[0], spellId);
            }
        }

        if (QC::DryRun)
            handler->PSendSysMessage("  Set QuestCatchup.DryRun=false in worldserver.conf to actually complete them.");

        // Console log
        if (QC::LogCompleted)
        {
            LOG_INFO("module.QuestCatchup", "=== Quest Sync ({} of {}) ===",
                group->isRaidGroup() ? "Raid" : "Party", onlineMembers.size());
            LOG_INFO("module.QuestCatchup", "  Members: {}", onlineMembers.size());
            LOG_INFO("module.QuestCatchup", "  Total syncs: {}", totalSyncs);
            if (QC::DryRun)
            {
                LOG_INFO("module.QuestCatchup", "  [DRY RUN] Would complete/accept: {} quests", totalDryRun);
                if (!allRewardSpellIds.empty())
                {
                    LOG_INFO("module.QuestCatchup", "  Spells gained:");
                    for (uint32 spellId : allRewardSpellIds)
                    {
                        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                            LOG_INFO("module.QuestCatchup", "    - {} (ID: {})", spell->SpellName[0], spellId);
                    }
                }
                LOG_INFO("module.QuestCatchup", "  Set QuestCatchup.DryRun=false to actually complete quests.");
            }
            else
            {
                LOG_INFO("module.QuestCatchup", "  Quests completed: {}", totalCompleted);
                if (totalAccepted > 0)
                    LOG_INFO("module.QuestCatchup", "  Quests accepted: {}", totalAccepted);
                if (!allRewardSpellIds.empty())
                {
                    LOG_INFO("module.QuestCatchup", "  Spells gained:");
                    for (uint32 spellId : allRewardSpellIds)
                    {
                        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                            LOG_INFO("module.QuestCatchup", "    - {} (ID: {})", spell->SpellName[0], spellId);
                    }
                }
            }
        }

        return true;
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "qcatchup", HandleCatchupCommand, SEC_PLAYER, Console::No },
            { "qc",       HandleCatchupCommand, SEC_PLAYER, Console::No },
            { "qsync",    HandleSyncCommand,    SEC_PLAYER, Console::No },
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
