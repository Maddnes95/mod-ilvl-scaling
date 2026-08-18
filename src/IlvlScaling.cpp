/*
 * mod-ilvl-scaling — implémentation.
 *
 * Principe :
 *  - la « difficulté » est appliquée sans toucher aux stats des créatures :
 *      * dégâts créature -> camp joueur multipliés par damageMult ;
 *      * dégâts camp joueur -> créature divisés par healthMult (vie
 *        effective augmentée, la barre de vie affichée reste inchangée) ;
 *  - le loot est amplifié via lootMult : or ramassé, chance de drop des
 *    entrées à pourcentage, et nombre de jets des références (loot de boss).
 *
 * Seuls les VRAIS joueurs comptent dans la moyenne d'ilvl (session dont
 * WorldSession::IsBot() est faux — fourni par le fork Playerbot). Les bots
 * subissent en revanche le scaling comme tout le monde.
 */

#include "IlvlScaling.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ModIlvlScaling
{
Settings config;

namespace
{
struct CacheEntry
{
    ScalingInfo info;
    time_t computedAt = 0;
};

// Clé : (guid brut du groupe — ou du joueur si solo, id de carte).
std::map<std::pair<uint64, uint32>, CacheEntry> scalingCache;
std::mutex cacheMutex;

float ExpectedIlvl(float avgLevel)
{
    if (avgLevel >= 80.f)
        return config.BaselineIlvl80;

    return avgLevel * config.BaselinePerLevel;
}

float MakeMult(float delta, float perIlvl, float mn, float mx)
{
    return std::clamp(1.f + delta * perIlvl, mn, mx);
}

void ComputeScaling(Player* player, ScalingInfo& out)
{
    float sumIlvl = 0.f;
    float sumLevel = 0.f;
    uint32 count = 0;

    auto addIfRealPlayer = [&](Player* member)
    {
        if (!member || !member->GetSession() || member->GetSession()->IsBot())
            return;

        // Seuls les vrais joueurs présents sur la même carte comptent (un
        // membre resté à Dalaran n'influence pas le scaling du donjon).
        if (member->FindMap() != player->FindMap())
            return;

        sumIlvl += AverageEquippedItemLevel(member);
        sumLevel += float(member->GetLevel());
        ++count;
    };

    if (Group* group = player->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            addIfRealPlayer(ref->GetSource());
    }
    else
        addIfRealPlayer(player);

    out = ScalingInfo();
    out.realPlayers = count;
    if (!count)
        return;

    out.avgIlvl = sumIlvl / float(count);
    out.avgLevel = sumLevel / float(count);

    float delta = out.avgIlvl - ExpectedIlvl(out.avgLevel);
    out.healthMult = MakeMult(delta, config.HealthPerIlvl, config.HealthMin, config.HealthMax);
    out.damageMult = MakeMult(delta, config.DamagePerIlvl, config.DamageMin, config.DamageMax);
    out.lootMult   = MakeMult(delta, config.LootPerIlvl, config.LootMin, config.LootMax);
}

// La créature est-elle concernée par le scaling ? (exclut les unités du
// camp joueur : familiers, totems, charmes, gardiens…)
bool IsScalableCreature(Unit* unit)
{
    Creature* creature = unit->ToCreature();
    if (!creature)
        return false;

    if (creature->IsPet() || creature->IsTotem() || creature->IsControlledByPlayer() ||
        creature->GetCharmerOrOwnerPlayerOrPlayerItself())
        return false;

    if (config.OnlyInInstances)
    {
        Map* map = creature->FindMap();
        if (!map || !map->IsDungeon())
            return false;
    }

    return true;
}

bool IsTrivialFor(Creature const* creature, ScalingInfo const& info)
{
    return config.IgnoreTrivial &&
           float(creature->GetLevel()) + float(config.TrivialLevelDiff) <= info.avgLevel;
}

// Le bonus de loot s'applique-t-il là où se trouve ce joueur ?
bool LootScalingAppliesHere(Player const* player)
{
    if (!config.OnlyInInstances)
        return true;

    Map const* map = player->FindMap();
    return map && map->IsDungeon();
}

// ---------------------------------------------------------------------------
// Upgrade des objets : remplacement par des équivalents d'ilvl adapté.
// ---------------------------------------------------------------------------

struct UpgradeCandidate
{
    uint16 itemLevel;
    uint8  requiredLevel;
    uint32 itemId;
};

// (classe, sous-classe, emplacement, qualité) -> candidats triés par ilvl.
std::unordered_map<uint32, std::vector<UpgradeCandidate>> upgradeIndex;
std::once_flag upgradeIndexOnce;

uint32 UpgradeKey(uint32 itemClass, uint32 subClass, uint32 invType, uint32 quality)
{
    // Les robes sont des torses, les armes à distance "droites" (fusils,
    // arbalètes, baguettes) partagent l'emplacement distance.
    if (invType == INVTYPE_ROBE)
        invType = INVTYPE_CHEST;
    if (invType == INVTYPE_RANGEDRIGHT)
        invType = INVTYPE_RANGED;

    return (itemClass << 24) | (subClass << 16) | (invType << 8) | quality;
}

bool IsCleanUpgradeCandidate(ItemTemplate const& proto)
{
    if (proto.Class != ITEM_CLASS_WEAPON && proto.Class != ITEM_CLASS_ARMOR)
        return false;

    if (proto.Quality < ITEM_QUALITY_UNCOMMON || proto.Quality > ITEM_QUALITY_EPIC)
        return false;

    if (!proto.InventoryType || !proto.ItemLevel)
        return false;

    if (!proto.SellPrice)
        return false; // écarte l'essentiel des objets de test/PNJ

    if (proto.Bonding == BIND_QUEST_ITEM)
        return false;

    if (proto.HasFlag(ITEM_FLAG_DEPRECATED))
        return false;

    std::string const& name = proto.Name1;
    if (name.rfind("Monster - ", 0) == 0 || name.rfind("Test", 0) == 0 || name.rfind("TEST", 0) == 0 ||
        name.rfind("NPC ", 0) == 0 || name.find("Deprecated") != std::string::npos ||
        name.find("DEPRECATED") != std::string::npos || name.find("[PH]") != std::string::npos)
        return false;

    return true;
}

void BuildUpgradeIndex()
{
    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!IsCleanUpgradeCandidate(proto))
            continue;

        upgradeIndex[UpgradeKey(proto.Class, proto.SubClass, proto.InventoryType, proto.Quality)]
            .push_back({ uint16(proto.ItemLevel), uint8(proto.RequiredLevel), itemId });
    }

    for (auto& [key, candidates] : upgradeIndex)
        std::sort(candidates.begin(), candidates.end(),
                  [](UpgradeCandidate const& a, UpgradeCandidate const& b) { return a.itemLevel < b.itemLevel; });
}

