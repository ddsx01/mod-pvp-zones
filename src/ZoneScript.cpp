#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <map>
#include <vector>
#include <format>
#include <random>

// Структура одного активного ивента
struct ActiveEvent {
    uint32 zoneId;
    uint32 areaId;
    std::string zoneName;
    std::string areaName;
};

struct Config
{
    bool   enabled            = true;
    uint32 kill_goal          = 50;
    uint32 kill_points        = 10;
    uint32 announcement_delay = 60000;
    uint32 event_delay        = 1800000; 
    uint32 event_lasts        = 600000;  
    uint32 max_simultaneous   = 10; // Сколько зон запускать одновременно

    std::unordered_map<uint32, std::vector<uint32>> ids;
    
    // Список всех запущенных ивентов
    std::vector<ActiveEvent> active_events;
    bool active = false;

    std::vector<ObjectGuid> area_players;
    std::vector<ObjectGuid> zone_players;
    std::map<ObjectGuid, uint32> points;

    int32 timer_event = 1800000;
    int32 timer_announce = 60000;
};

Config config;

class ZoneConfig : public WorldScript
{
public:
    ZoneConfig() : WorldScript("pvp_zones_Config", { WORLDHOOK_ON_STARTUP }) {}

    void OnStartup() override
    {
        config.enabled            = sConfigMgr->GetOption<bool>("pvp_zones.Enable", true);
        config.kill_goal          = sConfigMgr->GetOption<uint32>("pvp_zones.KillGoal", 50);
        config.kill_points        = sConfigMgr->GetOption<uint32>("pvp_zones.KillPoints", 10);
        config.max_simultaneous   = sConfigMgr->GetOption<uint32>("pvp_zones.MaxZones", 10);
        
        config.announcement_delay = sConfigMgr->GetOption<uint32>("pvp_zones.AnnouncementDelay", 60) * 1000;
        config.event_delay        = sConfigMgr->GetOption<uint32>("pvp_zones.EventDelay", 1800) * 1000;
        config.event_lasts        = sConfigMgr->GetOption<uint32>("pvp_zones.EventLasts", 600) * 1000;
        
        config.timer_event = config.event_delay;
        config.timer_announce = config.announcement_delay;

        std::set<uint32> levelingZones = {
            10, 33, 405, 36, 3, 8, 141, 440, 357, 45, 51, 4, 490, 361, 46, 139, 28, 618, 1377,
            3483, 3521, 3519, 3518, 3522, 3520, 3523,
            3537, 495, 394, 65, 66, 3711, 67, 210, 4197, 4395
        };
        
        config.ids.clear();
        uint32 totalSubAreas = 0;
        for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
            if (!area) continue;
            uint32 parentId = (area->zone != 0) ? area->zone : area->ID;

            if (levelingZones.find(parentId) != levelingZones.end())
            {
                if (area->IsSanctuary() || (area->flags & AREA_FLAG_CAPITAL)) continue;
                config.ids[parentId].push_back(area->ID);
                totalSubAreas++;
            }
        }
        LOG_INFO("module", "[pvp_zones] Loaded {} main zones and {} sub-areas.", (uint32)config.ids.size(), totalSubAreas);
    }
};

class ZoneLogicScript : public PlayerScript
{
public:
    ZoneLogicScript() : PlayerScript("pvp_zones_PlayerScript") {}

    // Помощник: проверка, активна ли данная зона/область сейчас
    static const ActiveEvent* GetActiveEventByZone(uint32 zoneId) {
        for (auto const& ev : config.active_events)
            if (ev.zoneId == zoneId) return &ev;
        return nullptr;
    }

    static const ActiveEvent* GetActiveEventByArea(uint32 areaId) {
        for (auto const& ev : config.active_events)
            if (ev.areaId == areaId) return &ev;
        return nullptr;
    }

    void OnPlayerUpdateArea(Player* player, uint32, uint32 newArea) override
    {
        if (!config.active) return;
        ObjectGuid guid = player->GetGUID();

        if (GetActiveEventByArea(newArea)) {
            if (std::find(config.area_players.begin(), config.area_players.end(), guid) == config.area_players.end()) {
                ChatHandler(player->GetSession()).SendSysMessage("You have entered a blood zone (Double Points)!");
                config.area_players.push_back(guid);
            }
        } else {
            config.area_players.erase(std::remove(config.area_players.begin(), config.area_players.end(), guid), config.area_players.end());
        }
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32) override
    {
        if (!config.active) return;
        ObjectGuid guid = player->GetGUID();

        if (GetActiveEventByZone(newZone)) {
            if (std::find(config.zone_players.begin(), config.zone_players.end(), guid) == config.zone_players.end()) {
                ChatHandler(player->GetSession()).SendSysMessage("You have entered a PvP War Zone!");
                config.zone_players.push_back(guid);
                player->SetPvP(true);
            }
        } else {
            config.zone_players.erase(std::remove(config.zone_players.begin(), config.zone_players.end(), guid), config.zone_players.end());
        }
    }

