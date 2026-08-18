# mod-ilvl-scaling

Module AzerothCore (fork Playerbot) qui **augmente la difficulté et le loot**
en fonction de la **moyenne d'item level des vrais joueurs du groupe** — les
bots de mod-playerbots sont **exclus du calcul**, mais subissent le scaling
comme tout le monde.

Prévu pour le fork [liyunfan1223/azerothcore-wotlk](https://github.com/liyunfan1223/azerothcore-wotlk)
(branche Playerbot) : la détection des bots repose sur `WorldSession::IsBot()`,
fourni par ce fork. Aucune dépendance de compilation envers mod-playerbots.

## Principe

1. À chaque événement concerné, le module calcule (avec un cache de 30 s)
   l'**ilvl moyen équipé** des vrais joueurs du groupe présents sur la même
   carte (chemise/tabard exclus ; la main gauche n'est pas comptée si une arme
   à deux mains est équipée). Un joueur sans groupe compte pour lui-même.
2. Il en déduit un **delta** par rapport à l'ilvl « attendu » :
   `190` au niveau 80 (configurable), `niveau × 2.0` en dessous.
3. Chaque point d'ilvl au-dessus du seuil augmente trois multiplicateurs
   (bornés par des min/max configurables) :

   | Multiplicateur | Effet | Défaut |
   |---|---|---|
   | **Vie** | les dégâts infligés aux créatures sont **divisés** (vie effective accrue, barre de vie affichée inchangée) | +2 %/ilvl, max ×4 |
   | **Dégâts** | les dégâts des créatures (mêlée, sorts, DoT) sont **multipliés** | +1 %/ilvl, max ×3 |
   | **Loot** | or ramassé, chance de drop des entrées à pourcentage, nombre de jets sur les références (loot de boss, arrondi probabiliste) | +1 %/ilvl, max ×3 |

4. **Upgrade des objets** (`IlvlScaling.Loot.Upgrade.*`) : chaque arme/armure
   qui drop sur un cadavre de créature est remplacée par un objet aléatoire de
   même emplacement, même type (tissu/cuir/mailles/plaques, type d'arme…) et
   même qualité, choisi entre `moyenne − 15` et `moyenne` d'ilvl du groupe.
   Avec un groupe à ilvl 274, un donjon héroïque droppe donc du 259–274 au
   lieu du 200. Les objets de quête, drops conditionnels, tokens et emblèmes
   ne sont jamais touchés ; s'il n'existe aucun objet de la qualité voulue
   dans la fourchette (ex. les verts s'arrêtent vers l'ilvl 200), les
   meilleurs disponibles sous la moyenne sont utilisés. Les coffres ne sont
   pas concernés (seulement les cadavres de créatures).

Exemple : groupe de 2 vrais joueurs en gear ICC 25 (ilvl moyen ~264, delta 74)
→ vie des monstres ×2.48, dégâts ×1.74, loot ×1.74 — même si le reste du
groupe est composé de bots fraîchement montés.

Par défaut, le scaling (difficulté **et** loot) ne s'applique **qu'en
donjon/raid** (`IlvlScaling.OnlyInInstances = 0` pour l'étendre au monde
ouvert), et les monstres « gris » (9 niveaux sous la moyenne du groupe) y
échappent toujours.

## En jeu

- À l'entrée d'un donjon/raid, chaque vrai joueur voit un récapitulatif :
  `[IlvlScaling] ilvl moyen des joueurs : 245 (2 joueurs) — vie des monstres x2.10, dégâts x1.55, loot x1.55`
- La commande **`.ilvlscaling`** affiche à tout moment les valeurs actuelles
  (recalculées immédiatement, sans attendre l'expiration du cache).

## Installation

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/Maddnes95/mod-ilvl-scaling.git
```

Puis relancer CMake et recompiler le worldserver. Le fichier de configuration
`mod_ilvl_scaling.conf.dist` est copié automatiquement ; toutes les options
sont rechargeables en jeu avec `.reload config`.

## Notes de conception

- La vie accrue est appliquée en **réduisant les dégâts reçus** par les
  créatures plutôt qu'en modifiant leur vie max : cela s'adapte instantanément
  aux changements de groupe et n'altère aucune stat persistée.
- Les familiers/totems/gardiens des joueurs ne sont jamais scalés en tant que
  cibles, et leurs dégâts sont réduits comme ceux de leur propriétaire.
- Les objets de quête ne voient jamais leur chance de drop modifiée.
- L'ilvl moyen est mis en cache 30 s par (groupe, carte) : un changement
  d'équipement est pris en compte au plus tard après ce délai.

## Licence

[GNU GPL v2](LICENSE) — la même licence qu'AzerothCore.