// Choisit un objet de même archétype dans [target - Band, target]. Si la
// fourchette est vide (ex. verts au-delà de l'ilvl ~200), retombe sur les
// meilleurs candidats disponibles sous la cible.
uint32 PickUpgradeReplacement(ItemTemplate const* proto, float target, float avgLevel)
{
    auto it = upgradeIndex.find(UpgradeKey(proto->Class, proto->SubClass, proto->InventoryType, proto->Quality));
    if (it == upgradeIndex.end())
        return 0;

    float bestIlvl = 0.f;
    for (UpgradeCandidate const& c : it->second)
    {
        if (float(c.itemLevel) > target)
            break;
        if (float(c.requiredLevel) > avgLevel + 0.5f)
            continue;
        bestIlvl = float(c.itemLevel);
    }

    // Rien de mieux que l'objet d'origine : on ne touche à rien.
    if (bestIlvl <= float(proto->ItemLevel))
        return 0;

    float bandLow = target - float(config.LootUpgradeBand);
    float poolLow = (bestIlvl >= bandLow) ? bandLow : bestIlvl - 5.f;

    std::vector<uint32> pool;
    for (UpgradeCandidate const& c : it->second)
    {
        if (float(c.itemLevel) > target)
            break;
        if (float(c.itemLevel) < poolLow || float(c.itemLevel) <= float(proto->ItemLevel))
            continue;
        if (float(c.requiredLevel) > avgLevel + 0.5f)
            continue;
        pool.push_back(c.itemId);
    }

    if (pool.empty())
        return 0;

    return pool[urand(0, pool.size() - 1)];
}

