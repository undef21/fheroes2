/***************************************************************************
 *   Free Heroes of Might and Magic II: https://github.com/ihhub/fheroes2  *
 *   Copyright (C) 2020                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "ai_normal.h"
#include "artifact.h"
#include "battle_arena.h"
#include "battle_army.h"
#include "battle_board.h"
#include "battle_catapult.h"
#include "battle_cell.h"
#include "battle_command.h"
#include "battle_tower.h"
#include "battle_troop.h"
#include "castle.h"
#include "heroes.h"
using namespace Battle;

namespace AI
{
    // Usual distance between units at the start of the battle is 10-14 tiles
    // 20% of maximum value lost for every tile travelled to make sure 4 tiles difference matters
    const double STRENGTH_DISTANCE_FACTOR = 5.0;
    const std::vector<int> underWallsIndicies = {7, 28, 49, 72, 95};

    bool isHeroWorthSaving( const Heroes * hero )
    {
        return hero && ( hero->GetLevel() > 2 || !hero->GetBagArtifacts().empty() );
    }

    bool CheckCommanderCanSpellcast( const Arena & arena, const HeroBase * commander )
    {
        return commander && commander->HaveSpellBook() && !commander->Modes( Heroes::SPELLCASTED ) && !arena.isSpellcastDisabled();
    }

    bool CheckBattleRetreat( double myArmy, double enemy )
    {
        // FIXME: more sophisticated logic to see if remaining units are under threat
        // Consider taking speed/turn order into account as well
        // Pass in ( const Units & friendly, const Units & enemies ) instead

        // Retreat if remaining army strength is 10% of enemy's army
        return myArmy * 10 < enemy;
    }

    bool CheckIfUnitIsFaster( const Unit & currentUnit, const Unit & target )
    {
        if ( currentUnit.isFlying() == target.isFlying() )
            return currentUnit.GetSpeed() > target.GetSpeed();
        return currentUnit.isFlying();
    }

    void ForceSpellcastBeforeRetreat( Arena & arena, const HeroBase * commander, Actions & actions )
    {
        if ( CheckCommanderCanSpellcast( arena, commander ) ) {
            const std::vector<Spell> allSpells = commander->GetSpells();
            int bestSpell = -1;
            double bestHeuristic = 0;
            int targetIdx = -1;

            const Units friendly( arena.GetForce( commander->GetColor() ), true );
            const Units enemies( arena.GetForce( commander->GetColor(), true ), true );

            const int spellPower = commander->GetPower();
            for ( const Spell & spell : allSpells ) {
                if ( spell.isCombat() && spell.isDamage() && spell.isSingleTarget() ) {
                    const uint32_t totalDamage = spell.Damage() * spellPower;
                    for ( const Unit * enemy : enemies ) {
                        const double spellHeuristic
                            = enemy->GetMonsterStrength() * enemy->HowManyWillKilled( totalDamage * ( 100 - enemy->GetMagicResist( spell, spellPower ) ) / 100 );

                        if ( spellHeuristic > bestHeuristic ) {
                            bestHeuristic = spellHeuristic;
                            bestSpell = spell.GetID();
                            targetIdx = enemy->GetHeadIndex();
                        }
                    }
                }
            }

            if ( bestSpell != -1 ) {
                actions.push_back( Battle::Command( MSG_BATTLE_CAST, bestSpell, targetIdx ) );
            }
        }
    }

    void Normal::BattleTurn( Arena & arena, const Unit & currentUnit, Actions & actions )
    {
        const int difficulty = Settings::Get().GameDifficulty();
        const int myColor = currentUnit.GetColor();
        const uint32_t currentUnitMoveRange = currentUnit.isFlying() ? MAXU16 : currentUnit.GetSpeed();

        DEBUG( DBG_AI, DBG_TRACE, currentUnit.GetName() << " start their turn. Side: " << myColor );

        const HeroBase * commander = currentUnit.GetCommander();
        const Force & friendlyForce = arena.GetForce( myColor );
        const Force & enemyForce = arena.GetForce( myColor, true );

        // This should filter out all invalid units
        const Units friendly( friendlyForce, true );
        const Units enemies( enemyForce, true );
        const size_t enemiesCount = enemies.size();

        // Step 1. Friendly and enemy army analysis
        double myArmyStrength = 0;
        double enemyArmyStrength = 0;
        double myShooterStr = 0;
        double enemyShooterStr = 0;
        double averageAllyDefense = 0;
        double averageEnemyAttack = 0;
        int highestDamageExpected = 0;

        for ( Units::const_iterator it = enemies.begin(); it != enemies.end(); ++it ) {
            const Unit & unit = **it;
            const double unitStr = unit.GetStrength();

            enemyArmyStrength += unitStr;
            if ( unit.isArchers() ) {
                enemyShooterStr += unitStr;
            }

            const int dmg = unit.CalculateMaxDamage( currentUnit );
            if ( dmg > highestDamageExpected )
                highestDamageExpected = dmg;

            averageEnemyAttack += unit.GetAttack();
        }

        for ( Units::const_iterator it = friendly.begin(); it != friendly.end(); ++it ) {
            const Unit & unit = **it;
            const double unitStr = unit.GetStrength();

            myArmyStrength += unitStr;
            if ( unit.isArchers() ) {
                myShooterStr += unitStr;
            }

            averageAllyDefense += unit.GetDefense();
        }

        averageAllyDefense = ( enemiesCount > 0 ) ? averageAllyDefense / enemiesCount : 1;
        averageEnemyAttack = ( enemiesCount > 0 ) ? averageEnemyAttack / enemiesCount : 1;

        // Step 2. Add castle siege (and battle arena) modifiers
        bool attackingCastle = false;
        bool defendingCastle = false;
        const Castle * castle = arena.GetCastle();
        if ( castle ) {
            const bool attackerIgnoresCover = arena.GetForce1().GetCommander()->HasArtifact( Artifact::GOLDEN_BOW );

            auto getTowerStrength = [&currentUnit]( const Tower * tower ) { return ( tower && tower->isValid() ) ? tower->GetScoreQuality( currentUnit ) : 0; };

            double towerStr = getTowerStrength( arena.GetTower( TWR_CENTER ) );
            towerStr += getTowerStrength( arena.GetTower( TWR_LEFT ) );
            towerStr += getTowerStrength( arena.GetTower( TWR_RIGHT ) );
            DEBUG( DBG_AI, DBG_TRACE, "- Castle strength: " << towerStr );

            if ( myColor == castle->GetColor() ) {
                defendingCastle = true;
                myShooterStr += towerStr;
                if ( !attackerIgnoresCover )
                    enemyShooterStr /= 2;
            }
            else {
                attackingCastle = true;
                enemyShooterStr += towerStr;
                if ( !attackerIgnoresCover )
                    myShooterStr /= 2;
            }
        }
        DEBUG( DBG_AI, DBG_TRACE, "Comparing shooters: " << myShooterStr << ", vs enemy " << enemyShooterStr );

        // FIXME: Disabled defensive tactics (not implemented yet)
        // const bool defensiveTactics = defendingCastle || myShooterStr > enemyShooterStr;
        const bool defensiveTactics = false;

        // Step 3. Check retreat/surrender condition
        const Heroes * actualHero = dynamic_cast<const Heroes *>( commander );
        if ( actualHero && arena.CanRetreatOpponent( myColor ) && isHeroWorthSaving( actualHero ) && CheckBattleRetreat( myArmyStrength, enemyArmyStrength ) ) {
            // Cast maximum damage spell
            ForceSpellcastBeforeRetreat( arena, commander, actions );

            actions.push_back( Command( MSG_BATTLE_RETREAT ) );
            actions.push_back( Command( MSG_BATTLE_END_TURN, currentUnit.GetUID() ) );
            return;
        }

        // Step 4. Calculate spell heuristics
        if ( CheckCommanderCanSpellcast( arena, commander ) ) {
            // 1. For damage spells - maximum amount of enemy threat lost
            // 2. For buffs - friendly unit strength gained
            // 3. For debuffs - enemy unit threat lost
            // 4. For dispell, resurrect and cure - amount of unit strength recovered
            // 5. For antimagic - based on enemy hero spellcasting abilities multiplied by friendly unit strength

            // 6. Cast best spell with highest heuristic on target pointer saved

            // Temporary: force damage spell
            ForceSpellcastBeforeRetreat( arena, commander, actions );
        }

        // Step 5. Current unit decision tree
        const Unit * target = NULL;
        int targetCell = -1;

        if ( currentUnit.isArchers() ) {
            // Ranged unit decision tree
            if ( currentUnit.isHandFighting() ) {
                // Current ranged unit is blocked by the enemy

                // force archer to fight back by setting initial expectation to lowest possible (if we're losing battle)
                int bestOutcome = ( myArmyStrength < enemyArmyStrength ) ? -highestDamageExpected : 0;
                bool canOutrunEnemy = true;

                const Indexes & adjacentEnemies = Board::GetAdjacentEnemies( currentUnit );
                for ( const int cell : adjacentEnemies ) {
                    const Unit * enemy = Board::GetCell( cell )->GetUnit();
                    if ( enemy ) {
                        const int archerMeleeDmg = currentUnit.GetDamage( *enemy );
                        const int damageDiff = archerMeleeDmg - enemy->CalculateRetaliationDamage( archerMeleeDmg );

                        if ( bestOutcome < damageDiff ) {
                            bestOutcome = damageDiff;
                            target = enemy;
                            targetCell = cell;
                        }

                        // try to determine if it's worth running away (canOutrunEnemy stays true ONLY if all enemies are slower)
                        if ( canOutrunEnemy && !CheckIfUnitIsFaster( currentUnit, *enemy ) )
                            canOutrunEnemy = false;
                    }
                    else {
                        DEBUG( DBG_AI, DBG_WARN, "Board::GetAdjacentEnemies returned a cell " << cell << " that does not contain a unit!" );
                    }
                }

                if ( target && targetCell != -1 ) {
                    // attack selected target
                    DEBUG( DBG_AI, DBG_INFO, currentUnit.GetName() << " archer deciding to fight back: " << bestOutcome );
                    actions.push_back( Battle::Command( MSG_BATTLE_ATTACK, currentUnit.GetUID(), target->GetUID(), targetCell, 0 ) );
                }
                else if ( canOutrunEnemy ) {
                    // Kiting enemy
                    // Search for a safe spot unit can move away
                    DEBUG( DBG_AI, DBG_INFO, currentUnit.GetName() << " archer kiting enemy" );
                }
                // Worst case scenario - Skip turn
            }
            else {
                // Normal attack: focus the highest value unit
                for ( const Unit * enemy : enemies ) {
                    const double attackPriority = enemy->GetScoreQuality( currentUnit );
                    DEBUG( DBG_AI, DBG_TRACE, "-- Unit " << enemy->GetName() << " attack priority: " << attackPriority );

                    double highestStrength = 0;
                    if ( highestStrength < attackPriority ) {
                        highestStrength = attackPriority;
                        target = enemy;
                        DEBUG( DBG_AI, DBG_TRACE, "- Set priority on " << enemy->GetName() );
                    }
                }

                if ( target ) {
                    actions.push_back( Battle::Command( MSG_BATTLE_ATTACK, currentUnit.GetUID(), target->GetUID(), target->GetHeadIndex(), 0 ) );

                    DEBUG( DBG_AI, DBG_INFO,
                           currentUnit.GetName() << " archer focusing enemy " << target->GetName() << " threat level: " << target->GetScoreQuality( currentUnit ) );
                }
            }
        }
        else {
            if ( defensiveTactics ) {
                // Melee unit - Defensive action

                // Search for enemy units threatening our archers within range
                const bool archersUnderThreat = false;

                if ( archersUnderThreat ) {
                    DEBUG( DBG_AI, DBG_INFO,
                           currentUnit.GetName() << " defending against ..."
                                                 << " threat level: ..." );
                }
                else {
                    // Find best friendly archer and move to them
                    DEBUG( DBG_AI, DBG_INFO, currentUnit.GetName() << " protecting unit ..." );
                }
            }
            else {
                // Melee unit - Offensive action
                const double distanceModifier = enemyArmyStrength / STRENGTH_DISTANCE_FACTOR;
                double maxPriority = distanceModifier * ARENASIZE * -1;
                double highestStrength = 0;

                for ( const Unit * enemy : enemies ) {
                    // move node pair consists of move hex index and distance
                    const std::pair<int, uint32_t> move = arena.CalculateMoveToUnit( *enemy );
                    const double enemyValue = enemy->GetScoreQuality( currentUnit );

                    if ( move.first != -1 && move.second <= currentUnitMoveRange && highestStrength < enemyValue ) {
                        highestStrength = enemyValue;
                        target = enemy;

                        if ( currentUnit.isFlying() ) {
                            // look for valid adjacent hex
                            const Indexes & around = Board::GetAroundIndexes( *target );

                            int validCell = -1;
                            for ( const int cell : around ) {
                                if ( arena.hexIsPassable( cell ) ) {
                                    validCell = cell;
                                    break;
                                }
                            }

                            // it must have one, otherwise ArenaPathfinder is bugged
                            if ( validCell == -1 ) {
                                DEBUG( DBG_AI, DBG_WARN, "Flyer path error while targeting enemy " << enemy->GetName() << ". Check pathfinder." );
                            }
                            else {
                                targetCell = validCell;
                            }
                        }
                        else {
                            targetCell = move.first;
                        }
                    }
                    else if ( target == NULL ) {
                        // For walking units that don't have a target within reach, pick based on distance priority
                        const double unitPriority = enemyValue - move.second * distanceModifier;
                        if ( unitPriority > maxPriority ) {
                            maxPriority = unitPriority;
                            targetCell = move.first;
                        }
                    }
                }

                // Walkers: move closer to the castle walls during siege
                if ( attackingCastle && targetCell == -1 ) {
                    uint32_t shortestDist = MAXU16;

                    for ( const int wallIndex : underWallsIndicies ) {
                        const uint32_t dist = arena.CalculateMoveDistance( wallIndex );
                        if ( dist < shortestDist ) {
                            shortestDist = dist;
                            targetCell = wallIndex;
                        }
                    }
                    DEBUG( DBG_AI, DBG_INFO, "Walker unit moving towards castle walls " << currentUnit.GetName() << " cell " << targetCell );
                }

                DEBUG( DBG_AI, DBG_INFO, "Melee phase end, targetCell is " << targetCell );
                if ( targetCell != -1 ) {
                    if ( currentUnit.GetHeadIndex() != targetCell )
                        actions.push_back( Battle::Command( MSG_BATTLE_MOVE, currentUnit.GetUID(), targetCell ) );

                    if ( target ) {
                        actions.push_back( Battle::Command( MSG_BATTLE_ATTACK, currentUnit.GetUID(), target->GetUID(), target->GetHeadIndex(), 0 ) );
                        DEBUG( DBG_AI, DBG_INFO,
                               currentUnit.GetName() << " melee offense, focus enemy " << target->GetName()
                                                     << " threat level: " << target->GetScoreQuality( currentUnit ) );
                    }
                }
                // else skip
            }
        }

        actions.push_back( Battle::Command( MSG_BATTLE_END_TURN, currentUnit.GetUID() ) );
    }
}