    static void CreateEvent(ChatHandler* handler)
    {
        if (config.active) return;

        config.active_events.clear();
        config.points.clear();
        config.area_players.clear();
        config.zone_players.clear();

        // Собираем все доступные материнские зоны
        std::vector<uint32> parentKeys;
        for (auto const& [id, subs] : config.ids) parentKeys.push_back(id);

        // Перемешиваем список, чтобы выбрать случайные
        std::shuffle(parentKeys.begin(), parentKeys.end(), std::default_random_engine(std::random_device{}()));

        // Берем до N зон (по умолчанию 10)
        uint32 toStart = std::min((uint32)parentKeys.size(), config.max_simultaneous);

        for (uint32 i = 0; i < toStart; ++i) {
            uint32 zId = parentKeys[i];
            uint32 aId = config.ids[zId][rand() % config.ids[zId].size()];

            ActiveEvent ev;
            ev.zoneId = zId;
            ev.areaId = aId;

            if (AreaTableEntry const* ze = sAreaTableStore.LookupEntry(zId))
                ev.zoneName = ze->area_name[handler->GetSessionDbcLocale()];
            if (AreaTableEntry const* ae = sAreaTableStore.LookupEntry(aId))
                ev.areaName = ae->area_name[handler->GetSessionDbcLocale()];

            config.active_events.push_back(ev);
        }

        config.active = true;
        config.timer_event = config.event_lasts;
        config.timer_announce = config.announcement_delay;

        handler->SendGlobalSysMessage(std::format("[pvp_zones] WAR DECLARED! {} zones are now active PvP areas!", toStart).c_str());
        
        // Массовый анонс всех зон
        for (auto const& ev : config.active_events) {
            handler->SendGlobalSysMessage(std::format(" > {} ({})", ev.zoneName, ev.areaName).c_str());
        }

        // Включаем PvP тем, кто уже там
        WorldSessionMgr::SessionMap const& m_sessions = sWorldSessionMgr->GetAllSessions();
        for (auto const& pair : m_sessions) {
            if (Player* p = pair.second->GetPlayer()) {
                if (GetActiveEventByZone(p->GetZoneId())) {
                    p->SetPvP(true);
                    config.zone_players.push_back(p->GetGUID());
                    if (GetActiveEventByArea(p->GetAreaId()))
                        config.area_players.push_back(p->GetGUID());
                }
            }
        }
    }

    static void EndEvent(ChatHandler* handler)
    {
        if (!config.active) return;
        config.active = false;
        config.timer_event = config.event_delay;
        config.active_events.clear();
        handler->SendGlobalSysMessage("[pvp_zones] All events have ended.");
    }

    void OnPlayerPVPKill(Player* winner, Player* loser) override
    {
        if (!config.active) return;

        // Если убийца в одной из активных зон
        if (GetActiveEventByZone(winner->GetZoneId())) {
            uint32 reward = config.kill_points;
            if (GetActiveEventByArea(winner->GetAreaId())) reward *= 2;

            config.points[winner->GetGUID()] += reward;
            ChatHandler(winner->GetSession()).SendSysMessage(std::format("[pvp_zones] +{} points!", reward));
        }
    }
	
	// --- ВСПОМОГАТЕЛЬНЫЕ МЕТОДЫ ДЛЯ КОМАНД ---

    // Показывает текущие активные зоны (10 штук)
    static bool HandleViewCommand(ChatHandler* handler)
    {
        if (!config.active || config.active_events.empty())
        {
            handler->SendSysMessage("|cffFF0000[PvP Zones]|r В данный момент нет активных событий.");
            return true;
        }

        handler->SendSysMessage("|cffFFFF00--- Список АКТИВНЫХ PvP зон ---|r");
        uint32 count = 0;
        for (auto const& ev : config.active_events)
        {
            count++;
            handler->SendSysMessage(std::format("|cff00FF00{}. {} ({})|r", count, ev.zoneName, ev.areaName).c_str());
        }
        
        handler->SendSysMessage(std::format("Цель убийств на зону: |cffFFFFFF{}|r", config.kill_goal).c_str());
        return true;
    }