void TryUpgradeLootItem(LootItem& lootItem, ScalingInfo const& info)
{
    // On ne touche jamais aux objets de quête, multi-drop ou conditionnels.
    if (lootItem.needs_quest || lootItem.freeforall || !lootItem.conditions.empty())
        return;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(lootItem.itemid);
    if (!proto)
        return;

    if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
        return;

    if (proto->Quality < config.LootUpgradeMinQuality || proto->Quality > ITEM_QUALITY_EPIC)
        return;

    if (!proto->InventoryType)
        return;

    // Déjà dans (ou au-dessus de) la fourchette visée : rien à faire.
    if (float(proto->ItemLevel) >= info.avgIlvl - float(config.LootUpgradeBand))
        return;

    uint32 newId = PickUpgradeReplacement(proto, info.avgIlvl, info.avgLevel);
    if (!newId || newId == lootItem.itemid)
        return;

    lootItem.itemid = newId;
    lootItem.randomSuffix = GenerateEnchSuffixFactor(newId);
    lootItem.randomPropertyId = Item::GenerateItemRandomPropertyId(newId);

    ItemTemplate const* newProto = sObjectMgr->GetItemTemplate(newId);
    lootItem.freeforall = newProto && newProto->HasFlag(ITEM_FLAG_MULTI_DROP);
    lootItem.follow_loot_rules = newProto && newProto->HasFlagCu(ITEM_FLAGS_CU_FOLLOW_LOOT_RULES);
}

// Appelé juste après la génération du loot d'une créature (Unit::Kill remplit
// le loot avant les hooks de kill).
void UpgradeCreatureLoot(Player* player, Creature* killed)
{
    if (!config.Enabled || !config.LootUpgradeEnable || !player || !killed)
        return;

    if (killed->loot.items.empty())
        return;

    if (!LootScalingAppliesHere(player))
        return;

    ScalingInfo info;
    if (!GetScalingForPlayer(player, info) || IsTrivialFor(killed, info))
        return;

    std::call_once(upgradeIndexOnce, BuildUpgradeIndex);

    for (LootItem& lootItem : killed->loot.items)
        TryUpgradeLootItem(lootItem, info);
}

void LoadSettings()
{
    config.Enabled          = sConfigMgr->GetOption<bool>("IlvlScaling.Enable", true);
    config.Announce         = sConfigMgr->GetOption<bool>("IlvlScaling.Announce", true);
    config.OnlyInInstances  = sConfigMgr->GetOption<bool>("IlvlScaling.OnlyInInstances", true);
    config.IgnoreTrivial    = sConfigMgr->GetOption<bool>("IlvlScaling.Trivial.Ignore", true);
    config.TrivialLevelDiff = sConfigMgr->GetOption<uint32>("IlvlScaling.Trivial.LevelDiff", 9);
    config.CacheSeconds     = sConfigMgr->GetOption<uint32>("IlvlScaling.Cache.Seconds", 30);

    config.BaselineIlvl80   = sConfigMgr->GetOption<float>("IlvlScaling.Baseline.Ilvl80", 190.0f);
    config.BaselinePerLevel = sConfigMgr->GetOption<float>("IlvlScaling.Baseline.PerLevel", 2.0f);

    config.HealthPerIlvl = sConfigMgr->GetOption<float>("IlvlScaling.Health.PerIlvl", 0.02f);
    config.HealthMin     = sConfigMgr->GetOption<float>("IlvlScaling.Health.Min", 1.0f);
    config.HealthMax     = sConfigMgr->GetOption<float>("IlvlScaling.Health.Max", 4.0f);

    config.DamagePerIlvl = sConfigMgr->GetOption<float>("IlvlScaling.Damage.PerIlvl", 0.01f);
    config.DamageMin     = sConfigMgr->GetOption<float>("IlvlScaling.Damage.Min", 1.0f);
    config.DamageMax     = sConfigMgr->GetOption<float>("IlvlScaling.Damage.Max", 3.0f);

    config.LootPerIlvl = sConfigMgr->GetOption<float>("IlvlScaling.Loot.PerIlvl", 0.01f);
    config.LootMin     = sConfigMgr->GetOption<float>("IlvlScaling.Loot.Min", 1.0f);
    config.LootMax     = sConfigMgr->GetOption<float>("IlvlScaling.Loot.Max", 3.0f);

    config.LootScaleMoney          = sConfigMgr->GetOption<bool>("IlvlScaling.Loot.ScaleMoney", true);
    config.LootScaleDropChance     = sConfigMgr->GetOption<bool>("IlvlScaling.Loot.ScaleDropChance", true);
    config.LootScaleReferenceRolls = sConfigMgr->GetOption<bool>("IlvlScaling.Loot.ScaleReferenceRolls", true);

    config.LootUpgradeEnable     = sConfigMgr->GetOption<bool>("IlvlScaling.Loot.Upgrade.Enable", true);
    config.LootUpgradeBand       = sConfigMgr->GetOption<uint32>("IlvlScaling.Loot.Upgrade.Band", 15);
    config.LootUpgradeMinQuality = sConfigMgr->GetOption<uint32>("IlvlScaling.Loot.Upgrade.MinQuality", 2);
}
} // namespace

