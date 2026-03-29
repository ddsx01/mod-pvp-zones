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

struct Config
{
    bool   enabled            = true;
    uint32 kill_goal          = 50;
    uint32 kill_points        = 10;
    uint32 announcement_delay = 60000; // Храним в мс
    uint32 event_delay        = 1800000; 
    uint32 event_lasts        = 600000;  
	
	std::unordered_map<uint32, std::vector<uint32>> ids;

    uint32 current_zone;
    uint32 current_area;
    std::string current_zone_name;
    std::string current_area_name;

    bool active = false;

    std::vector<ObjectGuid> area_players;
    std::vector<ObjectGuid> zone_players;
    std::map<ObjectGuid, uint32> points;

    // Таймеры в миллисекундах для OnUpdate
    int32 timer_event = 1800000;
    int32 timer_announce = 60000;
};

Config config;

class ZoneConfig : public WorldScript
{
public:
    ZoneConfig() : WorldScript("pvp_zones_Config", {
        WORLDHOOK_ON_STARTUP
    }) {}

    void OnStartup() override
    {
		config.enabled            = sConfigMgr->GetOption<bool>("pvp_zones.Enable", true);
		config.kill_goal          = sConfigMgr->GetOption<uint32>("pvp_zones.KillGoal", 50); // По умолчанию 50 киллов
		config.kill_points        = sConfigMgr->GetOption<uint32>("pvp_zones.KillPoints", 10);
		
		config.announcement_delay = sConfigMgr->GetOption<uint32>("pvp_zones.AnnouncementDelay", 60) * 1000;
        config.event_delay        = sConfigMgr->GetOption<uint32>("pvp_zones.EventDelay", 1800) * 1000;
        config.event_lasts        = sConfigMgr->GetOption<uint32>("pvp_zones.EventLasts", 600) * 1000;
		
		config.timer_event = config.event_delay; // Первый ивент через delay мс
        config.timer_announce = config.announcement_delay;
		
		std::set<uint32> levelingZones = {
			// --- Classic (30-60) ---
			10,   // Arathi Highlands
			33,   // Stranglethorn Vale
			405,  // Desolace
			36,   // Alterac Mountains
			3,    // Badlands
			8,    // Swamp of Sorrows
			141,  // Dustwallow Marsh
			440,  // Tanaris
			357,  // Feralas
			45,   // Hinterlands
			51,   // Searing Gorge
			4,    // Blasted Lands
			490,  // Un'Goro Crater
			361,  // Felwood
			46,   // Burning Steppes
			139,  // Western Plaguelands
			28,   // Eastern Plaguelands
			618,  // Winterspring
			1377, // Silithus

			// --- Outland (58-70) ---
			3483, // Hellfire Peninsula
			3521, // Zangarmarsh
			3519, // Terokkar Forest
			3518, // Nagrand
			3522, // Blade's Edge Mountains
			3520, // Shadowmoon Valley
			3523, // Netherstorm

			// --- Northrend (68-80) ---
			3537, // Borean Tundra
			495,  // Howling Fjord
			394,  // Grizzly Hills
			65,   // Dragonblight
			66,   // Zul'Drak
			3711, // Sholazar Basin
			67,   // The Storm Peaks
			210,  // Icecrown
			4197, // Wintergrasp
			4395  // Crystalsong Forest (но мы отфильтруем Даларан ниже)
		};
		
		config.ids.clear();
		
		uint32 totalSubAreas = 0;
		// Проходим по всем записям таблицы зон (AreaTable)
		for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
		{
			AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
			if (!area)
				continue;

			// В твоей структуре DBCStructure.h это поле называется 'zone'
			uint32 parentId = (area->zone != 0) ? area->zone : area->ID;

			// Теперь эта строка сработает без ошибок:
			if (levelingZones.find(parentId) != levelingZones.end())
			{
				if (area->flags & AREA_FLAG_SANCTUARY) continue; // Пропускаем Даларан, Шаттрат
                if (area->flags & AREA_FLAG_CAPITAL)   continue; // Пропускаем столицы

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

    // Метод получения имени игрока (универсальный способ)
    static std::string GetNameByGUID(ObjectGuid guid)
    {
        if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
            return p->GetName();

        QueryResult result = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", guid.GetCounter());
        if (result)
            return (*result)[0].Get<std::string>();

        return "Unknown";
    }

    void OnPlayerUpdateArea(Player* player, uint32, uint32 newArea) override
    {
        if (!config.active) return;
        ObjectGuid guid = player->GetGUID();
        if (config.current_area == newArea)
        {
            if (std::find(config.area_players.begin(), config.area_players.end(), guid) == config.area_players.end())
            {
                ChatHandler(player->GetSession()).SendSysMessage("You have entered the Oceanic War blood zone!");
                config.area_players.push_back(guid);
            }
        }
        else
            config.area_players.erase(std::remove(config.area_players.begin(), config.area_players.end(), guid), config.area_players.end());
    }

     void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32) override
    {
        if (!config.active) return;
        ObjectGuid guid = player->GetGUID();
        if (config.current_zone == newZone)
        {
            if (std::find(config.zone_players.begin(), config.zone_players.end(), guid) == config.zone_players.end())
            {
                ChatHandler(player->GetSession()).SendSysMessage("You have entered the Oceanic War zone!");
                config.zone_players.push_back(guid);
                player->SetPvP(true);
            }
        }
        else
            config.zone_players.erase(std::remove(config.zone_players.begin(), config.zone_players.end(), guid), config.zone_players.end());
    }

    void PostLeaderBoard(ChatHandler* handler)
    {
        handler->SendGlobalSysMessage("[PvP Zones Leaderboard]");
        if (config.points.empty()) return;

        for (auto const& [guid, score] : config.points)
        {
            std::string name = GetNameByGUID(guid);
			handler->PSendSysMessage(std::format("{}: {}", name, score));
        }
    }

    static void PostAnnouncement(ChatHandler* handler)
    {
        if (!config.active) return;
		handler->PSendSysMessage(std::format("[pvp_zones] Is currently active in: {} - {}", config.current_zone_name, config.current_area_name));
    }

    static void CreateEvent(ChatHandler* handler)
    {
        if (config.active) return;

        config.active = true;
        config.timer_event = config.event_lasts; // Теперь таймер отсчитывает длительность
        config.timer_announce = config.announcement_delay;
        
        config.kill_goal = sConfigMgr->GetOption<uint32>("pvp_zones.KillGoal", 50);
        config.points.clear();
        config.area_players.clear();
        config.zone_players.clear();

        auto map_it = std::begin(config.ids);
        std::advance(map_it, rand() % config.ids.size());
        auto area_it = std::begin(map_it->second);
        std::advance(area_it, rand() % map_it->second.size());

        config.current_zone = map_it->first;
        config.current_area = *area_it;

        if (AreaTableEntry const* entry = sAreaTableStore.LookupEntry(config.current_area))
            config.current_area_name = entry->area_name[handler->GetSessionDbcLocale()];
        if (AreaTableEntry const* z_entry = sAreaTableStore.LookupEntry(config.current_zone))
            config.current_zone_name = z_entry->area_name[handler->GetSessionDbcLocale()];

        handler->SendGlobalSysMessage(std::format("[pvp_zones] A new zone has been declared: {} - {}", config.current_zone_name, config.current_area_name).c_str());

        WorldSessionMgr::SessionMap const& m_sessions = sWorldSessionMgr->GetAllSessions();
        for (auto const& pair : m_sessions)
        {
            if (Player* p = pair.second->GetPlayer())
            {
                if (p->GetZoneId() == config.current_zone)
                {
                    p->SetPvP(true);
                    config.zone_players.push_back(p->GetGUID());
                }
                if (p->GetAreaId() == config.current_area)
                    config.area_players.push_back(p->GetGUID());
            }
        }
    }

    static void EndEvent(ChatHandler* handler)
    {
        if (!config.active) return;

        config.active = false;
        config.timer_event = config.event_delay; // Теперь таймер отсчитывает задержку до следующего
        
        handler->SendGlobalSysMessage("[pvp_zones] The event has ended.");
        config.points.clear();
        config.area_players.clear();
        config.zone_players.clear();
    }

    void OnPlayerPVPKill(Player* winner, Player* loser) override
    {
        if (!config.active) return;
        if (winner->GetZoneId() == config.current_zone)
        {
            uint32 reward = config.kill_points;
            if (winner->GetAreaId() == config.current_area) reward *= 2;
            config.points[winner->GetGUID()] += reward;
            ChatHandler(winner->GetSession()).PSendSysMessage(std::format("[pvp_zones] Gained {} points!", reward));
            
            if (config.kill_goal > 0)
            {
                config.kill_goal--;
                if (config.kill_goal == 0)
                {
                    ChatHandler h(winner->GetSession());
                    EndEvent(&h);
                }
            }
        }
    }
};

class ZoneWorld : public WorldScript
{
public:
    ZoneWorld() : WorldScript("pvp_zones_World") {}

	void OnUpdate(uint32 diff) override
    {
        if (!config.enabled) return;

        // 1. Управление таймером ивента (старт/стоп)
        config.timer_event -= diff;
        if (config.timer_event <= 0)
        {
            Player* p = GetAnyPlayer();
            if (p)
            {
                ChatHandler h(p->GetSession());
                if (!config.active)
                    ZoneLogicScript::CreateEvent(&h);
                else
                    ZoneLogicScript::EndEvent(&h);
            }
            else
            {
                // Если игроков нет, просто сбросим таймер, чтобы не спамить проверку
                config.timer_event = 5000; 
            }
        }

        // 2. Управление анонсами (только во время ивента)
        if (config.active)
        {
            config.timer_announce -= diff;
            if (config.timer_announce <= 0)
            {
                config.timer_announce = config.announcement_delay;
                if (Player* p = GetAnyPlayer())
                {
                    ChatHandler h(p->GetSession());
                    ZoneLogicScript::PostAnnouncement(&h);
                }
            }
        }
    }

private:
    Player* GetAnyPlayer()
    {
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
            { "pvpz.create", HandleCreateCommand, SEC_GAMEMASTER, Console::No },
            { "pvpz.end",    HandleEndCommand,    SEC_GAMEMASTER, Console::No },
			{ "pvpz.list",   HandleListCommand,   SEC_GAMEMASTER, Console::No }
        };
        return commandTable;
    }

    static bool HandleCreateCommand(ChatHandler* handler) { ZoneLogicScript::CreateEvent(handler); return true; }
    static bool HandleEndCommand(ChatHandler* handler) { ZoneLogicScript::EndEvent(handler); return true; }
	static bool HandleListCommand(ChatHandler* handler)
	{
		if (config.ids.empty())
		{
			handler->SendSysMessage("Список PvP зон пуст. Проверьте логи OnStartup.");
			return true;
		}

		handler->SendSysMessage("|cffFFFF00--- Список зарегистрированных PvP локаций ---|r");

		uint32 totalZones = 0;
		for (auto const& [parentId, subAreas] : config.ids)
		{
			// Получаем имя главной зоны
			std::string parentName = "Неизвестная зона";
			if (AreaTableEntry const* pEntry = sAreaTableStore.LookupEntry(parentId))
				parentName = pEntry->area_name[handler->GetSessionDbcLocale()];

			// Формируем заголовок группы
			handler->SendSysMessage(std::format("|cff00FF00Зона: {} ({}) - Подзон: {}|r", parentId, parentName, (uint32)subAreas.size()));

			// Собираем имена подзон в одну строку, чтобы не спамить в чат по одной строчке
			std::string areaList = "  |cff888888Области: ";
			for (size_t i = 0; i < subAreas.size(); ++i)
			{
				std::string aName = "Unknown";
				if (AreaTableEntry const* aEntry = sAreaTableStore.LookupEntry(subAreas[i]))
					aName = aEntry->area_name[handler->GetSessionDbcLocale()];

				areaList += std::format("{} ({})", aName, subAreas[i]);

				if (i < subAreas.size() - 1)
					areaList += ", ";

				// Если строка слишком длинная (более 250 символов), отправляем её и начинаем новую
				if (areaList.length() > 250)
				{
					handler->SendSysMessage(areaList.c_str());
					areaList = "  ";
				}
			}

			if (areaList.length() > 2)
				handler->SendSysMessage(areaList.c_str());
			
			totalZones++;
		}

		handler->PSendSysMessage(std::format("|cffFFFF00Всего активных материнских зон: {}", totalZones));
		return true;
	}
};

void Addpvp_zonesScripts()
{
	new ZoneConfig();
    new ZoneWorld();
    new ZoneLogicScript();
    new ZoneCommands();
}