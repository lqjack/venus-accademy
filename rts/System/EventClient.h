/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef EVENT_CLIENT_H
#define EVENT_CLIENT_H

#include "System/float3.h"
#include <string>
#include <vector>
#include <map>

#ifdef __APPLE__
// defined in X11/X.h
#undef KeyPress
#undef KeyRelease
#endif

using std::string;
using std::vector;
using std::map;

class CUnit;
class CWeapon;
class CFeature;
class CProjectile;
struct Command;
class IArchive;
struct SRectangle;

#ifndef zipFile
	// might be defined through zip.h already
	typedef void* zipFile;
#endif


class CEventClient
{
	public:
		enum SpecialTeams {
			NoAccessTeam   = -1,
			AllAccessTeam  = -2,
			MinSpecialTeam = AllAccessTeam
		};

	public:
		inline const std::string& GetName()   const { return name;   }
		inline int                GetOrder()  const { return order;  }
		inline bool               GetSynced() const { return synced_; }

		/**
		 * Used by the eventHandler to register
		 * call-ins when an EventClient is being added.
		 */
		virtual bool WantsEvent(const std::string& eventName) = 0;

		// used by the eventHandler to route certain event types
		virtual int  GetReadAllyTeam() const { return NoAccessTeam; }
		virtual bool GetFullRead()     const { return GetReadAllyTeam() == AllAccessTeam; }
		inline bool CanReadAllyTeam(int allyTeam) {
			return (GetFullRead() || (GetReadAllyTeam() == allyTeam));
		}

	private:
		const std::string name;
		const int         order;
		const bool        synced_;

	protected:
		CEventClient(const std::string& name, int order, bool synced);
		virtual ~CEventClient();

	public:
		/**
		 * @name Synced_events
		 * @{
		 */
		virtual void Load(IArchive* archive) {}

		virtual void GamePreload() {}
		virtual void GameStart() {}
		virtual void GameOver(const std::vector<unsigned char>& winningAllyTeams) {}
		virtual void GamePaused(int playerID, bool paused) {}
		virtual void GameFrame(int gameFrame) {}
		virtual void GameID(const unsigned char* gameID, unsigned int numBytes) {}

		virtual void TeamDied(int teamID) {}
		virtual void TeamChanged(int teamID) {}
		virtual void PlayerChanged(int playerID) {}
		virtual void PlayerAdded(int playerID) {}
		virtual void PlayerRemoved(int playerID, int reason) {}

		virtual void UnitCreated(const CUnit* unit, const CUnit* builder) {}
		virtual void UnitFinished(const CUnit* unit) {}
		virtual void UnitFromFactory(const CUnit* unit, const CUnit* factory,
		                             bool userOrders) {}
		virtual void UnitDestroyed(const CUnit* unit, const CUnit* attacker) {}
		virtual void UnitTaken(const CUnit* unit, int oldTeam, int newTeam) {}
		virtual void UnitGiven(const CUnit* unit, int oldTeam, int newTeam) {}

		virtual void UnitIdle(const CUnit* unit) {}
		virtual void UnitCommand(const CUnit* unit, const Command& command) {}
		virtual void UnitCmdDone(const CUnit* unit, int cmdType, int cmdTag) {}
		virtual void UnitDamaged(
			const CUnit* unit,
			const CUnit* attacker,
			float damage,
			int weaponDefID,
			int projectileID,
			bool paralyzer) {}
		virtual void UnitExperience(const CUnit* unit, float oldExperience) {}

		virtual void UnitSeismicPing(const CUnit* unit, int allyTeam,
		                             const float3& pos, float strength) {}
		virtual void UnitEnteredRadar(const CUnit* unit, int allyTeam) {}
		virtual void UnitEnteredLos(const CUnit* unit, int allyTeam) {}
		virtual void UnitLeftRadar(const CUnit* unit, int allyTeam) {}
		virtual void UnitLeftLos(const CUnit* unit, int allyTeam) {}

		virtual void UnitEnteredWater(const CUnit* unit) {}
		virtual void UnitEnteredAir(const CUnit* unit) {}
		virtual void UnitLeftWater(const CUnit* unit) {}
		virtual void UnitLeftAir(const CUnit* unit) {}