float AverageEquippedItemLevel(Player* player)
{
    uint32 sum = 0;
    uint32 expectedSlots = 0;

    bool twoHander = false;
    if (Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        twoHander = mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue; // cosmétique

        if (slot == EQUIPMENT_SLOT_OFFHAND && twoHander)
            continue; // pas de main gauche attendue avec une arme à deux mains

        ++expectedSlots;
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            sum += item->GetTemplate()->ItemLevel;
    }

    return expectedSlots ? float(sum) / float(expectedSlots) : 0.f;
}

bool GetScalingForPlayer(Player* player, ScalingInfo& out, bool forceRefresh /* = false */)
{
    if (!config.Enabled || !player)
        return false;

    Group* group = player->GetGroup();
    std::pair<uint64, uint32> key = {
        group ? group->GetGUID().GetRawValue() : player->GetGUID().GetRawValue(),
        player->GetMapId()
    };

    time_t now = GameTime::GetGameTime().count();

    if (!forceRefresh)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = scalingCache.find(key);
        if (it != scalingCache.end() && now - it->second.computedAt < time_t(config.CacheSeconds))
        {
            out = it->second.info;
            return out.realPlayers > 0;
        }
    }

    ComputeScaling(player, out);

    std::lock_guard<std::mutex> lock(cacheMutex);

    // Purge paresseuse des entrées périmées pour borner la taille du cache.
    if (scalingCache.size() > 512)
        for (auto it = scalingCache.begin(); it != scalingCache.end();)
            it = (now - it->second.computedAt >= time_t(config.CacheSeconds)) ? scalingCache.erase(it) : ++it;

    scalingCache[key] = { out, now };
    return out.realPlayers > 0;
}

void ClearCache()
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    scalingCache.clear();
}
} // namespace ModIlvlScaling

using namespace ModIlvlScaling;

class IlvlScaling_WorldScript : public WorldScript
{
public:
    IlvlScaling_WorldScript() : WorldScript("IlvlScaling_WorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
        ClearCache();
    }
};

class IlvlScaling_UnitScript : public UnitScript
{
public:
    IlvlScaling_UnitScript() : UnitScript("IlvlScaling_UnitScript") {}

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        HandleDamage(target, attacker, damage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage,
                                       SpellInfo const* /*spellInfo*/) override
    {
        HandleDamage(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage,
                                SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        uint32 value = uint32(damage);
        HandleDamage(target, attacker, value);
        damage = int32(value);
    }

private:
    static void HandleDamage(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!config.Enabled || !damage || !target || !attacker || target == attacker)
            return;

        // Créature -> camp joueur : dégâts augmentés.
        if (IsScalableCreature(attacker))
        {
            Player* owner = target->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!owner)
                return;

            ScalingInfo info;
            if (GetScalingForPlayer(owner, info) && !IsTrivialFor(attacker->ToCreature(), info))
                damage = uint32(float(damage) * info.damageMult);
        }
        // Camp joueur -> créature : dégâts réduits (= vie effective accrue).
        else if (IsScalableCreature(target))
        {
            Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!owner)
                return;

            ScalingInfo info;
            if (GetScalingForPlayer(owner, info) && !IsTrivialFor(target->ToCreature(), info) &&
                info.healthMult > 1.f)
                damage = uint32(float(damage) / info.healthMult);
        }
    }
};

class IlvlScaling_GlobalScript : public GlobalScript
{
public:
    IlvlScaling_GlobalScript() : GlobalScript("IlvlScaling_GlobalScript") {}

    // Entrées de loot à pourcentage : chance multipliée par lootMult.
    bool OnItemRoll(Player const* player, LootStoreItem const* lootItem, float& chance,
                    Loot& /*loot*/, LootStore const& store) override
    {
        if (!config.Enabled || !config.LootScaleDropChance || !player || !lootItem)
            return true;

        if (lootItem->needs_quest || chance <= 0.f || chance >= 100.f)
            return true;

        if (std::strcmp(store.GetName(), "creature_loot_template") != 0)
            return true;

        if (!LootScalingAppliesHere(player))
            return true;

        ScalingInfo info;
        if (GetScalingForPlayer(const_cast<Player*>(player), info))
            chance = std::min(chance * info.lootMult, 100.f);

        return true;
    }

