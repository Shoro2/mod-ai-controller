#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Log.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "WorldSocket.h" 
#include "ObjectAccessor.h"
#include <cmath>
#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <queue>
#include <sstream>
#include <future> 
#include <set> // Added for std::set
#include "GameTime.h" 

#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Cell.h"
#include "Item.h"   
#include "ItemTemplate.h"
#include "Bag.h"
#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "PreparedStatement.h"
#include "QueryHolder.h"
#include "DatabaseWorkerPool.h"

// WICHTIG: Zuerst MySQLConnection, dann CharacterDatabase
#include "MySQLConnection.h"

// Geht auch so
#include "CharacterDatabase.h"

using boost::asio::ip::tcp;
using CharacterDatabasePreparedStatement = PreparedStatement<CharacterDatabaseConnection>;

// HINWEIS: enum PlayerLoginQueryIndex wurde entfernt, da es bereits in Player.h definiert ist.

// --- Bot Login Helper (Nachbau von LoginQueryHolder aus CharacterHandler.cpp) ---
// ACHTUNG: Wir nutzen hier eine synchrone Implementierung!
class BotLoginQueryHolder : public SQLQueryHolder<CharacterDatabaseConnection> {
private:
    uint32 m_accountId;
    ObjectGuid m_guid;
public:
    BotLoginQueryHolder(uint32 accountId, ObjectGuid guid)
        : m_accountId(accountId), m_guid(guid) {
    }

    // Gibt die GUID zurück (wichtig für HandlePlayerLoginFromDB)
    ObjectGuid GetGuid() const { return m_guid; }

