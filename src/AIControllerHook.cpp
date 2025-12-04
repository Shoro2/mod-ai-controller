#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Log.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "ObjectAccessor.h"
#include <cmath>
#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <queue>
#include <sstream>

#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Cell.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Bag.h"

using boost::asio::ip::tcp;

struct AICommand {
    std::string playerName;
    std::string actionType;
    std::string value;
};

std::mutex g_Mutex;
std::string g_CurrentJsonState = "{}";
bool g_HasNewState = false;
std::queue<AICommand> g_CommandQueue;

std::mutex g_EventMutex;
long long g_XPGained = 0;
bool g_LeveledUp = false;
long long g_LootCopper = 0;
long long g_LootScore = 0;
bool g_EquippedUpgrade = false; // NEU: Flag für Python

// --- HELPER: ITEM SCORE ---
// Berechnet einen simplen Wert für ein Item (Level 1-20 Logik)
int GetItemScore(ItemTemplate const* proto) {
    if (!proto) return 0;
    int score = 0;

    // Basis-Punkte für Qualität
    score += proto->Quality * 10;
    // Basis-Punkte für ItemLevel
    score += proto->ItemLevel;
    // Rüstungswert
    score += proto->Armor;
    // DPS bei Waffen (x10 Gewichtung)
    if (proto->Class == ITEM_CLASS_WEAPON) {
        score += (int)(proto->Damage[0].DamageMax + proto->Damage[0].DamageMin);
    }

    // Stats (Stärke, Ausdauer, Int etc.)
    for (int i = 0; i < proto->StatsCount; ++i) {
        score += proto->ItemStat[i].ItemStatValue * 2;
    }
    return score;
}

// --- HELPER: AUTO EQUIP ---
void TryEquipIfBetter(Player* player, uint16 srcPos) {
    // srcPos ist der absolute Slot (z.B. BagIndex + SlotIndex)
    // Wir holen das Item über den absoluten Slot (GetItemByPos kann das oft)
    // Aber warte: GetItemByPos nimmt (bag, slot). Wir müssen umrechnen.

    // Umrechnung: Absoluter Slot -> Bag & Slot
    uint8 bag = srcPos >> 8;
    uint8 slot = srcPos & 255;

    Item* newItem = player->GetItemByPos(bag, slot);
    if (!newItem) return;

    if (player->CanUseItem(newItem) != EQUIP_ERR_OK) return;

    ItemTemplate const* proto = newItem->GetTemplate();
    uint16 destSlot = 0xffff;

    // Slot Mapping (wie vorher)
    switch (proto->InventoryType) {
    case INVTYPE_HEAD: destSlot = EQUIPMENT_SLOT_HEAD; break;
    case INVTYPE_SHOULDERS: destSlot = EQUIPMENT_SLOT_SHOULDERS; break;
    case INVTYPE_BODY:
    case INVTYPE_CHEST:
    case INVTYPE_ROBE: destSlot = EQUIPMENT_SLOT_CHEST; break;
    case INVTYPE_WAIST: destSlot = EQUIPMENT_SLOT_WAIST; break;
    case INVTYPE_LEGS: destSlot = EQUIPMENT_SLOT_LEGS; break;
    case INVTYPE_FEET: destSlot = EQUIPMENT_SLOT_FEET; break;
    case INVTYPE_WRISTS: destSlot = EQUIPMENT_SLOT_WRISTS; break;
    case INVTYPE_HANDS: destSlot = EQUIPMENT_SLOT_HANDS; break;
    case INVTYPE_WEAPON:
    case INVTYPE_2HWEAPON:
    case INVTYPE_WEAPONMAINHAND: destSlot = EQUIPMENT_SLOT_MAINHAND; break;
    case INVTYPE_SHIELD:
    case INVTYPE_WEAPONOFFHAND: destSlot = EQUIPMENT_SLOT_OFFHAND; break;
    }

    if (destSlot != 0xffff) {
        int newScore = GetItemScore(proto);
        int currentScore = -1;

        Item* currentItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, destSlot);
        if (currentItem) {
            currentScore = GetItemScore(currentItem->GetTemplate());
        }

        if (newScore > currentScore) {
            // FIX: SwapItem mit nur 2 Argumenten aufrufen!
            // srcPos haben wir schon.
            // destSlot ist der Equipment-Slot (0..18). Da Equipment immer in Bag 0 ist (oder "kein Bag"),
            // ist der absolute Slot gleich dem Index (0..18).

            // Player::SwapItem(src, dst)
            player->SwapItem(srcPos, destSlot);

            player->PlayDistanceSound(120, player);

            {
                std::lock_guard<std::mutex> lock(g_EventMutex);
                g_EquippedUpgrade = true;
            }

            LOG_INFO("module", "AI-GEAR: Upgrade angelegt! (Slot: {})", destSlot);
        }
    }
}