    // Показывает ВООБЩЕ ВСЕ зоны, которые загружены в систему (из OnStartup)
    static bool HandleListCommand(ChatHandler* handler)
    {
        if (config.ids.empty())
        {
            handler->SendSysMessage("Список всех зон пуст. Проверьте OnStartup.");
            return true;
        }

        handler->SendSysMessage("|cffFFFF00--- База данных всех доступных PvP локаций ---|r");

        for (auto const& [parentId, subAreas] : config.ids)
        {
            std::string parentName = "Unknown";
            if (AreaTableEntry const* pEntry = sAreaTableStore.LookupEntry(parentId))
                parentName = pEntry->area_name[handler->GetSessionDbcLocale()];

            handler->SendSysMessage(std::format("|cff00FF00Зона: {} ({})|r - Подзон: {}", parentId, parentName, (uint32)subAreas.size()).c_str());

            std::string areaList = "  |cff888888Подзоны: ";
            for (size_t i = 0; i < subAreas.size(); ++i)
            {
                std::string aName = "Unknown";
                if (AreaTableEntry const* aEntry = sAreaTableStore.LookupEntry(subAreas[i]))
                    aName = aEntry->area_name[handler->GetSessionDbcLocale()];

                areaList += std::format("{} ({})", aName, subAreas[i]);
                if (i < subAreas.size() - 1) areaList += ", ";

                if (areaList.length() > 240)
                {
                    handler->SendSysMessage(areaList);
                    areaList = "  ";
                }
            }
            if (areaList.length() > 5) handler->SendSysMessage(areaList);
        }
        return true;
    }
};

class ZoneWorld : public WorldScript
{
public:
    ZoneWorld() : WorldScript("pvp_zones_World") {}

    void OnUpdate(uint32 diff) override
    {
        if (!config.enabled) return;

        config.timer_event -= diff;
        if (config.timer_event <= 0) {
            if (Player* p = GetAnyPlayer()) {
                ChatHandler h(p->GetSession());
                if (!config.active) ZoneLogicScript::CreateEvent(&h);
                else ZoneLogicScript::EndEvent(&h);
            } else {
                config.timer_event = 5000;
            }
        }

        if (config.active) {
            config.timer_announce -= diff;
            if (config.timer_announce <= 0) {
                config.timer_announce = config.announcement_delay;
                if (Player* p = GetAnyPlayer()) {
                    ChatHandler h(p->GetSession());
                    for (auto const& ev : config.active_events)
                        h.PSendSysMessage(std::format("[pvp_zones] Active: {} ({})", ev.zoneName, ev.areaName));
                }
            }
        }
    }

    Player* GetAnyPlayer() {
        WorldSessionMgr::SessionMap const& sessions = sWorldSessionMgr->GetAllSessions();
        for (auto const& pair : sessions)
            if (pair.second && pair.second->GetPlayer() && pair.second->GetPlayer()->IsInWorld())
                return pair.second->GetPlayer();
        return nullptr;
    }
};

using namespace Acore::ChatCommands;
class ZoneCommands : public CommandScript
{
public:
    ZoneCommands() : CommandScript("pvp_zones_Commands") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "create", HandleCreateCommand, SEC_GAMEMASTER, Console::No },
            { "end",    HandleEndCommand,    SEC_GAMEMASTER, Console::No },
            { "list",   HandleListCommand,   SEC_GAMEMASTER, Console::No },
            { "view",   HandleViewCommand,   SEC_GAMEMASTER, Console::No }
        };

        // Регистрация подкоманд для .pvpz (например: .pvpz view)
        static ChatCommandTable pvpzCommandTable =
        {
            { "pvpz", commandTable }
        };

        return pvpzCommandTable;
    }

    // Врапперы (обертки) для связи команд с логикой
    static bool HandleCreateCommand(ChatHandler* handler) 
    { 
        ZoneLogicScript::CreateEvent(handler); 
        return true; 
    }

    static bool HandleEndCommand(ChatHandler* handler) 
    { 
        ZoneLogicScript::EndEvent(handler); 
        return true; 
    }

    static bool HandleListCommand(ChatHandler* handler) 
    { 
        return ZoneLogicScript::HandleListCommand(handler); 
    }

    static bool HandleViewCommand(ChatHandler* handler) 
    { 
        return ZoneLogicScript::HandleViewCommand(handler); 
    }
};

void Addpvp_zonesScripts()
{
	new ZoneConfig();
    new ZoneWorld();
    new ZoneLogicScript();
    new ZoneCommands();
}