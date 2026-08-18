/*
 * mod-ilvl-scaling — difficulté et loot dynamiques basés sur l'ilvl moyen
 * des VRAIS joueurs du groupe (les bots de mod-playerbots sont ignorés).
 */

#ifndef MOD_ILVL_SCALING_H
#define MOD_ILVL_SCALING_H

#include "Define.h"

class Player;

namespace ModIlvlScaling
{
struct Settings
{
    bool   Enabled;
    bool   Announce;
    bool   OnlyInInstances;
    bool   IgnoreTrivial;
    uint32 TrivialLevelDiff;
    uint32 CacheSeconds;

    // ilvl « attendu » : en dessous, aucun bonus ; au-dessus, chaque point
    // d'ilvl ajoute PerIlvl à chaque multiplicateur.
    float BaselineIlvl80;
    float BaselinePerLevel;

    float HealthPerIlvl, HealthMin, HealthMax;
    float DamagePerIlvl, DamageMin, DamageMax;
    float LootPerIlvl, LootMin, LootMax;

    bool LootScaleMoney;
    bool LootScaleDropChance;
    bool LootScaleReferenceRolls;
};

extern Settings config;

struct ScalingInfo
{
    float  avgIlvl     = 0.f;
    float  avgLevel    = 0.f;
    uint32 realPlayers = 0;
    float  healthMult  = 1.f; // appliqué en divisant les dégâts subis par la créature
    float  damageMult  = 1.f; // appliqué aux dégâts infligés par la créature
    float  lootMult    = 1.f; // or, chances de drop, jets de références
};

// Calcule (avec cache) le scaling s'appliquant au groupe du joueur. Seuls
// les vrais joueurs (session non-bot) présents sur la même carte que
// `player` sont comptés dans la moyenne. Retourne false s'il n'y a aucun
// vrai joueur concerné (donc aucun scaling à appliquer).
bool GetScalingForPlayer(Player* player, ScalingInfo& out, bool forceRefresh = false);

// Moyenne d'ilvl de l'équipement porté (chemise/tabard exclus, main gauche
// non attendue si arme à deux mains équipée).
float AverageEquippedItemLevel(Player* player);

void ClearCache();
} // namespace ModIlvlScaling

#endif // MOD_ILVL_SCALING_H