    bool Initialize() {
        SetSize(MAX_PLAYER_LOGIN_QUERY);

        bool res = true;
        ObjectGuid::LowType lowGuid = m_guid.GetCounter();

        // SKIP LIST: Hier können IDs eingetragen werden, die "Assertion Failed" verursachen.
        // 35 = CHAR_SEL_CHARACTER_AURAS (Verursachte den Crash)
        // Falls 36 (Spells) auch crasht, einfach 36 hinzufügen.
        std::set<int> skipList = {
            CHAR_SEL_CHARACTER_AURAS
            //, CHAR_SEL_CHARACTER_SPELL // Uncomment if Spells crash too
        };

        // Helper Lambda für weniger Code-Duplizierung
        // FIX: Wir nutzen CharacterDatabase.Query(stmt).get() für synchrone Ausführung
        auto LoadQuery = [&](CharacterDatabaseStatements index, PlayerLoginQueryIndex holderIndex) -> bool {

            // 1. Check Skip List
            if (skipList.find(index) != skipList.end()) {
                LOG_WARN("module", "AI-DB: Skipping Query Index {} intentionally to prevent crash.", (int)index);
                return false;
            }

            // SECURITY CHECK: Verhindert "Statement not found" Assertions/Crashes VOR dem Abruf
            CharacterDatabasePreparedStatement* stmt = nullptr;
            try {
                stmt = CharacterDatabase.GetPreparedStatement(index);
            }
            catch (...) {
                LOG_ERROR("module", "AI-DB: Exception getting PreparedStatement index {}", (int)index);
                return false;
            }

            if (!stmt) {
                // Das Statement existiert nicht (z.B. falscher Index oder DB Version mismatch). 
                // Wir loggen es und brechen ab, aber crashen nicht.
                LOG_ERROR("module", "AI-DB: Statement {} skipped (not found/null) - Check DB definitions!", (int)index);
                return false;
            }

            stmt->SetData(0, lowGuid);

            PreparedResultSet* result = nullptr;
            try {
                // Future abwarten und Ergebnis holen
                // HIER passierte der "m_mStmt" Assert Crash, weil das Statement auf der Connection fehlte.
                // Durch die skipList oben verhindern wir das Ausführen für bekannte Problem-Statements.
                auto futureResult = CharacterDatabase.Query(stmt);
                result = futureResult.get();
            }
            catch (std::exception& e) {
                LOG_ERROR("module", "AIController: Exception during DB Query execution (Idx {}): {}", (int)index, e.what());
                return false;
            }
            catch (...) {
                LOG_ERROR("module", "AIController: Unknown Exception during DB Query execution (Idx {}).", (int)index);
                return false;
            }

            // CRASH FIX für Vector Subscript:
            // Auch ein leeres Ergebnis (RowCount == 0) ist ein gültiges Ergebnis (z.B. leeres Inventar).
            // Wir müssen es an SetPreparedResult übergeben, damit LoadFromDB weiß "Aha, Inventar ist leer" 
            // statt "Huch, Inventar wurde nicht geladen -> Crash".

            if (!result) {
                // Wenn NULL zurückkommt (Query failed), dann geben wir false zurück.
                // LoadFromDB wird diesen Teil überspringen.
                return false;
            }

            // Wir übergeben das Ergebnis IMMER, auch wenn es 0 Zeilen hat.
            SetPreparedResult(holderIndex, result);
            return true;
            };

        // Das wichtigste Statement: Basis-Infos (Name, Rasse, Klasse etc.)
        if (!LoadQuery(CHAR_SEL_CHARACTER_AT_LOGIN, PLAYER_LOGIN_QUERY_LOAD_FROM))
        {
            LOG_ERROR("module", "AIController: Failed to load basic character data for GUID {} (Query returned null)", lowGuid);
            return false;
        }

        // --- RESTORED CORE LOGIC ---
        // Wir laden nun alle Standard-Daten.
        // Problemfälle (wie AURAS) werden durch die skipList oben abgefangen.
        // WICHTIG: Inventory und Skills MÜSSEN geladen werden (auch wenn leer), sonst droht "Vector subscript out of range".

        LoadQuery(CHAR_SEL_CHARACTER_AURAS, PLAYER_LOGIN_QUERY_LOAD_AURAS); // Wird geskippt durch skipList
        LoadQuery(CHAR_SEL_CHARACTER_SPELL, PLAYER_LOGIN_QUERY_LOAD_SPELLS);
        LoadQuery(CHAR_SEL_CHARACTER_QUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS);
        LoadQuery(CHAR_SEL_CHARACTER_DAILYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_DAILY_QUEST_STATUS);
        LoadQuery(CHAR_SEL_CHARACTER_WEEKLYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_WEEKLY_QUEST_STATUS);
        LoadQuery(CHAR_SEL_CHARACTER_MONTHLYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_MONTHLY_QUEST_STATUS);
        LoadQuery(CHAR_SEL_CHARACTER_SEASONALQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_SEASONAL_QUEST_STATUS);
        LoadQuery(CHAR_SEL_CHARACTER_REPUTATION, PLAYER_LOGIN_QUERY_LOAD_REPUTATION);
        LoadQuery(CHAR_SEL_CHARACTER_INVENTORY, PLAYER_LOGIN_QUERY_LOAD_INVENTORY); // Kritisch für Vector Crash!
        LoadQuery(CHAR_SEL_CHARACTER_ACTIONS, PLAYER_LOGIN_QUERY_LOAD_ACTIONS);
        LoadQuery(CHAR_SEL_CHARACTER_SKILLS, PLAYER_LOGIN_QUERY_LOAD_SKILLS); // Kritisch!

        LoadQuery(CHAR_SEL_CHARACTER_EQUIPMENTSETS, PLAYER_LOGIN_QUERY_LOAD_EQUIPMENT_SETS);
        LoadQuery(CHAR_SEL_CHARACTER_GLYPHS, PLAYER_LOGIN_QUERY_LOAD_GLYPHS);
        LoadQuery(CHAR_SEL_CHARACTER_TALENTS, PLAYER_LOGIN_QUERY_LOAD_TALENTS);

        // Weitere (weniger kritisch, aber gut für Vollständigkeit):
        // LoadQuery(CHAR_SEL_CHARACTER_HOMEBIND, PLAYER_LOGIN_QUERY_LOAD_HOME_BIND);
        // LoadQuery(CHAR_SEL_CHARACTER_SPELLCOOLDOWNS, PLAYER_LOGIN_QUERY_LOAD_SPELL_COOLDOWNS);

        /*
        if (sWorld->getBoolConfig(CONFIG_DECLINED_NAMES_USED))
            LoadQuery(CHAR_SEL_CHARACTER_DECLINEDNAMES, PLAYER_LOGIN_QUERY_LOAD_DECLINED_NAMES);
        */

        // LoadQuery(CHAR_SEL_CHARACTER_ACHIEVEMENTS, PLAYER_LOGIN_QUERY_LOAD_ACHIEVEMENTS);
        // LoadQuery(CHAR_SEL_CHARACTER_CRITERIAPROGRESS, PLAYER_LOGIN_QUERY_LOAD_CRITERIA_PROGRESS);
        // LoadQuery(CHAR_SEL_CHARACTER_ENTRY_POINT, PLAYER_LOGIN_QUERY_LOAD_ENTRY_POINT);
        // LoadQuery(CHAR_SEL_ACCOUNT_DATA, PLAYER_LOGIN_QUERY_LOAD_ACCOUNT_DATA);
        // LoadQuery(CHAR_SEL_CHARACTER_RANDOMBG, PLAYER_LOGIN_QUERY_LOAD_RANDOM_BG);
        // LoadQuery(CHAR_SEL_CHARACTER_BANNED, PLAYER_LOGIN_QUERY_LOAD_BANNED);
        // LoadQuery(CHAR_SEL_CHARACTER_QUESTSTATUSREW, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_REW);
        // LoadQuery(CHAR_SEL_BREW_OF_THE_MONTH, PLAYER_LOGIN_QUERY_LOAD_BREW_OF_THE_MONTH);

        // LoadQuery(CHAR_SEL_CORPSE_LOCATION, PLAYER_LOGIN_QUERY_LOAD_CORPSE_LOCATION);
        // LoadQuery(CHAR_SEL_CHAR_SETTINGS, PLAYER_LOGIN_QUERY_LOAD_CHARACTER_SETTINGS);
        // LoadQuery(CHAR_SEL_CHAR_PETS, PLAYER_LOGIN_QUERY_LOAD_PET_SLOTS);
        // LoadQuery(CHAR_SEL_CHAR_ACHIEVEMENT_OFFLINE_UPDATES, PLAYER_LOGIN_QUERY_LOAD_OFFLINE_ACHIEVEMENTS_UPDATES);

        return true;
    }
};

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
bool g_EquippedUpgrade = false;