		virtual void UnitLoaded(const CUnit* unit, const CUnit* transport) {}
		virtual void UnitUnloaded(const CUnit* unit, const CUnit* transport) {}

		virtual void UnitCloaked(const CUnit* unit) {}
		virtual void UnitDecloaked(const CUnit* unit) {}

		virtual void RenderUnitCreated(const CUnit* unit, int cloaked) {}
		virtual void RenderUnitDestroyed(const CUnit* unit) {}
		virtual void RenderUnitCloakChanged(const CUnit* unit, int cloaked) {}
		virtual void RenderUnitLOSChanged(const CUnit* unit, int allyTeam, int newStatus) {}
		virtual void RenderUnitMoved(const CUnit* unit, const float3& newpos) {}

		virtual void UnitUnitCollision(const CUnit* collider, const CUnit* collidee) {}
		virtual void UnitFeatureCollision(const CUnit* collider, const CFeature* collidee) {}
		virtual void UnitMoved(const CUnit* unit) {}
		virtual void UnitMoveFailed(const CUnit* unit) {}

		virtual void FeatureCreated(const CFeature* feature) {}
		virtual void FeatureDestroyed(const CFeature* feature) {}
		virtual void FeatureMoved(const CFeature* feature) {}

		virtual void RenderFeatureCreated(const CFeature* feature) {}
		virtual void RenderFeatureDestroyed(const CFeature* feature, const float3& pos) {}
		virtual void RenderFeatureMoved(const CFeature* feature, const float3& oldpos, const float3& newpos) {}

		virtual void ProjectileCreated(const CProjectile* proj) {}
		virtual void ProjectileDestroyed(const CProjectile* proj) {}

		virtual void RenderProjectileCreated(const CProjectile* proj) {}
		virtual void RenderProjectileDestroyed(const CProjectile* proj) {}

		virtual void StockpileChanged(const CUnit* unit,
		                              const CWeapon* weapon, int oldCount) {}

		virtual bool Explosion(int weaponID, int projectileID, const float3& pos, const CUnit* owner) { return false; }
		/// @}

		/**
		 * @name Unsynced_events
		 * @{
		 */
		virtual void Save(zipFile archive);

		virtual void Update();
		virtual void UnsyncedHeightMapUpdate(const SRectangle& rect);

		virtual bool KeyPress(unsigned short key, bool isRepeat);
		virtual bool KeyRelease(unsigned short key);
		virtual bool MouseMove(int x, int y, int dx, int dy, int button);
		virtual bool MousePress(int x, int y, int button);
		virtual int  MouseRelease(int x, int y, int button); // FIXME - bool / void?
		virtual bool MouseWheel(bool up, float value);
		virtual bool JoystickEvent(const std::string& event, int val1, int val2);
		virtual bool IsAbove(int x, int y);
		virtual std::string GetTooltip(int x, int y);

		virtual bool DefaultCommand(const CUnit* unit, const CFeature* feature, int& cmd);

		virtual bool CommandNotify(const Command& cmd);

		virtual bool AddConsoleLine(const std::string& msg, const std::string& section, int level);

		virtual void LastMessagePosition(const float3& pos);

		virtual bool GroupChanged(int groupID);

		virtual bool GameSetup(const std::string& state, bool& ready,
		                       const map<int, std::string>& playerStates);

		virtual std::string WorldTooltip(const CUnit* unit,
		                                 const CFeature* feature,
		                                 const float3* groundPos);

		virtual bool MapDrawCmd(int playerID, int type,
		                        const float3* pos0,
		                        const float3* pos1,
		                        const std::string* label);

		virtual void SunChanged(const float3& sunDir);

		virtual void ViewResize();

		virtual void DrawGenesis();
		virtual void DrawWorld();
		virtual void DrawWorldPreUnit();
		virtual void DrawWorldShadow();
		virtual void DrawWorldReflection();
		virtual void DrawWorldRefraction();
		virtual void DrawScreenEffects();
		virtual void DrawScreen();
		virtual void DrawInMiniMap();

		virtual void GameProgress(int gameFrame) {}

		virtual void DrawLoadScreen() {}
		virtual void LoadProgress(const std::string& msg, const bool replace_lastline) {}
		/// @}
};


#endif /* EVENT_CLIENT_H */