// --- COLLECTOR ---
class CreatureCollector {
public:
    std::vector<Creature*> foundCreatures;
    Player* i_player;

    CreatureCollector(Player* player) : i_player(player) {}

    void Visit(CreatureMapType& m) {
        for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr) {
            Creature* creature = itr->GetSource();
            if (creature) {
                if (!creature->IsInWorld()) continue;
                if (creature->IsTotem() || creature->IsPet()) continue;
                if (creature->GetCreatureTemplate()->type == CREATURE_TYPE_CRITTER) continue;

                if (i_player->GetDistance(creature) > 50.0f) continue;

                foundCreatures.push_back(creature);
            }
        }
    }

    template<class SKIP> void Visit(GridRefMgr<SKIP>&) {}
};

// --- HELPER: BAGS ---
uint32 GetFreeBagSlots(Player* player) {
    uint32 freeSlots = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot) {
        if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) freeSlots++;
    }
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
        Bag* bagItem = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (bagItem) freeSlots += bagItem->GetFreeSlots();
    }
    return freeSlots;
}

// --- SERVER THREAD ---
void AIServerThread() {
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 5000));
        LOG_INFO("module", ">>> AI-SOCKET: Lausche auf Port 5000... <<<");

        while (true) {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            LOG_INFO("module", ">>> CLIENT VERBUNDEN! <<<");

            try {
                char data_[8192];
                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(g_Mutex);
                        if (g_HasNewState) {
                            std::string msg = g_CurrentJsonState + "\n";
                            boost::asio::write(socket, boost::asio::buffer(msg));
                            g_HasNewState = false;
                        }
                    }

                    if (socket.available() > 0) {
                        boost::system::error_code error;
                        size_t length = socket.read_some(boost::asio::buffer(data_), error);
                        if (error == boost::asio::error::eof) break;

                        std::string receivedData(data_, length);
                        size_t p1 = receivedData.find(':');
                        size_t p2 = receivedData.find(':', p1 + 1);
                        if (p1 != std::string::npos && p2 != std::string::npos) {
                            AICommand cmd;
                            cmd.playerName = receivedData.substr(0, p1);
                            cmd.actionType = receivedData.substr(p1 + 1, p2 - p1 - 1);
                            cmd.value = receivedData.substr(p2 + 1);

                            std::lock_guard<std::mutex> lock(g_Mutex);
                            g_CommandQueue.push(cmd);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            catch (std::exception& e) {
                LOG_ERROR("module", "Verbindung verloren: {}", e.what());
            }
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("module", "Server Crash: {}", e.what());
    }
}

// --- LOGIC ---
class AIControllerWorldScript : public WorldScript {
private:
    uint32 _fastTimer;
    uint32 _slowTimer;
    uint32 _faceTimer;
    std::string _cachedNearbyMobsJson;

public:
    AIControllerWorldScript() : WorldScript("AIControllerWorldScript"), _fastTimer(0), _slowTimer(0), _faceTimer(0), _cachedNearbyMobsJson("[]") {}

    void OnStartup() override {
        std::thread(AIServerThread).detach();
    }

    void OnUpdate(uint32 diff) override {
        _fastTimer += diff;
        _slowTimer += diff;
        _faceTimer += diff;

        // Auto-Tracking
        if (_faceTimer >= 150) {
            _faceTimer = 0;
            auto const& sessions = sWorldSessionMgr->GetAllSessions();
            for (auto const& pair : sessions) {
                Player* p = pair.second->GetPlayer();
                if (!p) continue;
                if (p->IsInCombat() || p->HasUnitState(UNIT_STATE_CASTING)) {
                    Unit* target = p->GetSelectedUnit();
                    if (target) p->SetFacingToObject(target);
                }
            }
        }

        // Commands
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            while (!g_CommandQueue.empty()) {
                AICommand cmd = g_CommandQueue.front();
                g_CommandQueue.pop();

                Player* player = ObjectAccessor::FindPlayerByName(cmd.playerName);
                if (!player) continue;

                if (cmd.actionType == "say") player->Say(cmd.value, LANG_UNIVERSAL);
                else if (cmd.actionType == "stop") {
                    player->GetMotionMaster()->Clear();
                    player->GetMotionMaster()->MoveIdle();
                }
                else if (cmd.actionType == "turn_left" || cmd.actionType == "turn_right") {
                    float o = player->GetOrientation();
                    float step = (cmd.actionType == "turn_left") ? 0.5f : -0.5f;
                    o += step;
                    if (o > 6.283f) o -= 6.283f;
                    if (o < 0) o += 6.283f;
                    player->SetFacingTo(o);
                }
                else if (cmd.actionType == "move_forward") {
                    float o = player->GetOrientation();
                    float x = player->GetPositionX() + (3.0f * std::cos(o));
                    float y = player->GetPositionY() + (3.0f * std::sin(o));
                    float z = player->GetPositionZ();
                    player->UpdateGroundPositionZ(x, y, z);
                    player->GetMotionMaster()->MovePoint(1, x, y, z);
                }
                else if (cmd.actionType == "cast") {
                    uint32 spellId = std::stoi(cmd.value);
                    Unit* target = player->GetSelectedUnit();

                    if (spellId == 2050) target = player;
                    else if (spellId == 585) {
                        if (!target || target == player) {
                            target = player->SelectNearbyTarget(nullptr, 30.0f);
                            if (target && !player->IsValidAttackTarget(target)) target = nullptr;
                        }
                    }
                    else if (!target) target = player;

                    if (target) {
                        if (spellId == 585 && target == player) {} // Do nothing
                        else {
                            LOG_INFO("module", "AI-CAST: {} wirkt Spell {} auf {}", player->GetName(), spellId, target->GetName());
                            player->CastSpell(target, spellId, false);
                        }
                    }
                }
                else if (cmd.actionType == "reset") {
                    player->CombatStop(true);
                    player->AttackStop();
                    player->GetMotionMaster()->Clear();
                    if (!player->isDead()) {
                        player->ResurrectPlayer(1.0f, false);
                        player->SpawnCorpseBones();
                    }
                    player->SetHealth(player->GetMaxHealth());
                    player->SetPower(player->getPowerType(), player->GetMaxPower(player->getPowerType()));
                    player->RemoveAllSpellCooldown();
                    player->RemoveAllAuras();
                    player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, player->GetOrientation());
                }
                else if (cmd.actionType == "move_to") {
                    std::string val = cmd.value;
                    size_t p1 = val.find(':');
                    size_t p2 = val.find(':', p1 + 1);
                    if (p1 != std::string::npos && p2 != std::string::npos) {
                        float tx = std::stof(val.substr(0, p1));
                        float ty = std::stof(val.substr(p1 + 1, p2 - p1 - 1));
                        float tz = std::stof(val.substr(p2 + 1));
                        player->UpdateGroundPositionZ(tx, ty, tz);
                        Position pos(tx, ty, tz, 0.0f);
                        player->GetMotionMaster()->MovePoint(1, pos, (ForcedMovement)0, 0.0f, true);
                    }
                }
                else if (cmd.actionType == "target_guid") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Unit* target = ObjectAccessor::GetUnit(*player, guid);
                    if (target) {
                        player->SetSelection(target->GetGUID());
                        player->SetTarget(target->GetGUID());
                        player->SetFacingToObject(target);
                        player->AttackStop();
                    }
                }
                else if (cmd.actionType == "loot_guid") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Creature* target = ObjectAccessor::GetCreature(*player, guid);

                    if (target && target->isDead()) {
                        if (player->GetDistance(target) <= 10.0f) {

                            player->SendLoot(target->GetGUID(), LOOT_CORPSE);
                            Loot* loot = &target->loot;

                            // Gold
                            uint32 gold = loot->gold;
                            if (gold > 0) {
                                loot->gold = 0;
                                player->ModifyMoney(gold);
                                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);
                                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                                data << uint32(gold);
                                data << uint8(1);
                                player->GetSession()->SendPacket(&data);
                                {
                                    std::lock_guard<std::mutex> lock(g_EventMutex);
                                    g_LootCopper += gold;
                                }
                            }
                            // Items
                            for (uint8 i = 0; i < loot->items.size(); ++i) {
                                LootItem* item = loot->LootItemInSlot(i, player);
                                if (item && !item->is_looted && !item->freeforall && !item->needs_quest) {
                                    ItemPosCountVec dest;
                                    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid, item->count);

                                    if (msg == EQUIP_ERR_OK) {
                                        Item* newItem = player->StoreNewItem(dest, item->itemid, true);
                                        item->count = 0;
                                        item->is_looted = true;
                                        if (newItem) player->SendNewItem(newItem, 1, false, true);

                                        // CHECK FOR UPGRADE!
                                        // Wir prüfen, ob wir das Item, das wir gerade bekommen haben, anziehen wollen
                                        // dest.GetBag() ist 255 (NULL_BAG) für Rucksack
                                        // dest.GetPos() ist der Slot
                                        if (msg == EQUIP_ERR_OK) {
                                            Item* newItem = player->StoreNewItem(dest, item->itemid, true);
                                            item->count = 0;
                                            item->is_looted = true;
                                            if (newItem) player->SendNewItem(newItem, 1, false, true);

                                            // FIX: Wir nutzen direkt den Member '.pos' von ItemPosCount
                                            // dest ist ein Vector<ItemPosCount>. dest[0] ist das erste Element.
                                            TryEquipIfBetter(player, dest[0].pos);
                                        }
                                        // Score Update
                                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
                                        if (proto) {
                                            std::lock_guard<std::mutex> lock(g_EventMutex);
                                            g_LootScore += 1;
                                        }
                                    }
                                }
                            }

                            target->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
                            target->AllLootRemovedFromCorpse();
                            player->SendLootRelease(player->GetLootGUID());

                            player->SetSelection(ObjectGuid::Empty);
                            player->SetTarget(ObjectGuid::Empty);
                            player->AttackStop();
                        }
                    }
                }
                else if (cmd.actionType == "sell_grey") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Creature* vendor = ObjectAccessor::GetCreature(*player, guid);

                    if (vendor && player->GetDistance(vendor) <= 15.0f) {
                        player->StopMoving();
                        uint32 totalMoney = 0;

                        // Rucksack
                        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i) {
                            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) {
                                ItemTemplate const* proto = item->GetTemplate();
                                if (proto->SellPrice > 0 && proto->ItemId != 6948) {
                                    uint32 price = proto->SellPrice * item->GetCount();
                                    totalMoney += price;
                                    player->DestroyItem(INVENTORY_SLOT_BAG_0, i, true);
                                }
                            }
                        }
                        // Taschen
                        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
                            if (Bag* bagItem = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)) {
                                for (uint8 i = 0; i < bagItem->GetBagSize(); ++i) {
                                    if (Item* item = bagItem->GetItemByPos(i)) {
                                        ItemTemplate const* proto = item->GetTemplate();
                                        if (proto->SellPrice > 0 && proto->ItemId != 6948) {
                                            uint32 price = proto->SellPrice * item->GetCount();
                                            totalMoney += price;
                                            player->DestroyItem(bag, i, true);
                                        }
                                    }
                                }
                            }
                        }

                        if (totalMoney > 0) {
                            player->ModifyMoney(totalMoney);
                            player->PlayDistanceSound(120, player);
                            {
                                std::lock_guard<std::mutex> lock(g_EventMutex);
                                g_LootCopper += totalMoney;
                            }
                        }
                        player->SetSelection(ObjectGuid::Empty);
                        player->SetTarget(ObjectGuid::Empty);
                    }
                }
            }
        }

        if (_fastTimer >= 400) {
            _fastTimer = 0;
            long long xp = 0;
            long long lCopper = 0;
            long long lScore = 0;
            bool lvlUp = false;
            bool equipUp = false;

            {
                std::lock_guard<std::mutex> lock(g_EventMutex);
                xp = g_XPGained;
                g_XPGained = 0;
                lvlUp = g_LeveledUp;
                g_LeveledUp = false;
                lCopper = g_LootCopper;
                g_LootCopper = 0;
                lScore = g_LootScore;
                g_LootScore = 0;

                equipUp = g_EquippedUpgrade;
                g_EquippedUpgrade = false; // Reset
            }

            std::stringstream ss;
            ss << "{ \"players\": [";
            bool first = true;
            auto const& sessions = sWorldSessionMgr->GetAllSessions();
            for (auto const& pair : sessions) {
                WorldSession* session = pair.second;
                if (!session) continue;
                Player* p = session->GetPlayer();
                if (!p) continue;

                if (!first) ss << ", ";
                first = false;

                ss << "{";
                ss << "\"name\": \"" << p->GetName() << "\", ";
                ss << "\"hp\": " << p->GetHealth() << ", ";
                ss << "\"max_hp\": " << p->GetMaxHealth() << ", ";
                ss << "\"power\": " << p->GetPower(p->getPowerType()) << ", ";
                ss << "\"max_power\": " << p->GetMaxPower(p->getPowerType()) << ", ";
                ss << "\"level\": " << (int)p->GetLevel() << ", ";
                ss << "\"x\": " << p->GetPositionX() << ", ";
                ss << "\"y\": " << p->GetPositionY() << ", ";
                ss << "\"z\": " << p->GetPositionZ() << ", ";
                ss << "\"o\": " << p->GetOrientation() << ", ";
                ss << "\"combat\": \"" << (p->IsInCombat() ? "true" : "false") << "\", ";
                ss << "\"casting\": \"" << (p->HasUnitState(UNIT_STATE_CASTING) ? "true" : "false") << "\", ";
                ss << "\"free_slots\": " << GetFreeBagSlots(p) << ", ";

                Unit* target = p->GetSelectedUnit();
                std::string tStatus = "none";
                uint32 tHp = 0;
                float tx = 0, ty = 0, tz = 0;
                if (target) {
                    tStatus = target->IsAlive() ? "alive" : "dead";
                    tHp = target->GetHealth();
                    tx = target->GetPositionX();
                    ty = target->GetPositionY();
                    tz = target->GetPositionZ();
                }
                ss << "\"target_status\": \"" << tStatus << "\", ";
                ss << "\"target_hp\": " << tHp << ", ";
                ss << "\"xp_gained\": " << xp << ", ";
                ss << "\"loot_copper\": " << lCopper << ", ";
                ss << "\"loot_score\": " << lScore << ", ";
                ss << "\"equipped_upgrade\": \"" << (equipUp ? "true" : "false") << "\", "; // NEU
                ss << "\"leveled_up\": \"" << (lvlUp ? "true" : "false") << "\", ";
                ss << "\"tx\": " << tx << ", ";
                ss << "\"ty\": " << ty << ", ";
                ss << "\"tz\": " << tz << ", ";
                if (_cachedNearbyMobsJson.empty()) _cachedNearbyMobsJson = "[]";
                ss << "\"nearby_mobs\": " << _cachedNearbyMobsJson;
                ss << "}";
            }
            ss << "] }";
            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                g_CurrentJsonState = ss.str();
                g_HasNewState = true;
            }
        }

        if (_slowTimer >= 2000) {
            _slowTimer = 0;
            auto const& sessions = sWorldSessionMgr->GetAllSessions();
            if (!sessions.empty()) {
                for (auto const& pair : sessions) {
                    Player* p = pair.second->GetPlayer();
                    if (p) {
                        CreatureCollector collector(p);
                        Cell::VisitObjects(p, collector, 50.0f);
                        std::stringstream mobSS;
                        mobSS << "[";
                        bool firstMob = true;
                        for (Creature* c : collector.foundCreatures) {
                            if (!firstMob) mobSS << ", ";
                            mobSS << "{";
                            mobSS << "\"guid\": \"" << c->GetGUID().GetRawValue() << "\", ";
                            mobSS << "\"name\": \"" << c->GetName() << "\", ";
                            mobSS << "\"level\": " << (int)c->GetLevel() << ", ";
                            mobSS << "\"attackable\": " << (p->IsValidAttackTarget(c) ? "1" : "0") << ", ";
                            mobSS << "\"vendor\": " << (c->IsVendor() ? "1" : "0") << ", ";

                            // NEU: Wer ist das Ziel dieses Mobs? (für Aggro-Erkennung in Python)
                            uint64 targetGuid = 0;
                            if (c->GetTarget()) targetGuid = c->GetTarget().GetRawValue();
                            mobSS << "\"target\": \"" << targetGuid << "\", ";

                            mobSS << "\"hp\": " << c->GetHealth() << ", ";
                            mobSS << "\"x\": " << c->GetPositionX() << ", ";
                            mobSS << "\"y\": " << c->GetPositionY() << ", ";
                            mobSS << "\"z\": " << c->GetPositionZ();
                            mobSS << "}";
                            firstMob = false;
                        }
                        mobSS << "]";
                        _cachedNearbyMobsJson = mobSS.str();
                        break;
                    }
                }
            }
        }
    }
};

class AIControllerPlayerScript : public PlayerScript {
public:
    AIControllerPlayerScript() : PlayerScript("AIControllerPlayerScript") {}
    void OnPlayerLogin(Player* player) override {}
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override {
        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_XPGained += amount;
    }
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override {
        if (player->GetLevel() >= 2) {
            {
                std::lock_guard<std::mutex> lock(g_EventMutex);
                g_LeveledUp = true;
            }
            player->SetLevel(1);
            player->SetUInt32Value(PLAYER_XP, 0);
            player->InitStatsForLevel(true);
            player->SetHealth(player->GetMaxHealth());
            player->SetPower(player->getPowerType(), player->GetMaxPower(player->getPowerType()));
        }
    }
    void OnPlayerMoneyChanged(Player* player, int32& amount) override {
        if (amount > 0) {
            std::lock_guard<std::mutex> lock(g_EventMutex);
            g_LootCopper += amount;
        }
    }
    // WICHTIG: Kein OnPlayerLootItem hier, wir machen es oben im loot_guid block manuell
};

void AddAIControllerScripts() {
    new AIControllerPlayerScript();
    new AIControllerWorldScript();
}