    // Références (loot de boss) : nombre de jets multiplié par lootMult,
    // avec arrondi probabiliste pour la partie fractionnaire.
    void OnAfterRefCount(Player const* player, LootStoreItem* /*lootItem*/, Loot& /*loot*/,
                         bool /*canRate*/, uint16 /*lootMode*/, uint32& maxcount,
                         LootStore const& store) override
    {
        if (!config.Enabled || !config.LootScaleReferenceRolls || !player || !maxcount)
            return;

        if (std::strcmp(store.GetName(), "creature_loot_template") != 0)
            return;

        if (!LootScalingAppliesHere(player))
            return;

        ScalingInfo info;
        if (!GetScalingForPlayer(const_cast<Player*>(player), info) || info.lootMult <= 1.f)
            return;

        float scaled = float(maxcount) * info.lootMult;
        uint32 result = uint32(scaled);
        if (roll_chance_f((scaled - float(result)) * 100.f))
            ++result;

        maxcount = std::max(maxcount, result);
    }
};

class IlvlScaling_PlayerScript : public PlayerScript
{
public:
    IlvlScaling_PlayerScript() : PlayerScript("IlvlScaling_PlayerScript") {}

    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (!config.Enabled || !config.LootScaleMoney || !player || !loot)
            return;

        if (!LootScalingAppliesHere(player))
            return;

        ScalingInfo info;
        if (GetScalingForPlayer(player, info))
            loot->gold = uint32(float(loot->gold) * info.lootMult);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        UpgradeCreatureLoot(killer, killed);
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* killed) override
    {
        UpgradeCreatureLoot(petOwner, killed);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!config.Enabled || !config.Announce || !player)
            return;

        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        Map* map = player->FindMap();
        if (!map || !map->IsDungeon())
            return;

        ScalingInfo info;
        if (!GetScalingForPlayer(player, info, true))
            return;

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff4CFF00[IlvlScaling]|r ilvl moyen des joueurs : {:.0f} ({} joueur{}) — "
            "vie des monstres x{:.2f}, dégâts x{:.2f}, loot x{:.2f}",
            info.avgIlvl, info.realPlayers, info.realPlayers > 1 ? "s" : "",
            info.healthMult, info.damageMult, info.lootMult);
    }
};

using namespace Acore::ChatCommands;

class IlvlScaling_CommandScript : public CommandScript
{
public:
    IlvlScaling_CommandScript() : CommandScript("IlvlScaling_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "ilvlscaling", HandleIlvlScaling, SEC_PLAYER, Console::No }
        };
        return table;
    }

    static bool HandleIlvlScaling(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (!config.Enabled)
        {
            handler->SendSysMessage("|cff4CFF00[IlvlScaling]|r module désactivé.");
            return true;
        }

        ScalingInfo info;
        if (!GetScalingForPlayer(player, info, true))
        {
            handler->SendSysMessage(
                "|cff4CFF00[IlvlScaling]|r aucun vrai joueur dans le groupe sur cette carte : "
                "aucun scaling appliqué.");
            return true;
        }

        handler->PSendSysMessage(
            "|cff4CFF00[IlvlScaling]|r ilvl moyen : {:.1f} ({} joueur{}, niveau moyen {:.0f}) — "
            "vie des monstres x{:.2f}, dégâts x{:.2f}, loot x{:.2f}",
            info.avgIlvl, info.realPlayers, info.realPlayers > 1 ? "s" : "", info.avgLevel,
            info.healthMult, info.damageMult, info.lootMult);

        if (config.LootUpgradeEnable)
            handler->PSendSysMessage(
                "|cff4CFF00[IlvlScaling]|r objets : remplacés par des équivalents d'ilvl {:.0f} à {:.0f}",
                info.avgIlvl - float(config.LootUpgradeBand), info.avgIlvl);
        return true;
    }
};

void AddIlvlScalingScripts()
{
    new IlvlScaling_WorldScript();
    new IlvlScaling_UnitScript();
    new IlvlScaling_GlobalScript();
    new IlvlScaling_PlayerScript();
    new IlvlScaling_CommandScript();
}
