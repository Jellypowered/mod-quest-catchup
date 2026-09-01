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
#include <map>
#include <sstream>

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
        RepairBrokenQuests = sConfigMgr->GetOption<bool>("QuestCatchup.RepairBrokenQuests", true);

        if (reload)
            LOG_DEBUG("module.QuestCatchup", "Configuration reloaded. Enabled={}, DryRun={}, LogCompleted={}, RepairBrokenQuests={}", Enabled, DryRun, LogCompleted, RepairBrokenQuests);
        else
            LOG_DEBUG("module.QuestCatchup", "Configuration loaded. Enabled={}, DryRun={}, LogCompleted={}, RepairBrokenQuests={}", Enabled, DryRun, LogCompleted, RepairBrokenQuests);
    }

    bool IsQuestEligible(Quest const* quest, Player const* player)
    {
        if (!quest)
            return false;

        // Skip quests that can be repeated or reset on a schedule.
        // Catch-up should only copy one-time progression quests.
        if (quest->IsRepeatable() || quest->IsDaily() || quest->IsWeekly() || quest->IsMonthly() || quest->IsSeasonal())
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

    SyncStats SyncQuestsToPlayer(Player* source, Player* target, bool dryRunMode)
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

            bool isRewarded = target->IsQuestRewarded(questId);

            // If already rewarded, check if we should repair broken completions.
            // Never repair repeatable/time-reset quests to avoid duplicate rewards.
            if (isRewarded)
            {
                if (!RepairBrokenQuests)
                    continue;  // Skip if repair mode is disabled

                if (quest->IsRepeatable() || quest->IsDaily() || quest->IsWeekly() || quest->IsMonthly() || quest->IsSeasonal())
                    continue;

                auto const& questStatusMap = target->getQuestStatusMap();
                auto statusIt = questStatusMap.find(questId);
                if (statusIt == questStatusMap.end())
                    continue;  // Normally rewarded quests have no active status entry

                // Check if quest has unmet objectives (broken state from bot sync)
                QuestStatusData const& q_status = statusIt->second;
                bool needsRepair = false;

                // Check item objectives
                if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_DELIVER))
                {
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredItemCount[i] != 0 &&
                            q_status.ItemCount[i] < quest->RequiredItemCount[i])
                        {
                            needsRepair = true;
                            break;
                        }
                    }
                }

                // Check creature/GO objectives
                if (!needsRepair && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_KILL | QUEST_SPECIAL_FLAGS_CAST | QUEST_SPECIAL_FLAGS_SPEAKTO))
                {
                    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredNpcOrGo[i] != 0 &&
                            quest->RequiredNpcOrGoCount[i] != 0 &&
                            q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                        {
                            needsRepair = true;
                            break;
                        }
                    }
                }

                if (!needsRepair)
                    continue;  // Quest is properly completed, skip

                if (LogCompleted)
                    LOG_DEBUG("module.QuestCatchup", "Repairing broken quest %u \"%s\" for player %s (source: %s)",
                        questId, quest->GetTitle().c_str(), target->GetName().c_str(), source->GetName().c_str());
            }

            // Skip if target cannot do this quest
            if (!IsQuestEligible(quest, target))
                continue;

            // Collect reward spells
            if (uint32 displaySpell = quest->GetRewSpell())
                stats.rewardSpellIds.insert(displaySpell);
            if (int32 castSpell = quest->GetRewSpellCast())
                stats.rewardSpellIds.insert(static_cast<uint32>(castSpell));

            if (dryRunMode)
            {
                ++stats.dryRunTotal;
                stats.completeQuestIds.push_back(questId);
                continue;
            }
            
            // For repaired quests, reset the rewarded state first
            if (isRewarded && RepairBrokenQuests)
            {
                target->RemoveRewardedQuest(questId, false);
                target->SetQuestStatus(questId, QUEST_STATUS_COMPLETE);
            }

            // For item turn-in quests: give target the required items first
            if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_DELIVER))
            {
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredItemId[i] != 0 && quest->RequiredItemCount[i] != 0)
                    {
                        uint32 itemId = quest->RequiredItemId[i];
                        uint32 requiredCount = quest->RequiredItemCount[i];
                        uint32 currentCount = target->GetItemCount(itemId, true);
                        
                        if (currentCount < requiredCount)
                        {
                            uint32 toGive = requiredCount - currentCount;
                            ItemPosCountVec dest;
                            InventoryResult res = target->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, toGive);
                            if (res == EQUIP_ERR_OK)
                                target->StoreNewItem(dest, itemId, toGive);
                            else
                            {
                                std::vector<std::pair<uint32, uint32>> mailItems;
                                mailItems.emplace_back(itemId, toGive);
                                target->SendItemRetrievalMail(mailItems);
                            }
                        }
                    }
                }
            }

            // For creature/GO kill quests: set objective counts
            if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_KILL | QUEST_SPECIAL_FLAGS_CAST | QUEST_SPECIAL_FLAGS_SPEAKTO))
            {
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredNpcOrGo[i] != 0 && quest->RequiredNpcOrGoCount[i] != 0)
                    {
                        uint16 slot = target->FindQuestSlot(questId);
                        if (slot < MAX_QUEST_LOG_SIZE)
                        {
                            target->getQuestStatusMap()[questId].CreatureOrGOCount[i] = quest->RequiredNpcOrGoCount[i];
                            target->SetQuestSlotCounter(slot, i, quest->RequiredNpcOrGoCount[i]);
                        }
                    }
                }
            }

            // Complete the quest
            target->CompleteQuest(questId);

            // Grant rewards
            target->RewardQuest(quest, 0, target, false);

            stats.completeQuestIds.push_back(questId);

            if (isRewarded && RepairBrokenQuests)
                ++stats.repaired;
            else
                ++stats.completed;

            if (LogCompleted)
            {
                if (isRewarded && RepairBrokenQuests)
                    LOG_DEBUG("module.QuestCatchup", "Repaired quest %u \"%s\" for player %s (source: %s)",
                        questId, quest->GetTitle().c_str(), target->GetName().c_str(), source->GetName().c_str());
                else
                    LOG_DEBUG("module.QuestCatchup", "Completed quest %u \"%s\" for player %s (source: %s)",
                        questId, quest->GetTitle().c_str(), target->GetName().c_str(), source->GetName().c_str());
            }
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

            if (dryRunMode)
            {
                ++stats.dryRunTotal;
                stats.acceptQuestIds.push_back(questId);
                continue;
            }

            target->AddQuest(quest, target);
            stats.acceptQuestIds.push_back(questId);
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

        SyncStats stats = SyncQuestsToPlayer(target, source, DryRun);

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
            LOG_INFO("module.QuestCatchup", "  Enabled={}, DryRun={}, LogCompleted={}, RepairBrokenQuests={}",
                QC::Enabled, QC::DryRun, QC::LogCompleted, QC::RepairBrokenQuests);
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

    inline static bool HandleSyncCommand(ChatHandler* handler, Optional<Variant<EXACT_SEQUENCE("doveride"), EXACT_SEQUENCE("dryrunoverride")>> dryRunOverride)
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

        bool const effectiveDryRun = QC::DryRun && !dryRunOverride.has_value();
        bool const showDetailedSummary = QC::LogCompleted || dryRunOverride.has_value();

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
        std::map<uint32, std::set<std::string>> dryRunAcceptByQuest;
        std::map<uint32, std::set<std::string>> dryRunCompleteByQuest;

        for (Player* member : onlineMembers)
        {
            for (Player* target : onlineMembers)
            {
                if (target == member)
                    continue;

                QC::SyncStats stats = QC::SyncQuestsToPlayer(member, target, effectiveDryRun);

                ++totalSyncs;
                totalCompleted += stats.completed;
                totalAccepted += stats.accepted;
                totalDryRun += stats.dryRunTotal;
                for (uint32 spellId : stats.rewardSpellIds)
                    allRewardSpellIds.insert(spellId);

                if (showDetailedSummary)
                {
                    for (uint32 questId : stats.acceptQuestIds)
                        dryRunAcceptByQuest[questId].insert(target->GetName());

                    for (uint32 questId : stats.completeQuestIds)
                        dryRunCompleteByQuest[questId].insert(target->GetName());
                }
            }
        }

        auto countUniqueDryRunActions = [](std::map<uint32, std::set<std::string>> const& byQuest)
        {
            uint32 total = 0;
            for (auto const& [_, botNames] : byQuest)
                total += botNames.size();
            return total;
        };

        uint32 uniqueDryRunAccepts = countUniqueDryRunActions(dryRunAcceptByQuest);
        uint32 uniqueDryRunCompletes = countUniqueDryRunActions(dryRunCompleteByQuest);

        auto formatBotList = [](std::set<std::string> const& botNames)
        {
            std::ostringstream out;
            bool first = true;
            for (std::string const& botName : botNames)
            {
                if (!first)
                    out << ", ";
                out << botName;
                first = false;
            }
            return out.str();
        };

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

        if (effectiveDryRun)
        {
            handler->PSendSysMessage("  [DRY RUN] Raw sync matches: {}", totalDryRun);
            handler->PSendSysMessage("  [DRY RUN] Unique bot quest actions: {}", uniqueDryRunAccepts + uniqueDryRunCompletes);
            if (uniqueDryRunAccepts > 0)
                handler->PSendSysMessage("    Accepts: {}", uniqueDryRunAccepts);
            if (uniqueDryRunCompletes > 0)
                handler->PSendSysMessage("    Completes: {}", uniqueDryRunCompletes);

            if (!dryRunAcceptByQuest.empty())
            {
                handler->PSendSysMessage("  [Accept]");
                for (auto const& [questId, botNames] : dryRunAcceptByQuest)
                {
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                    handler->PSendSysMessage("    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                }
            }

            if (!dryRunCompleteByQuest.empty())
            {
                handler->PSendSysMessage("  [Complete]");
                for (auto const& [questId, botNames] : dryRunCompleteByQuest)
                {
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                    handler->PSendSysMessage("    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                }
            }
        }
        else
        {
            handler->PSendSysMessage("  Quests completed: {}", totalCompleted);
            if (totalAccepted > 0)
                handler->PSendSysMessage("  Quests accepted: {}", totalAccepted);

            if (dryRunOverride.has_value())
            {
                if (!dryRunAcceptByQuest.empty())
                {
                    handler->PSendSysMessage("  [Accept]");
                    for (auto const& [questId, botNames] : dryRunAcceptByQuest)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                        std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                        handler->PSendSysMessage("    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                    }
                }
                if (!dryRunCompleteByQuest.empty())
                {
                    handler->PSendSysMessage("  [Complete]");
                    for (auto const& [questId, botNames] : dryRunCompleteByQuest)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                        std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                        handler->PSendSysMessage("    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                    }
                }
            }
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

        if (effectiveDryRun)
            handler->PSendSysMessage("  Set QuestCatchup.DryRun=false in worldserver.conf or use .qsync doveride to actually perform those actions.");

        // Console log
        if (QC::LogCompleted)
        {
            LOG_INFO("module.QuestCatchup", "=== Quest Sync ({} of {}) ===",
                group->isRaidGroup() ? "Raid" : "Party", onlineMembers.size());
            LOG_INFO("module.QuestCatchup", "  Members: {}", onlineMembers.size());
            LOG_INFO("module.QuestCatchup", "  Total syncs: {}", totalSyncs);
            if (effectiveDryRun)
            {
                LOG_INFO("module.QuestCatchup", "  [DRY RUN] Raw sync matches: {}", totalDryRun);
                LOG_INFO("module.QuestCatchup", "  [DRY RUN] Unique bot quest actions: {}", uniqueDryRunAccepts + uniqueDryRunCompletes);
                if (uniqueDryRunAccepts > 0)
                    LOG_INFO("module.QuestCatchup", "    Accepts: {}", uniqueDryRunAccepts);
                if (uniqueDryRunCompletes > 0)
                    LOG_INFO("module.QuestCatchup", "    Completes: {}", uniqueDryRunCompletes);

                if (!dryRunAcceptByQuest.empty())
                {
                    LOG_INFO("module.QuestCatchup", "  [Accept]");
                    for (auto const& [questId, botNames] : dryRunAcceptByQuest)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                        std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                        LOG_INFO("module.QuestCatchup", "    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                    }
                }

                if (!dryRunCompleteByQuest.empty())
                {
                    LOG_INFO("module.QuestCatchup", "  [Complete]");
                    for (auto const& [questId, botNames] : dryRunCompleteByQuest)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                        std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                        LOG_INFO("module.QuestCatchup", "    [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                    }
                }

                if (!allRewardSpellIds.empty())
                {
                    LOG_INFO("module.QuestCatchup", "  Spells gained:");
                    for (uint32 spellId : allRewardSpellIds)
                    {
                        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                            LOG_INFO("module.QuestCatchup", "    - {} (ID: {})", spell->SpellName[0], spellId);
                    }
                }
                LOG_INFO("module.QuestCatchup", "  Set QuestCatchup.DryRun=false or use .qsync doveride to actually perform those actions.");
            }
            else
            {
                LOG_INFO("module.QuestCatchup", "  Quests completed: {}", totalCompleted);
                if (totalAccepted > 0)
                    LOG_INFO("module.QuestCatchup", "  Quests accepted: {}", totalAccepted);
                if (showDetailedSummary)
                {
                    LOG_INFO("module.QuestCatchup", "  Detailed quest actions:");
                    if (!dryRunAcceptByQuest.empty())
                    {
                        LOG_INFO("module.QuestCatchup", "    [Accept]");
                        for (auto const& [questId, botNames] : dryRunAcceptByQuest)
                        {
                            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                            std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                            LOG_INFO("module.QuestCatchup", "      [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                        }
                    }
                    if (!dryRunCompleteByQuest.empty())
                    {
                        LOG_INFO("module.QuestCatchup", "    [Complete]");
                        for (auto const& [questId, botNames] : dryRunCompleteByQuest)
                        {
                            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                            std::string questName = quest ? quest->GetTitle() : "Unknown Quest";
                            LOG_INFO("module.QuestCatchup", "      [{}] [{}] [Bots: {}]", questId, questName, formatBotList(botNames));
                        }
                    }
                }
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