// --- HELPER ---

int GetItemScore(ItemTemplate const* proto) {
    if (!proto) return 0;
    int score = 0;
    score += proto->Quality * 10;
    score += proto->ItemLevel;
    score += proto->Armor;
    if (proto->Class == ITEM_CLASS_WEAPON) {
        score += (int)(proto->Damage[0].DamageMax + proto->Damage[0].DamageMin);
    }
    for (int i = 0; i < proto->StatsCount; ++i) {
        score += proto->ItemStat[i].ItemStatValue * 2;
    }
    return score;
}

void TryEquipIfBetter(Player* player, uint16 srcPos) {
    uint8 bag = srcPos >> 8;
    uint8 slot = srcPos & 255;
    Item* newItem = player->GetItemByPos(bag, slot);
    if (!newItem) return;
    if (player->CanUseItem(newItem) != EQUIP_ERR_OK) return;

    ItemTemplate const* proto = newItem->GetTemplate();
    uint16 destSlot = 0xffff;

    switch (proto->InventoryType) {
    case INVTYPE_HEAD: destSlot = EQUIPMENT_SLOT_HEAD; break;
    case INVTYPE_SHOULDERS: destSlot = EQUIPMENT_SLOT_SHOULDERS; break;
    case INVTYPE_BODY: case INVTYPE_CHEST: case INVTYPE_ROBE: destSlot = EQUIPMENT_SLOT_CHEST; break;
    case INVTYPE_WAIST: destSlot = EQUIPMENT_SLOT_WAIST; break;
    case INVTYPE_LEGS: destSlot = EQUIPMENT_SLOT_LEGS; break;
    case INVTYPE_FEET: destSlot = EQUIPMENT_SLOT_FEET; break;
    case INVTYPE_WRISTS: destSlot = EQUIPMENT_SLOT_WRISTS; break;
    case INVTYPE_HANDS: destSlot = EQUIPMENT_SLOT_HANDS; break;
    case INVTYPE_WEAPON: case INVTYPE_2HWEAPON: case INVTYPE_WEAPONMAINHAND: destSlot = EQUIPMENT_SLOT_MAINHAND; break;
    case INVTYPE_SHIELD: case INVTYPE_WEAPONOFFHAND: destSlot = EQUIPMENT_SLOT_OFFHAND; break;
    }

    if (destSlot != 0xffff) {
        int newScore = GetItemScore(proto);
        int currentScore = -1;
        Item* currentItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, destSlot);
        if (currentItem) currentScore = GetItemScore(currentItem->GetTemplate());

        if (newScore > currentScore) {
            player->SwapItem(srcPos, destSlot);
            player->PlayDistanceSound(120, player);
            { std::lock_guard<std::mutex> lock(g_EventMutex); g_EquippedUpgrade = true; }
            LOG_INFO("module", "AI-GEAR: Upgrade angelegt! (Slot: {})", destSlot);
        }
    }
}

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

                if (!creature->IsAlive()) {
                    if (i_player->GetDistance(creature) > 10.0f) continue;
                }
                else if (i_player->GetDistance(creature) > 50.0f) continue;

                foundCreatures.push_back(creature);
            }
        }
    }
    template<class SKIP> void Visit(GridRefMgr<SKIP>&) {}
};

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
            catch (std::exception& e) { LOG_ERROR("module", "Verbindung verloren: {}", e.what()); }
        }
    }
    catch (std::exception& e) { LOG_ERROR("module", "Server Crash: {}", e.what()); }
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
    void OnStartup() override { std::thread(AIServerThread).detach(); }

    void OnUpdate(uint32 diff) override {
        _fastTimer += diff; _slowTimer += diff; _faceTimer += diff;

        if (_fastTimer >= 150) {
            _fastTimer = 0;
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

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            while (!g_CommandQueue.empty()) {
                AICommand cmd = g_CommandQueue.front();
                g_CommandQueue.pop();
                Player* player = ObjectAccessor::FindPlayerByName(cmd.playerName);
                if (!player) continue;

                if (cmd.actionType == "say") player->Say(cmd.value, LANG_UNIVERSAL);
                else if (cmd.actionType == "stop") { player->GetMotionMaster()->Clear(); player->GetMotionMaster()->MoveIdle(); }
                else if (cmd.actionType == "turn_left" || cmd.actionType == "turn_right") {
                    float o = player->GetOrientation();
                    float step = (cmd.actionType == "turn_left") ? 0.5f : -0.5f;
                    o += step; if (o > 6.283f) o -= 6.283f; if (o < 0) o += 6.283f;
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
                        if (!(spellId == 585 && target == player)) player->CastSpell(target, spellId, false);
                    }
                }
                else if (cmd.actionType == "reset") {
                    player->CombatStop(true); player->AttackStop(); player->GetMotionMaster()->Clear();
                    if (!player->isDead()) { player->ResurrectPlayer(1.0f, false); player->SpawnCorpseBones(); }
                    player->SetHealth(player->GetMaxHealth()); player->SetPower(player->getPowerType(), player->GetMaxPower(player->getPowerType()));
                    player->RemoveAllSpellCooldown(); player->RemoveAllAuras();
                    player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, player->GetOrientation());
                }
                else if (cmd.actionType == "move_to") {
                    std::string val = cmd.value;
                    size_t p1 = val.find(':');
                    size_t p2 = val.find(':', p1 + 1);
                    if (p1 != std::string::npos && p2 != std::string::npos) {
                        float tx = std::stof(val.substr(0, p1)); float ty = std::stof(val.substr(p1 + 1, p2 - p1 - 1)); float tz = std::stof(val.substr(p2 + 1));
                        player->UpdateGroundPositionZ(tx, ty, tz);
                        Position pos(tx, ty, tz, 0.0f);
                        player->GetMotionMaster()->MovePoint(1, pos, (ForcedMovement)0, 0.0f, true);
                    }
                }
                else if (cmd.actionType == "target_guid") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Unit* target = ObjectAccessor::GetUnit(*player, guid);
                    if (target) { player->SetSelection(target->GetGUID()); player->SetTarget(target->GetGUID()); player->SetFacingToObject(target); player->AttackStop(); }
                }
                else if (cmd.actionType == "loot_guid") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Creature* target = ObjectAccessor::GetCreature(*player, guid);
                    if (target && target->isDead()) {
                        if (player->GetDistance(target) <= 10.0f) {
                            player->SendLoot(target->GetGUID(), LOOT_CORPSE);
                            Loot* loot = &target->loot;
                            uint32 gold = loot->gold;
                            if (gold > 0) {
                                loot->gold = 0; player->ModifyMoney(gold);
                                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);
                                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1); data << uint32(gold); data << uint8(1); player->GetSession()->SendPacket(&data);
                                { std::lock_guard<std::mutex> lock(g_EventMutex); g_LootCopper += gold; }
                            }
                            for (uint8 i = 0; i < loot->items.size(); ++i) {
                                LootItem* item = loot->LootItemInSlot(i, player);
                                if (item && !item->is_looted && !item->freeforall && !item->needs_quest) {
                                    ItemPosCountVec dest;
                                    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid, item->count);
                                    if (msg == EQUIP_ERR_OK) {
                                        Item* newItem = player->StoreNewItem(dest, item->itemid, true);
                                        item->count = 0; item->is_looted = true;
                                        if (newItem) player->SendNewItem(newItem, 1, false, true);
                                        TryEquipIfBetter(player, dest[0].pos);
                                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
                                        if (proto) { std::lock_guard<std::mutex> lock(g_EventMutex); g_LootScore += 1; }
                                    }
                                }
                            }
                            target->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
                            target->AllLootRemovedFromCorpse();
                            player->SendLootRelease(player->GetLootGUID());
                            player->SetSelection(ObjectGuid::Empty); player->SetTarget(ObjectGuid::Empty); player->AttackStop();
                        }
                    }
                }
                else if (cmd.actionType == "sell_grey") {
                    ObjectGuid guid = ObjectGuid(std::stoull(cmd.value));
                    Creature* vendor = ObjectAccessor::GetCreature(*player, guid);
                    if (vendor && player->GetDistance(vendor) <= 15.0f) {
                        player->StopMoving();
                        uint32 totalMoney = 0;
                        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i) {
                            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) {
                                ItemTemplate const* proto = item->GetTemplate();
                                if (proto->SellPrice > 0 && proto->ItemId != 6948) {
                                    uint32 price = proto->SellPrice * item->GetCount();
                                    totalMoney += price; player->DestroyItem(INVENTORY_SLOT_BAG_0, i, true);
                                }
                            }
                        }
                        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
                            if (Bag* bagItem = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)) {
                                for (uint8 i = 0; i < bagItem->GetBagSize(); ++i) {
                                    if (Item* item = bagItem->GetItemByPos(i)) {
                                        ItemTemplate const* proto = item->GetTemplate();
                                        if (proto->SellPrice > 0 && proto->ItemId != 6948) {
                                            uint32 price = proto->SellPrice * item->GetCount();
                                            totalMoney += price; player->DestroyItem(bag, i, true);
                                        }
                                    }
                                }
                            }
                        }
                        if (totalMoney > 0) {
                            player->ModifyMoney(totalMoney);
                            player->PlayDistanceSound(120, player);
                            { std::lock_guard<std::mutex> lock(g_EventMutex); g_LootCopper += totalMoney; }
                        }
                        player->SetSelection(ObjectGuid::Empty); player->SetTarget(ObjectGuid::Empty);
                    }
                }
            }
        }

        if (_fastTimer >= 400) {
            _fastTimer = 0;
            long long xp = 0; long long lCopper = 0; long long lScore = 0; bool lvlUp = false; bool equipUp = false;
            {
                std::lock_guard<std::mutex> lock(g_EventMutex);
                xp = g_XPGained; g_XPGained = 0; lvlUp = g_LeveledUp; g_LeveledUp = false;
                lCopper = g_LootCopper; g_LootCopper = 0; lScore = g_LootScore; g_LootScore = 0;
                equipUp = g_EquippedUpgrade; g_EquippedUpgrade = false;
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
                ss << "\"equipped_upgrade\": \"" << (equipUp ? "true" : "false") << "\", ";
                Unit* target = p->GetSelectedUnit();
                std::string tStatus = "none"; uint32 tHp = 0; float tx = 0, ty = 0, tz = 0;
                if (target) {
                    tStatus = target->IsAlive() ? "alive" : "dead";
                    tHp = target->GetHealth();
                    tx = target->GetPositionX(); ty = target->GetPositionY(); tz = target->GetPositionZ();
                }
                ss << "\"target_status\": \"" << tStatus << "\", ";
                ss << "\"target_hp\": " << tHp << ", ";
                ss << "\"xp_gained\": " << xp << ", ";
                ss << "\"loot_copper\": " << lCopper << ", ";
                ss << "\"loot_score\": " << lScore << ", ";
                ss << "\"leveled_up\": \"" << (lvlUp ? "true" : "false") << "\", ";
                ss << "\"tx\": " << tx << ", ";
                ss << "\"ty\": " << ty << ", ";
                ss << "\"tz\": " << tz << ", ";
                if (_cachedNearbyMobsJson.empty()) _cachedNearbyMobsJson = "[]";
                ss << "\"nearby_mobs\": " << _cachedNearbyMobsJson;
                ss << "}";
            }
            ss << "] }";
            { std::lock_guard<std::mutex> lock(g_Mutex); g_CurrentJsonState = ss.str(); g_HasNewState = true; }
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

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override {
        std::string commandPrefix = "#spawn";

        if (msg.length() >= commandPrefix.length() && msg.substr(0, commandPrefix.length()) == commandPrefix) {

            LOG_INFO("module", "AI-DEBUG: Chat von {}: '{}'", player->GetName(), msg);

            if (msg.length() <= commandPrefix.length() + 1) {
                ChatHandler(player->GetSession()).SendSysMessage("Benutzung: #spawn <BotName>");
                msg = "";
                return;
            }

            std::string botName = msg.substr(commandPrefix.length() + 1);
            if (!botName.empty() && botName.back() == ' ') botName.pop_back();

            LOG_INFO("module", "AI-DEBUG: Versuche Bot zu spawnen: '{}'", botName);

            // ACHTUNG: Hier muss sCharacterCache evtl. durch eine entsprechende Methode in Ihrem Core ersetzt werden,
            // falls GetCharacterGuidByName dort anders heißt.
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(botName);
            if (!guid) {
                ChatHandler(player->GetSession()).SendSysMessage("Charakter nicht gefunden.");
                msg = "";
                return;
            }

            if (ObjectAccessor::FindPlayerByName(botName)) {
                ChatHandler(player->GetSession()).SendSysMessage("Bot ist bereits online.");
                msg = "";
                return;
            }

            LOG_INFO("module", "DEBUG STEP 1: Hole AccountID...");
            uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
            LOG_INFO("module", "DEBUG STEP 2: AccountID ist {}", accountId);

            if (accountId == 0) {
                ChatHandler(player->GetSession()).SendSysMessage("Fehler: Ungültige AccountID.");
                msg = "";
                return;
            }

            if (sWorldSessionMgr->FindSession(accountId)) {
                LOG_ERROR("module", "ABORT: Account {} ist bereits eingeloggt!", accountId);
                ChatHandler(player->GetSession()).SendSysMessage("Fehler: Account bereits eingeloggt.");
                msg = "";
                return;
            }

            // --- NEUE SYNCHRONE LADESTRATEGIE (Playerbot-Style) ---
            LOG_INFO("module", "DEBUG STEP 3: Starte synchronen Login...");

            // 1. Holder erstellen
            auto holder = std::make_shared<BotLoginQueryHolder>(accountId, guid);

            // 2. Initialisieren (führt alle Queries SYNCHRON aus!)
            if (!holder->Initialize()) {
                LOG_ERROR("module", "FAIL: Konnte LoginQueryHolder nicht initialisieren.");
                ChatHandler(player->GetSession()).SendSysMessage("Interner DB-Fehler (Init).");
                msg = "";
                return;
            }

            LOG_INFO("module", "DEBUG STEP 4: DB-Daten geladen. Erstelle Session...");

            // 4. Session erstellen (IsBot simuliert durch Flags oder manuelles Handling, da Konstruktor nur 12 Args hat)
            WorldSession* botSession = new WorldSession(
                accountId,
                std::string(botName),
                0,      // Security Token (dummy)
                nullptr,// Socket (dummy)
                SEC_PLAYER,
                EXPANSION_WRATH_OF_THE_LICH_KING,
                time_t(0),
                LOCALE_enUS,
                0,
                false,
                true,   // skipQueue = true
                0
            );

            // TRICK: Setze InQueue=true, um den Idle-Timeout (Kick) zu verhindern, da wir keinen Socket haben.
            botSession->SetInQueue(true);

            // WICHTIG: Session MUSS im Manager sein
            sWorldSessionMgr->AddSession(botSession);

            LOG_INFO("module", "DEBUG STEP 5: Erstelle Player und lade aus DB...");

            // 5. Player laden mit den vorbereiteten Daten (DIREKT, kein Callback!)
            Player* botPlayer = new Player(botSession);

            // Cast zu Basisklasse für LoadFromDB (signatur erwartet CharacterDatabaseQueryHolder)
            // Hinweis: *holder wird als Referenz übergeben (dereferenziert).
            if (!botPlayer->LoadFromDB(holder->GetGuid(), *holder)) {
                LOG_ERROR("module", "FAIL: Player konnte nicht geladen werden.");
                delete botPlayer;
                sWorldSessionMgr->KickSession(accountId);
                return;
            }

            LOG_INFO("module", "DEBUG STEP 6: Player geladen: {}", botPlayer->GetName());

            // Session verknüpfen
            botSession->SetPlayer(botPlayer);

            // Zur Welt hinzufügen
            botPlayer->GetMotionMaster()->Initialize();
            botPlayer->SetMap(botPlayer->GetMap());
            botPlayer->AddToWorld();

            // Initialisierungs-Pakete senden (simuliert)
            botPlayer->SendInitialPacketsBeforeAddToMap();
            botPlayer->SendInitialPacketsAfterAddToMap();

            // Teleport zum Beschwörer (wichtig!)
            botPlayer->TeleportTo(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation());

            // Reset Queue Flag, da wir jetzt "drin" sind
            botSession->SetInQueue(false);

            LOG_INFO("module", "DEBUG STEP 7: Fertig!");
            ChatHandler(player->GetSession()).SendSysMessage("Bot erfolgreich gespawnt!");

            msg = "";
        }
    }
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override {
        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_XPGained += amount;
    }
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override {
        if (player->GetLevel() >= 2) {
            { std::lock_guard<std::mutex> lock(g_EventMutex); g_LeveledUp = true; }
            player->SetLevel(1); player->SetUInt32Value(PLAYER_XP, 0); player->InitStatsForLevel(true);
            player->SetHealth(player->GetMaxHealth()); player->SetPower(player->getPowerType(), player->GetMaxPower(player->getPowerType()));
        }
    }
    void OnPlayerMoneyChanged(Player* player, int32& amount) override {
        if (amount > 0) { std::lock_guard<std::mutex> lock(g_EventMutex); g_LootCopper += amount; }
    }
};

void AddAIControllerScripts() {
    new AIControllerPlayerScript();
    new AIControllerWorldScript();
}
