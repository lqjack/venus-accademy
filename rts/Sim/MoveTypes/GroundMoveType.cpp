/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GroundMoveType.h"
#include "MoveDefHandler.h"
#include "ExternalAI/EngineOutHandler.h"
#include "Game/Camera.h"
#include "Game/GameHelper.h"
#include "Game/GlobalUnsynced.h"
#include "Game/Player.h"
#include "Game/SelectedUnits.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "MoveMath/MoveMath.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Path/IPathController.hpp"
#include "Sim/Path/IPathManager.h"
#include "Sim/Units/Scripts/CobInstance.h"
#include "Sim/Units/UnitTypes/TransportUnit.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/CommandAI/MobileCAI.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Sim/Weapons/Weapon.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/FastMath.h"
#include "System/myMath.h"
#include "System/Vec2.h"
#include "System/Sound/SoundChannels.h"
#include "System/Sync/SyncTracer.h"

#if 1
#include "Rendering/IPathDrawer.h"
#define DEBUG_DRAWING_ENABLED pathDrawer->IsEnabled()
#else
#define DEBUG_DRAWING_ENABLED false
#endif

#define LOG_SECTION_GMT "GroundMoveType"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_GMT)

// use the specific section for all LOG*() calls in this source file
#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_GMT

// speeds near (MAX_UNIT_SPEED * 1e1) elmos / frame can be caused by explosion impulses
// CUnitHandler removes units with speeds > MAX_UNIT_SPEED as soon as they exit the map,
// so the assertion can be less strict
#define ASSERT_SANE_OWNER_SPEED(v) assert(v.SqLength() < (MAX_UNIT_SPEED * MAX_UNIT_SPEED * 1e2));

// magic number to reduce damage taken from collisions
// between a very heavy and a very light CSolidObject
#define COLLISION_DAMAGE_MULT         0.02f

#define MAX_IDLING_SLOWUPDATES           16
#define IGNORE_OBSTACLES                  0
#define WAIT_FOR_PATH                     1
#define PLAY_SOUNDS                       1

#define UNIT_CMD_QUE_SIZE(u) (u->commandAI->commandQue.size())
#define UNIT_HAS_MOVE_CMD(u) (u->commandAI->commandQue.empty() || u->commandAI->commandQue[0].GetID() == CMD_MOVE)

#define FOOTPRINT_RADIUS(xs, zs, s) ((math::sqrt((xs * xs + zs * zs)) * 0.5f * SQUARE_SIZE) * s)


CR_BIND_DERIVED(CGroundMoveType, AMoveType, (NULL));
CR_REG_METADATA(CGroundMoveType, (
	CR_IGNORED(pathController),
	CR_MEMBER(turnRate),
	CR_MEMBER(accRate),
	CR_MEMBER(decRate),

	CR_MEMBER(maxReverseSpeed),
	CR_MEMBER(wantedSpeed),
	CR_MEMBER(currentSpeed),
	CR_MEMBER(deltaSpeed),

	CR_MEMBER(pathId),
	CR_MEMBER(goalRadius),

	CR_MEMBER(currWayPoint),
	CR_MEMBER(nextWayPoint),
	CR_MEMBER(atGoal),
	CR_MEMBER(atEndOfPath),

	CR_MEMBER(currWayPointDist),
	CR_MEMBER(prevWayPointDist),

	CR_MEMBER(pathRequestDelay),

	CR_MEMBER(numIdlingUpdates),
	CR_MEMBER(numIdlingSlowUpdates),
	CR_MEMBER(wantedHeading),

	CR_MEMBER(nextObstacleAvoidanceUpdate),

	CR_MEMBER(skidding),
	CR_MEMBER(flying),
	CR_MEMBER(reversing),
	CR_MEMBER(idling),
	CR_MEMBER(canReverse),
	CR_MEMBER(useMainHeading),

	CR_MEMBER(waypointDir),
	CR_MEMBER(flatFrontDir),
	CR_MEMBER(lastAvoidanceDir),
	CR_MEMBER(mainHeadingPos),

	CR_MEMBER(skidRotVector),
	CR_MEMBER(skidRotSpeed),
	CR_MEMBER(skidRotAccel),
	CR_ENUM_MEMBER(oldPhysState),

	CR_POSTLOAD(PostLoad)
));



std::vector<int2> CGroundMoveType::lineTable[LINETABLE_SIZE][LINETABLE_SIZE];

CGroundMoveType::CGroundMoveType(CUnit* owner):
	AMoveType(owner),
	pathController(owner ? IPathController::GetInstance(owner) : NULL),

	turnRate(0.1f),
	accRate(0.01f),
	decRate(0.01f),
	maxReverseSpeed(0.0f),
	wantedSpeed(0.0f),
	currentSpeed(0.0f),
	deltaSpeed(0.0f),

	pathId(0),
	goalRadius(0),

	currWayPoint(ZeroVector),
	nextWayPoint(ZeroVector),
	atGoal(false),
	atEndOfPath(false),

	currWayPointDist(0.0f),
	prevWayPointDist(0.0f),

	skidding(false),
	flying(false),
	reversing(false),
	idling(false),
	canReverse(owner ? owner->unitDef->rSpeed > 0.0f : false),
	useMainHeading(false),

	skidRotVector(UpVector),
	skidRotSpeed(0.0f),
	skidRotAccel(0.0f),

	oldPhysState(CSolidObject::OnGround),

	flatFrontDir(0.0f, 0.0, 1.0f),
	lastAvoidanceDir(ZeroVector),
	mainHeadingPos(ZeroVector),

	nextObstacleAvoidanceUpdate(0),
	pathRequestDelay(0),

	numIdlingUpdates(0),
	numIdlingSlowUpdates(0),

	wantedHeading(0)
{
	if (owner && owner->unitDef) {
		// maxSpeed is set in AMoveType's ctor
		maxReverseSpeed = owner->unitDef->rSpeed / GAME_SPEED;

		turnRate = owner->unitDef->turnRate;
		accRate = std::max(0.01f, owner->unitDef->maxAcc);
		decRate = std::max(0.01f, owner->unitDef->maxDec);
	}
}

CGroundMoveType::~CGroundMoveType()
{
	if (pathId != 0) {
		pathManager->DeletePath(pathId);
	}

	IPathController::FreeInstance(pathController);
}

void CGroundMoveType::PostLoad()
{
	pathController = IPathController::GetInstance(owner);

	// HACK: re-initialize path after load
	if (pathId != 0) {
		pathId = pathManager->RequestPath(owner, owner->moveDef, owner->pos, goalPos, goalRadius, true);
	}
}

bool CGroundMoveType::Update()
{
	ASSERT_SYNCED(owner->pos);

	if (owner->GetTransporter() != NULL) {
		return false;
	}

	if (OnSlope(1.0f)) {
		skidding = true;
	}
	if (skidding) {
		UpdateSkid();
		return false;
	}

	ASSERT_SYNCED(owner->pos);

	// set drop height when we start to drop
	if (owner->falling) {
		UpdateControlledDrop();
		return false;
	}

	ASSERT_SYNCED(owner->pos);

	bool hasMoved = false;
	bool wantReverse = false;

	const short heading = owner->heading;

	if (owner->IsStunned() || owner->beingBuilt) {
		owner->script->StopMoving();
		ChangeSpeed(0.0f, false);
	} else {
		if (owner->fpsControlPlayer != NULL) {
			wantReverse = UpdateDirectControl();
		} else {
			wantReverse = FollowPath();
		}
	}

	// these must be executed even when stunned (so
	// units do not get buried by restoring terrain)
	UpdateOwnerPos(wantReverse);
	AdjustPosToWaterLine();
	HandleObjectCollisions();

	ASSERT_SANE_OWNER_SPEED(owner->speed);

	// <dif> is normally equal to owner->speed (if no collisions)
	// we need more precision (less tolerance) in the y-dimension
	// for all-terrain units that are slowed down a lot on cliffs
	const float3 posDif = owner->pos - oldPos;
	const float3 cmpEps = float3(float3::CMP_EPS, float3::CMP_EPS * 1e-2f, float3::CMP_EPS);

	if (posDif.equals(ZeroVector, cmpEps)) {
		// note: the float3::== test is not exact, so even if this
		// evaluates to true the unit might still have an epsilon
		// speed vector --> nullify it to prevent apparent visual
		// micro-stuttering (speed is used to extrapolate drawPos)
		owner->speed = ZeroVector;

		// negative y-coordinates indicate temporary waypoints that
		// only exist while we are still waiting for the pathfinder
		// (so we want to avoid being considered "idle", since that
		// will cause our path to be re-requested and again give us
		// a temporary waypoint, etc.)
		// NOTE: this is only relevant for QTPFS (at present)
		// if the unit is just turning in-place over several frames
		// (eg. to maneuver around an obstacle), do not consider it
		// as "idling"
		idling = true;
		idling &= (currWayPoint.y != -1.0f && nextWayPoint.y != -1.0f);
		idling &= (std::abs(owner->heading - heading) < turnRate);
		hasMoved = false;
	} else {
		// note: HandleObjectCollisions() may have negated the position set
		// by UpdateOwnerPos() (so that owner->pos is again equal to oldPos)
		// note: the idling-check can only succeed if we are oriented in the
		// direction of our waypoint, which compensates for the fact distance
		// decreases much less quickly when moving orthogonal to <waypointDir>
		oldPos = owner->pos;

		const float3 ffd = flatFrontDir * posDif.SqLength() * 0.5f;
		const float3 wpd = waypointDir * ((int(!reversing) * 2) - 1);

		// too many false negatives: speed is unreliable if stuck behind an obstacle
		//   idling = (owner->speed.SqLength() < (accRate * accRate));
		//   idling &= (Square(currWayPointDist - prevWayPointDist) <= (accRate * accRate));
		// too many false positives: waypoint-distance delta and speed vary too much
		//   idling = (Square(currWayPointDist - prevWayPointDist) < owner->speed.SqLength());
		// too many false positives: many slow units cannot even manage 1 elmo/frame
		//   idling = (Square(currWayPointDist - prevWayPointDist) < 1.0f);
		idling = true;
		idling &= (math::fabs(posDif.y) < math::fabs(cmpEps.y * owner->pos.y));
		idling &= (Square(currWayPointDist - prevWayPointDist) < ffd.dot(wpd));
		hasMoved = true;
	}

	return hasMoved;
}

void CGroundMoveType::SlowUpdate()
{
	if (owner->GetTransporter() != NULL) {
		if (progressState == Active) {
			StopEngine();
		}
	} else {
		if (progressState == Active) {
			if (pathId != 0) {
				if (idling) {
					numIdlingSlowUpdates = std::min(MAX_IDLING_SLOWUPDATES, int(numIdlingSlowUpdates + 1));
				} else {
					numIdlingSlowUpdates = std::max(0, int(numIdlingSlowUpdates - 1));
				}

				if (numIdlingUpdates > (SHORTINT_MAXVALUE / turnRate)) {
					// case A: we have a path but are not moving
					LOG_L(L_DEBUG,
							"SlowUpdate: unit %i has pathID %i but %i ETA failures",
							owner->id, pathId, numIdlingUpdates);

					if (numIdlingSlowUpdates < MAX_IDLING_SLOWUPDATES) {
						StopEngine();
						StartEngine();
					} else {
						// unit probably ended up on a non-traversable
						// square, or got stuck in a non-moving crowd
						Fail();
					}
				}
			} else {
				if (gs->frameNum > pathRequestDelay) {
					// case B: we want to be moving but don't have a path
					LOG_L(L_DEBUG, "SlowUpdate: unit %i has no path", owner->id);

					StopEngine();
					StartEngine();
				}
			}
		}

		if (!flying) {
			// move us into the map, and update <oldPos>
			// to prevent any extreme changes in <speed>
			if (!owner->pos.IsInBounds()) {
				owner->Move3D(oldPos = owner->pos.cClampInBounds(), false);
			}
		}
	}

	AMoveType::SlowUpdate();
}


/*
Sets unit to start moving against given position with max speed.
*/
void CGroundMoveType::StartMoving(float3 pos, float goalRadius) {
	StartMoving(pos, goalRadius, (reversing? maxReverseSpeed: maxSpeed));
}


/*
Sets owner unit to start moving against given position with requested speed.
*/
void CGroundMoveType::StartMoving(float3 moveGoalPos, float _goalRadius, float speed)
{
#ifdef TRACE_SYNC
	tracefile << "[" << __FUNCTION__ << "] ";
	tracefile << owner->pos.x << " " << owner->pos.y << " " << owner->pos.z << " " << owner->id << "\n";
#endif

	if (progressState == Active) {
		StopEngine();
	}

	// set the new goal
	goalPos.x = moveGoalPos.x;
	goalPos.z = moveGoalPos.z;
	goalPos.y = 0.0f;
	goalRadius = _goalRadius;
	atGoal = false;

	useMainHeading = false;
	progressState = Active;

	numIdlingUpdates = 0;
	numIdlingSlowUpdates = 0;

	currWayPointDist = 0.0f;
	prevWayPointDist = 0.0f;

	LOG_L(L_DEBUG, "StartMoving: starting engine for unit %i", owner->id);

	StartEngine();

	#if (PLAY_SOUNDS == 1)
	if (owner->team == gu->myTeam) {
		Channels::General.PlayRandomSample(owner->unitDef->sounds.activate, owner);
	}
	#endif
}

void CGroundMoveType::StopMoving() {
#ifdef TRACE_SYNC
	tracefile << "[" << __FUNCTION__ << "] ";
	tracefile << owner->pos.x << " " << owner->pos.y << " " << owner->pos.z << " " << owner->id << "\n";
#endif

	LOG_L(L_DEBUG, "StopMoving: stopping engine for unit %i", owner->id);

	StopEngine();

	useMainHeading = false;
	progressState = Done;
}



bool CGroundMoveType::FollowPath()
{
	bool wantReverse = false;

	if (pathId == 0) {
		ChangeSpeed(0.0f, false);
		SetMainHeading();
	} else {
		ASSERT_SYNCED(currWayPoint);
		ASSERT_SYNCED(nextWayPoint);
		ASSERT_SYNCED(owner->pos);

		prevWayPointDist = currWayPointDist;
		currWayPointDist = owner->pos.distance2D(currWayPoint);

		{
			// NOTE:
			//     uses owner->pos instead of currWayPoint (ie. not the same as atEndOfPath)
			//
			//     if our first command is a build-order, then goalRadius is set to our build-range
			//     and we cannot increase tolerance safely (otherwise the unit might stop when still
			//     outside its range and fail to start construction)
			const float curGoalDistSq = (owner->pos - goalPos).SqLength2D();
			const float minGoalDistSq = (UNIT_HAS_MOVE_CMD(owner))?
				Square(goalRadius * (numIdlingSlowUpdates + 1)):
				Square(goalRadius                             );

			atGoal |= (curGoalDistSq < minGoalDistSq);
		}

		if (!atGoal) {
			if (!idling) {
				numIdlingUpdates = std::max(0, int(numIdlingUpdates - 1));
			} else {
				numIdlingUpdates = std::min(SHORTINT_MAXVALUE, int(numIdlingUpdates + 1));
			}
		}

		if (!atEndOfPath) {
			GetNextWayPoint();
		} else {
			if (atGoal) {
				Arrived();
			}
		}

		// set direction to waypoint AFTER requesting it
		waypointDir.x = currWayPoint.x - owner->pos.x;
		waypointDir.z = currWayPoint.z - owner->pos.z;
		waypointDir.y = 0.0f;
		waypointDir.SafeNormalize();

		ASSERT_SYNCED(waypointDir);

		if (waypointDir.dot(flatFrontDir) < 0.0f) {
			wantReverse = WantReverse(waypointDir);
		}

		// apply obstacle avoidance (steering)
		const float3 rawWantedDir = waypointDir * (int(!wantReverse) * 2 - 1);
		const float3& modWantedDir = GetObstacleAvoidanceDir(rawWantedDir);

		// ASSERT_SYNCED(modWantedDir);

		ChangeHeading(GetHeadingFromVector(modWantedDir.x, modWantedDir.z));
		ChangeSpeed(maxWantedSpeed, wantReverse);
	}

	pathManager->UpdatePath(owner, pathId);
	return wantReverse;
}

void CGroundMoveType::ChangeSpeed(float newWantedSpeed, bool wantReverse, bool fpsMode)
{
	wantedSpeed = newWantedSpeed;

	// round low speeds to zero
	if (wantedSpeed <= 0.0f && currentSpeed < 0.01f) {
		currentSpeed = 0.0f;
		deltaSpeed = 0.0f;
		return;
	}

	// first calculate the "unrestricted" speed and acceleration
	float targetSpeed = wantReverse? maxReverseSpeed: maxSpeed;

	#if (WAIT_FOR_PATH == 1)
	// don't move until we have an actual path, trying to hide queuing
	// lag is too dangerous since units can blindly drive into objects,
	// cliffs, etc. (requires the QTPFS idle-check in Update)
	if (currWayPoint.y == -1.0f && nextWayPoint.y == -1.0f) {
		targetSpeed = 0.0f;
	} else
	#endif
	{
		if (wantedSpeed > 0.0f) {
			const UnitDef* ud = owner->unitDef;
			const MoveDef* md = owner->moveDef;

			// the pathfinders do NOT check the entire footprint to determine
			// passability wrt. terrain (only wrt. structures), so we look at
			// the center square ONLY for our current speedmod
			const float groundSpeedMod = CMoveMath::GetPosSpeedMod(*md, owner->pos, flatFrontDir);

			const float curGoalDistSq = (owner->pos - goalPos).SqLength2D();
			const float minGoalDistSq = Square(BrakingDistance(currentSpeed));

			const float3& waypointDifFwd = waypointDir;
			const float3  waypointDifRev = -waypointDifFwd;

			const float3& waypointDif = reversing? waypointDifRev: waypointDifFwd;
			const short turnDeltaHeading = owner->heading - GetHeadingFromVector(waypointDif.x, waypointDif.z);

			// NOTE: <= 2 because every CMD_MOVE has a trailing CMD_SET_WANTED_MAX_SPEED
			const bool startBraking = (UNIT_CMD_QUE_SIZE(owner) <= 2 && curGoalDistSq <= minGoalDistSq);

			if (!fpsMode && turnDeltaHeading != 0) {
				// only auto-adjust speed for turns when not in FPS mode
				const float reqTurnAngle = math::fabs(180.0f * (owner->heading - wantedHeading) / SHORTINT_MAXVALUE);
				const float maxTurnAngle = (turnRate / SPRING_CIRCLE_DIVS) * 360.0f;

				float turnSpeed = (reversing)? maxReverseSpeed: maxSpeed;

				if (reqTurnAngle != 0.0f) {
					turnSpeed *= std::min(maxTurnAngle / reqTurnAngle, 1.0f);
				}

				if (waypointDir.SqLength() > 0.1f) {
					if (!ud->turnInPlace) {
						targetSpeed = std::max(ud->turnInPlaceSpeedLimit, turnSpeed);
					} else {
						if (reqTurnAngle > ud->turnInPlaceAngleLimit) {
							targetSpeed = turnSpeed;
						}
					}
				}

				if (atEndOfPath) {
					// at this point, Update() will no longer call GetNextWayPoint()
					// and we must slow down to prevent entering an infinite circle
					targetSpeed = std::min(targetSpeed, (currWayPointDist * PI) / (SPRING_CIRCLE_DIVS / turnRate));
				}
			}

			// now apply the terrain and command restrictions
			// NOTE:
			//     if wantedSpeed > targetSpeed, the unit will
			//     not accelerate to speed > targetSpeed unless
			//     its actual max{Reverse}Speed is also changed
			//
			//     raise wantedSpeed iff the terrain-modifier is
			//     larger than 1 (so units still get their speed
			//     bonus correctly), otherwise leave it untouched
			wantedSpeed *= std::max(groundSpeedMod, 1.0f);
			targetSpeed *= groundSpeedMod;
			targetSpeed *= (1 - startBraking);
			targetSpeed = std::min(targetSpeed, wantedSpeed);
		} else {
			targetSpeed = 0.0f;
		}
	}

	deltaSpeed = pathController->GetDeltaSpeed(
		pathId,
		targetSpeed,
		currentSpeed,
		accRate,
		decRate,
		wantReverse,
		reversing
	);
}

/*
 * Changes the heading of the owner.
 * FIXME near-duplicate of HoverAirMoveType::UpdateHeading
 */
void CGroundMoveType::ChangeHeading(short newHeading) {
	if (flying) return;
	if (owner->GetTransporter() != NULL) return;

	owner->heading += pathController->GetDeltaHeading(pathId, (wantedHeading = newHeading), owner->heading, turnRate);

	owner->UpdateDirVectors(!owner->upright && maxSpeed > 0.0f, true);
	owner->UpdateMidAndAimPos();

	flatFrontDir = owner->frontdir;
	flatFrontDir.y = 0.0f;
	flatFrontDir.Normalize();
}




bool CGroundMoveType::CanApplyImpulse(const float3& impulse)
{
	// NOTE: ships must be able to receive impulse too (for collision handling)
	if (owner->beingBuilt)
		return false;
	// TODO: or apply impulse to the transporter?
	if (owner->GetTransporter() != NULL)
		return false;
	if (impulse.SqLength() <= 0.01f)
		return false;
	// TODO (92.0+):
	//   we should not delay the skidding-state until owner has "accumulated" an
	//   arbitrary hardcoded amount of impulse (possibly across several frames!),
	//   but enter it on any vector with non-zero length --> most weapon impulses
	//   will just reduce the unit's speed a bit
	//   there should probably be a configurable minimum-impulse below which the
	//   unit does not react at all (can re-use SolidObject::residualImpulse for
	//   this) but also does NOT store the impulse like a small-charge capacitor,
	//   see CUnit::StoreImpulse
	if (owner->residualImpulse.SqLength() <= 9.0f)
		return false;

	skidding = true;
	useHeading = false;

	skidRotSpeed = 0.0f;
	skidRotAccel = 0.0f;

	float3 newSpeed = owner->speed + owner->residualImpulse;
	float3 skidDir = owner->frontdir;

	if (newSpeed.SqLength2D() >= 0.01f) {
		skidDir = newSpeed;
		skidDir.y = 0.0f;
		skidDir.Normalize();
	}

	skidRotVector = skidDir.cross(UpVector);

	oldPhysState = owner->physicalState;
	owner->physicalState = CSolidObject::Flying;

	if (newSpeed.dot(ground->GetNormal(owner->pos.x, owner->pos.z)) > 0.2f) {
		skidRotAccel = (gs->randFloat() - 0.5f) * 0.04f;
		flying = true;
	}

	ASSERT_SANE_OWNER_SPEED(newSpeed);
	// indicate we want to react to the impulse
	return true;
}

void CGroundMoveType::UpdateSkid()
{
	ASSERT_SYNCED(owner->midPos);

	const float3& pos = owner->pos;
	      float3& speed  = owner->speed;

	const UnitDef* ud = owner->unitDef;
	const float groundHeight = GetGroundHeight(pos);

	if (flying) {
		// water drag
		if (pos.y < 0.0f)
			speed *= 0.95f;

		const float impactSpeed = pos.IsInBounds()?
			-speed.dot(ground->GetNormal(pos.x, pos.z)):
			-speed.dot(UpVector);
		const float impactDamageMult = impactSpeed * owner->mass * COLLISION_DAMAGE_MULT;
		const bool doColliderDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > ud->minCollisionSpeed && ud->minCollisionSpeed >= 0.0f);

		if (groundHeight > pos.y) {
			// ground impact, stop flying
			flying = false;

			owner->Move1D(groundHeight, 1, false);

			// deal ground impact damage
			// TODO:
			//     bouncing behaves too much like a rubber-ball,
			//     most impact energy needs to go into the ground
			if (doColliderDamage) {
				owner->DoDamage(DamageArray(impactDamageMult), ZeroVector, NULL, -CSolidObject::DAMAGE_COLLISION_GROUND, -1);
			}

			skidRotSpeed = 0.0f;
			// skidRotAccel = 0.0f;
		} else {
			speed.y += mapInfo->map.gravity;
		}
	} else {
		// *assume* this means the unit is still on the ground
		// (Lua gadgetry can interfere with our "physics" logic)
		float speedf = speed.Length();
		float skidRotSpd = 0.0f;

		const bool onSlope = OnSlope(-1.0f);
		const float speedReduction = 0.35f;

		if (speedf < speedReduction && !onSlope) {
			// stop skidding
			speed = ZeroVector;

			skidding = false;
			useHeading = true;

			owner->physicalState = oldPhysState;

			skidRotSpd = math::floor(skidRotSpeed + skidRotAccel + 0.5f);
			skidRotAccel = (skidRotSpd - skidRotSpeed) * 0.5f;
			skidRotAccel *= (PI / 180.0f);

			ChangeHeading(owner->heading);
		} else {
			if (onSlope) {
				const float3 normal = ground->GetNormal(pos.x, pos.z);
				const float3 normalForce = normal * normal.dot(UpVector * mapInfo->map.gravity);
				const float3 newForce = UpVector * mapInfo->map.gravity - normalForce;

				speed += newForce;
				speedf = speed.Length();
				speed *= (1.0f - (0.1f * normal.y));
			} else {
				speed *= (1.0f - std::min(1.0f, speedReduction / speedf)); // clamped 0..1
			}

			// number of frames until rotational speed would drop to 0
			const float remTime = std::max(1.0f, speedf / speedReduction);

			skidRotSpd = math::floor(skidRotSpeed + skidRotAccel * (remTime - 1.0f) + 0.5f);
			skidRotAccel = (skidRotSpd - skidRotSpeed) / remTime;
			skidRotAccel *= (PI / 180.0f);

			if (math::floor(skidRotSpeed) != math::floor(skidRotSpeed + skidRotAccel)) {
				skidRotSpeed = 0.0f;
				skidRotAccel = 0.0f;
			}
		}

		if ((groundHeight - pos.y) < (speed.y + mapInfo->map.gravity)) {
			speed.y += mapInfo->map.gravity;

			flying = true;
			skidding = true; // flying requires skidding
			useHeading = false; // and relies on CalcSkidRot
		} else if ((groundHeight - pos.y) > speed.y) {
			const float3& normal = (pos.IsInBounds())? ground->GetNormal(pos.x, pos.z): UpVector;
			const float dot = speed.dot(normal);

			if (dot > 0.0f) {
				speed *= 0.95f;
			} else {
				speed += (normal * (math::fabs(speed.dot(normal)) + 0.1f)) * 1.9f;
				speed *= 0.8f;
			}
		}
	}

	// translate before rotate, match terrain normal if not in air
	owner->Move3D(speed, true);
	owner->UpdateDirVectors(true, true);

	if (skidding) {
		CalcSkidRot();
		CheckCollisionSkid();
	} else {
		// do this here since ::Update returns early if it calls us
		HandleObjectCollisions();
	}

	// always update <oldPos> here so that <speed> does not make
	// extreme jumps when the unit transitions from skidding back
	// to non-skidding
	oldPos = owner->pos;

	ASSERT_SANE_OWNER_SPEED(speed);
	ASSERT_SYNCED(owner->midPos);
}

void CGroundMoveType::UpdateControlledDrop()
{
	if (!owner->falling)
		return;

	const float3& pos = owner->pos;
	      float3& speed = owner->speed;

	speed.y += (mapInfo->map.gravity * owner->fallSpeed);
	speed.y = std::min(speed.y, 0.0f);

	owner->Move3D(speed, true);

	// water drag
	if (pos.y < 0.0f)
		speed *= 0.90;

	const float wh = GetGroundHeight(pos);

	if (wh > pos.y) {
		// ground impact
		owner->Move1D(wh, 1, (owner->falling = false));
		owner->script->Landed(); //stop parachute animation
	}
}

void CGroundMoveType::CheckCollisionSkid()
{
	CUnit* collider = owner;

	// NOTE:
	//     the QuadField::Get* functions check o->midPos,
	//     but the quad(s) that objects are stored in are
	//     derived from o->pos (!)
	const float3& pos = collider->pos;
	const UnitDef* colliderUD = collider->unitDef;
	const vector<CUnit*>& nearUnits = quadField->GetUnitsExact(pos, collider->radius);
	const vector<CFeature*>& nearFeatures = quadField->GetFeaturesExact(pos, collider->radius);

	vector<CUnit*>::const_iterator ui;
	vector<CFeature*>::const_iterator fi;

	for (ui = nearUnits.begin(); ui != nearUnits.end(); ++ui) {
		CUnit* collidee = *ui;
		const UnitDef* collideeUD = collider->unitDef;

		const float sqDist = (pos - collidee->pos).SqLength();
		const float totRad = collider->radius + collidee->radius;

		if ((sqDist >= totRad * totRad) || (sqDist <= 0.01f)) {
			continue;
		}

		// stop units from reaching escape velocity
		const float3 dif = (pos - collidee->pos).SafeNormalize();

		if (collidee->moveDef == NULL) {
			const float impactSpeed = -collider->speed.dot(dif);
			const float impactDamageMult = std::min(impactSpeed * collider->mass * COLLISION_DAMAGE_MULT, MAX_UNIT_SPEED);

			const bool doColliderDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > colliderUD->minCollisionSpeed && colliderUD->minCollisionSpeed >= 0.0f);
			const bool doCollideeDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > collideeUD->minCollisionSpeed && collideeUD->minCollisionSpeed >= 0.0f);

			if (impactSpeed <= 0.0f)
				continue;

			collider->Move3D(dif * impactSpeed, true);
			collider->speed += ((dif * impactSpeed) * 1.8f);

			// damage the collider, no added impulse
			if (doColliderDamage) {
				collider->DoDamage(DamageArray(impactDamageMult), ZeroVector, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
			}
			// damage the (static) collidee based on collider's mass, no added impulse
			if (doCollideeDamage) {
				collidee->DoDamage(DamageArray(impactDamageMult), ZeroVector, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
			}
		} else {
			assert(collider->mass > 0.0f && collidee->mass > 0.0f);

			// don't conserve momentum (impact speed is halved, so impulses are too)
			// --> collisions are neither truly elastic nor truly inelastic to prevent
			// the simulation from blowing up from impulses applied to tight groups of
			// units
			const float impactSpeed = (collidee->speed - collider->speed).dot(dif) * 0.5f;
			const float colliderRelMass = (collider->mass / (collider->mass + collidee->mass));
			const float colliderRelImpactSpeed = impactSpeed * (1.0f - colliderRelMass);
			const float collideeRelImpactSpeed = impactSpeed * (       colliderRelMass); 

			const float  colliderImpactDmgMult = std::min(colliderRelImpactSpeed * collider->mass * COLLISION_DAMAGE_MULT, MAX_UNIT_SPEED);
			const float  collideeImpactDmgMult = std::min(collideeRelImpactSpeed * collider->mass * COLLISION_DAMAGE_MULT, MAX_UNIT_SPEED);
			const float3 colliderImpactImpulse = dif * colliderRelImpactSpeed;
			const float3 collideeImpactImpulse = dif * collideeRelImpactSpeed;

			const bool doColliderDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > colliderUD->minCollisionSpeed && colliderUD->minCollisionSpeed >= 0.0f);
			const bool doCollideeDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > collideeUD->minCollisionSpeed && collideeUD->minCollisionSpeed >= 0.0f);

			if (impactSpeed <= 0.0f)
				continue;

			collider->Move3D( colliderImpactImpulse, true);
			collidee->Move3D(-collideeImpactImpulse, true);

			// damage the collider
			if (doColliderDamage) {
				collider->DoDamage(DamageArray(colliderImpactDmgMult), dif * colliderImpactDmgMult, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
			}
			// damage the collidee
			if (doCollideeDamage) {
				collidee->DoDamage(DamageArray(collideeImpactDmgMult), dif * -collideeImpactDmgMult, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
			}

			collider->speed += colliderImpactImpulse;
			collidee->speed -= collideeImpactImpulse;
		}
	}

	for (fi = nearFeatures.begin(); fi != nearFeatures.end(); ++fi) {
		CFeature* f = *fi;

		if (!f->blocking)
			continue;

		const float sqDist = (pos - f->pos).SqLength();
		const float totRad = collider->radius + f->radius;

		if ((sqDist >= totRad * totRad) || (sqDist <= 0.01f))
			continue;

		const float3 dif = (pos - f->pos).SafeNormalize();
		const float impactSpeed = -collider->speed.dot(dif);
		const float impactDamageMult = std::min(impactSpeed * collider->mass * COLLISION_DAMAGE_MULT, MAX_UNIT_SPEED);
		const float3 impactImpulse = dif * impactSpeed;
		const bool doColliderDamage = (modInfo.allowUnitCollisionDamage && impactSpeed > colliderUD->minCollisionSpeed && colliderUD->minCollisionSpeed >= 0.0f);

		if (impactSpeed <= 0.0f)
			continue;

		collider->Move3D(impactImpulse, true);
		collider->speed += (impactImpulse * 1.8f);

		// damage the collider, no added impulse (!) 
		if (doColliderDamage) {
			collider->DoDamage(DamageArray(impactDamageMult), ZeroVector, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
		}

		// damage the collidee feature based on collider's mass
		f->DoDamage(DamageArray(impactDamageMult), -impactImpulse, NULL, -CSolidObject::DAMAGE_COLLISION_OBJECT, -1);
	}

	ASSERT_SANE_OWNER_SPEED(collider->speed);
}

void CGroundMoveType::CalcSkidRot()
{
	skidRotSpeed += skidRotAccel;
	skidRotSpeed *= 0.999f;
	skidRotAccel *= 0.95f;

	const float angle = (skidRotSpeed / GAME_SPEED) * (PI * 2.0f);
	const float cosp = math::cos(angle);
	const float sinp = math::sin(angle);

	float3 f1 = skidRotVector * skidRotVector.dot(owner->frontdir);
	float3 f2 = owner->frontdir - f1;

	float3 r1 = skidRotVector * skidRotVector.dot(owner->rightdir);
	float3 r2 = owner->rightdir - r1;

	float3 u1 = skidRotVector * skidRotVector.dot(owner->updir);
	float3 u2 = owner->updir - u1;

	f2 = f2 * cosp + f2.cross(skidRotVector) * sinp;
	r2 = r2 * cosp + r2.cross(skidRotVector) * sinp;
	u2 = u2 * cosp + u2.cross(skidRotVector) * sinp;

	owner->frontdir = f1 + f2;
	owner->rightdir = r1 + r2;
	owner->updir    = u1 + u2;

	owner->UpdateMidAndAimPos();
}




/*
 * Dynamic obstacle avoidance, helps the unit to
 * follow the path even when it's not perfect.
 */
float3 CGroundMoveType::GetObstacleAvoidanceDir(const float3& desiredDir) {
	#if (IGNORE_OBSTACLES == 1)
	return desiredDir;
	#endif

	// Obstacle-avoidance-system only needs to be run if the unit wants to move
	if (pathId == 0)
		return ZeroVector;

	// Speed-optimizer. Reduces the times this system is run.
	if (gs->frameNum < nextObstacleAvoidanceUpdate)
		return lastAvoidanceDir;

	float3 avoidanceVec = ZeroVector;
	float3 avoidanceDir = desiredDir;

	lastAvoidanceDir = desiredDir;
	nextObstacleAvoidanceUpdate = gs->frameNum + 1;

	CUnit* avoider = owner;
	// const UnitDef* avoiderUD = avoider->unitDef;
	const MoveDef* avoiderMD = avoider->moveDef;

	// degenerate case: if facing anti-parallel to desired direction,
	// do not actively avoid obstacles since that can interfere with
	// normal waypoint steering (if the final avoidanceDir demands a
	// turn in the opposite direction of desiredDir)
	if (avoider->frontdir.dot(desiredDir) < 0.0f)
		return lastAvoidanceDir;

	static const float AVOIDER_DIR_WEIGHT = 1.0f;
	static const float DESIRED_DIR_WEIGHT = 0.5f;
	static const float MAX_AVOIDEE_COSINE = math::cosf(120.0f * (PI / 180.0f));
	static const float LAST_DIR_MIX_ALPHA = 0.7f;

	// now we do the obstacle avoidance proper
	// avoider always uses its never-rotated MoveDef footprint
	const float avoidanceRadius = std::max(currentSpeed, 1.0f) * (avoider->radius * 2.0f);
	const float avoiderRadius = FOOTPRINT_RADIUS(avoiderMD->xsize, avoiderMD->zsize, 1.0f);

	const vector<CSolidObject*>& objects = quadField->GetSolidsExact(avoider->pos, avoidanceRadius);

	for (vector<CSolidObject*>::const_iterator oi = objects.begin(); oi != objects.end(); ++oi) {
		const CSolidObject* avoidee = *oi;
		const MoveDef* avoideeMD = avoidee->moveDef;
		const UnitDef* avoideeUD = dynamic_cast<const UnitDef*>(avoidee->objectDef);

		// cases in which there is no need to avoid this obstacle
		if (avoidee == owner)
			continue;
		// ignore aircraft (or flying ground units)
		if (avoidee->physicalState == CSolidObject::Hovering || avoidee->physicalState == CSolidObject::Flying)
			continue;
		if (CMoveMath::IsNonBlocking(*avoiderMD, avoidee, avoider))
			continue;
		if (!CMoveMath::CrushResistant(*avoiderMD, avoidee))
			continue;

		const bool avoideeMobile  = (avoideeMD != NULL);
		const bool avoideeMovable = (avoideeUD != NULL && !avoideeUD->pushResistant);

		const float3 avoideeVector = (avoider->pos + avoider->speed) - (avoidee->pos + avoidee->speed);

		// use the avoidee's MoveDef footprint as radius if it is mobile
		// use the avoidee's Unit (not UnitDef) footprint as radius otherwise
		const float avoideeRadius = avoideeMobile?
			FOOTPRINT_RADIUS(avoideeMD->xsize, avoideeMD->zsize, 1.0f):
			FOOTPRINT_RADIUS(avoidee  ->xsize, avoidee  ->zsize, 1.0f);
		const float avoidanceRadiusSum = avoiderRadius + avoideeRadius;
		const float avoidanceMassSum = avoider->mass + avoidee->mass;
		const float avoideeMassScale = (avoideeMobile)? (avoidee->mass / avoidanceMassSum): 1.0f;
		const float avoideeDistSq = avoideeVector.SqLength();
		const float avoideeDist   = fastmath::sqrt2(avoideeDistSq) + 0.01f;

		// do not bother steering around idling MOBILE objects
		// (since collision handling will just push them aside)
		if (avoideeMobile && avoideeMovable) {
			if (!avoiderMD->avoidMobilesOnPath || (!avoidee->isMoving && avoidee->allyteam == avoider->allyteam)) {
				continue;
			}
		}

		// ignore objects that are more than this many degrees off-center from us
		// NOTE:
		//   if MAX_AVOIDEE_COSINE is too small, then this condition can be true
		//   one frame and false the next (after avoider has turned) causing the
		//   avoidance vector to oscillate --> units with turnInPlace = true will
		//   slow to a crawl as a result
		if (avoider->frontdir.dot(-(avoideeVector / avoideeDist)) < MAX_AVOIDEE_COSINE)
			continue;

		if (avoideeDistSq >= Square(std::max(currentSpeed, 1.0f) * GAME_SPEED + avoidanceRadiusSum))
			continue;
		if (avoideeDistSq >= avoider->pos.SqDistance2D(goalPos))
			continue;

		// if object and unit in relative motion are closing in on one another
		// (or not yet fully apart), then the object is on the path of the unit
		// and they are not collided
		if (DEBUG_DRAWING_ENABLED) {
			GML_RECMUTEX_LOCK(sel); // GetObstacleAvoidanceDir

			if (selectedUnits.selectedUnits.find(owner) != selectedUnits.selectedUnits.end()) {
				geometricObjects->AddLine(avoider->pos + (UpVector * 20.0f), avoidee->pos + (UpVector * 20.0f), 3, 1, 4);
			}
		}

		float avoiderTurnSign = ((avoidee->pos.dot(avoider->rightdir) - avoider->pos.dot(avoider->rightdir)) <= 0.0f) * 2.0f - 1.0f;
		float avoideeTurnSign = ((avoider->pos.dot(avoidee->rightdir) - avoidee->pos.dot(avoidee->rightdir)) <= 0.0f) * 2.0f - 1.0f;

		// for mobile units, avoidance-response is modulated by angle
		// between avoidee's and avoider's frontdir such that maximal
		// avoidance occurs when they are anti-parallel
		const float avoidanceCosAngle = Clamp(avoider->frontdir.dot(avoidee->frontdir), -1.0f, 1.0f);
		const float avoidanceResponse = (1.0f - avoidanceCosAngle * int(avoideeMobile)) + 0.1f;
		const float avoidanceFallOff  = (1.0f - std::min(1.0f, avoideeDist / (5.0f * avoidanceRadiusSum)));

		// if parties are anti-parallel, it is always more efficient for
		// both to turn in the same local-space direction (either R/R or
		// L/L depending on relative object positions) but there exists
		// a range of orientations for which the signs are not equal
		//
		// (this is also true for the parallel situation, but there the
		// degeneracy only occurs when one of the parties is behind the
		// other and can be ignored)
		if (avoidanceCosAngle < 0.0f)
			avoiderTurnSign = std::max(avoiderTurnSign, avoideeTurnSign);

		avoidanceDir = avoider->rightdir * AVOIDER_DIR_WEIGHT * avoiderTurnSign;
		avoidanceVec += (avoidanceDir * avoidanceResponse * avoidanceFallOff * avoideeMassScale);
	}


	// use a weighted combination of the desired- and the avoidance-directions
	// also linearly smooth it using the vector calculated the previous frame
	avoidanceDir = (desiredDir * DESIRED_DIR_WEIGHT + avoidanceVec).SafeNormalize();
	avoidanceDir = lastAvoidanceDir * LAST_DIR_MIX_ALPHA + avoidanceDir * (1.0f - LAST_DIR_MIX_ALPHA);

	if (DEBUG_DRAWING_ENABLED) {
		GML_RECMUTEX_LOCK(sel); // GetObstacleAvoidanceDir

		if (selectedUnits.selectedUnits.find(owner) != selectedUnits.selectedUnits.end()) {
			const float3 p0 = owner->pos + (    UpVector * 20.0f);
			const float3 p1 =         p0 + (avoidanceVec * 40.0f);
			const float3 p2 =         p0 + (avoidanceDir * 40.0f);

			const int avFigGroupID = geometricObjects->AddLine(p0, p1, 8.0f, 1, 4);
			const int adFigGroupID = geometricObjects->AddLine(p0, p2, 8.0f, 1, 4);

			geometricObjects->SetColor(avFigGroupID, 1, 0.3f, 0.3f, 0.6f);
			geometricObjects->SetColor(adFigGroupID, 1, 0.3f, 0.3f, 0.6f);
		}
	}

	return (lastAvoidanceDir = avoidanceDir);
}



// Calculates an aproximation of the physical 2D-distance between given two objects.
float CGroundMoveType::Distance2D(CSolidObject* object1, CSolidObject* object2, float marginal)
{
	// calculate the distance in (x,z) depending
	// on the shape of the object footprints
	float dist2D;
	if (object1->xsize == object1->zsize || object2->xsize == object2->zsize) {
		// use xsize as a cylindrical radius.
		float3 distVec = (object1->midPos - object2->midPos);
		dist2D = distVec.Length2D() - (object1->xsize + object2->xsize) * SQUARE_SIZE / 2 + 2 * marginal;
	} else {
		// Pytagorean sum of the x and z distance.
		float3 distVec;
		const float xdiff = math::fabs(object1->midPos.x - object2->midPos.x);
		const float zdiff = math::fabs(object1->midPos.z - object2->midPos.z);

		distVec.x = xdiff - (object1->xsize + object2->xsize) * SQUARE_SIZE / 2 + 2 * marginal;
		distVec.z = zdiff - (object1->zsize + object2->zsize) * SQUARE_SIZE / 2 + 2 * marginal;

		if (distVec.x > 0.0f && distVec.z > 0.0f) {
			dist2D = distVec.Length2D();
		} else if (distVec.x < 0.0f && distVec.z < 0.0f) {
			dist2D = -distVec.Length2D();
		} else if (distVec.x > 0.0f) {
			dist2D = distVec.x;
		} else {
			dist2D = distVec.z;
		}
	}

	return dist2D;
}

// Creates a path to the goal.
void CGroundMoveType::GetNewPath()
{
	assert(pathId == 0);
	pathId = pathManager->RequestPath(owner, owner->moveDef, owner->pos, goalPos, goalRadius, true);

	if (pathId != 0) {
		atGoal = false;
		atEndOfPath = false;

		currWayPoint = pathManager->NextWayPoint(owner, pathId, 0,   owner->pos, 1.25f * SQUARE_SIZE, true);
		nextWayPoint = pathManager->NextWayPoint(owner, pathId, 0, currWayPoint, 1.25f * SQUARE_SIZE, true);

		pathController->SetRealGoalPosition(pathId, goalPos);
		pathController->SetTempGoalPosition(pathId, currWayPoint);
	} else {
		Fail();
	}

	// limit frequency of (case B) path-requests from SlowUpdate's
	pathRequestDelay = gs->frameNum + (UNIT_SLOWUPDATE_RATE << 1);
}



bool CGroundMoveType::CanGetNextWayPoint() {
	if (pathId == 0)
		return false;
	if (!pathController->AllowSetTempGoalPosition(pathId, nextWayPoint))
		return false;


	if (currWayPoint.y != -1.0f && nextWayPoint.y != -1.0f) {
		const float3& pos = owner->pos;
		      float3& cwp = (float3&) currWayPoint;
		      float3& nwp = (float3&) nextWayPoint;

		if (pathManager->PathUpdated(pathId)) {
			// path changed while we were following it (eg. due
			// to terrain deformation) in between two waypoints
			// but still has the same ID; in this case (which is
			// specific to QTPFS) we don't go through GetNewPath
			//
			cwp = pathManager->NextWayPoint(owner, pathId, 0, pos, 1.25f * SQUARE_SIZE, true);
			nwp = pathManager->NextWayPoint(owner, pathId, 0, cwp, 1.25f * SQUARE_SIZE, true);
		}

		if (DEBUG_DRAWING_ENABLED) {
			GML_RECMUTEX_LOCK(sel); // CanGetNextWayPoint

			if (selectedUnits.selectedUnits.find(owner) != selectedUnits.selectedUnits.end()) {
				// plot the vectors to {curr, next}WayPoint
				const int cwpFigGroupID = geometricObjects->AddLine(pos + (UpVector * 20.0f), cwp + (UpVector * (pos.y + 20.0f)), 8.0f, 1, 4);
				const int nwpFigGroupID = geometricObjects->AddLine(pos + (UpVector * 20.0f), nwp + (UpVector * (pos.y + 20.0f)), 8.0f, 1, 4);

				geometricObjects->SetColor(cwpFigGroupID, 1, 0.3f, 0.3f, 0.6f);
				geometricObjects->SetColor(nwpFigGroupID, 1, 0.3f, 0.3f, 0.6f);
			}
		}

		// perform a turn-radius check: if the waypoint
		// lies outside our turning circle, don't skip
		// it (since we can steer toward this waypoint
		// and pass it without slowing down)
		// note that we take the DIAMETER of the circle
		// to prevent sine-like "snaking" trajectories
		const int dirSign = int(!reversing) * 2 - 1;
		const float turnFrames = SPRING_CIRCLE_DIVS / turnRate;
		const float turnRadius = (owner->speed.Length() * turnFrames) / (PI + PI);
		const float waypointDot = Clamp(waypointDir.dot(flatFrontDir * dirSign), -1.0f, 1.0f);

		#if 1
		if (currWayPointDist > (turnRadius * 2.0f)) {
			return false;
		}
		if (currWayPointDist > SQUARE_SIZE && waypointDot >= 0.995f) {
			return false;
		}
		#else
		if ((currWayPointDist > std::max(turnRadius * 2.0f, 1.0f * SQUARE_SIZE)) && (waypointDot >= 0.0f)) {
			return false;
		}
		if ((currWayPointDist > std::max(turnRadius * 1.0f, 1.0f * SQUARE_SIZE)) && (waypointDot <  0.0f)) {
			return false;
		}
		if (math::acosf(waypointDot) < ((turnRate / SPRING_CIRCLE_DIVS) * (PI + PI))) {
			return false;
		}
		#endif

		{
			// check the rectangle between pos and cwp for obstacles
			// TODO: use the line-table? (faster but less accurate)
			const int xmin = std::min(cwp.x / SQUARE_SIZE, pos.x / SQUARE_SIZE) - 1, xmax = std::max(cwp.x / SQUARE_SIZE, pos.x / SQUARE_SIZE) + 1;
			const int zmin = std::min(cwp.z / SQUARE_SIZE, pos.z / SQUARE_SIZE) - 1, zmax = std::max(cwp.z / SQUARE_SIZE, pos.z / SQUARE_SIZE) + 1;

			//const UnitDef* ownerUD = owner->unitDef;
			const MoveDef* ownerMD = owner->moveDef;

			for (int x = xmin; x < xmax; x++) {
				for (int z = zmin; z < zmax; z++) {
					const bool noStructBlock = ((CMoveMath::SquareIsBlocked(*ownerMD, x, z, owner) & CMoveMath::BLOCK_STRUCTURE) == 0);
					const bool noGroundBlock = (CMoveMath::GetPosSpeedMod(*ownerMD, pos) >= 0.01f);

					if (noStructBlock && noGroundBlock) {
						continue;
					}
					if ((pos - cwp).SqLength() > (SQUARE_SIZE * SQUARE_SIZE)) {
						return false;
					}
				}
			}
		}

		{
			const float curGoalDistSq = (currWayPoint - goalPos).SqLength2D();
			const float minGoalDistSq = (UNIT_HAS_MOVE_CMD(owner))?
				Square(goalRadius * (numIdlingSlowUpdates + 1)):
				Square(goalRadius                             );

			// trigger Arrived on the next Update (but
			// only if we have non-temporary waypoints)
			// atEndOfPath |= (currWayPoint == nextWayPoint);
			atEndOfPath |= (curGoalDistSq < minGoalDistSq);
		}

		if (atEndOfPath) {
			currWayPoint = goalPos;
			nextWayPoint = goalPos;
			return false;
		}
	}

	return true;
}

void CGroundMoveType::GetNextWayPoint()
{
	if (CanGetNextWayPoint()) {
		pathController->SetTempGoalPosition(pathId, nextWayPoint);

		// NOTE: pathfinder implementation should ensure waypoints are not equal
		currWayPoint = nextWayPoint;
		nextWayPoint = pathManager->NextWayPoint(owner, pathId, 0, currWayPoint, 1.25f * SQUARE_SIZE, true);
	}

	if (nextWayPoint.x == -1.0f && nextWayPoint.z == -1.0f) {
		Fail();
	} else {
		#define CWP_BLOCK_MASK CMoveMath::SquareIsBlocked(*owner->moveDef, currWayPoint, owner)
		#define NWP_BLOCK_MASK CMoveMath::SquareIsBlocked(*owner->moveDef, nextWayPoint, owner)

		if ((CWP_BLOCK_MASK & CMoveMath::BLOCK_STRUCTURE) != 0 || (NWP_BLOCK_MASK & CMoveMath::BLOCK_STRUCTURE) != 0) {
			// this can happen if we crushed a non-blocking feature
			// and it spawned another feature which we cannot crush
			// (eg.) --> repath
			StopEngine();
			StartEngine();
		}

		#undef NWP_BLOCK_MASK
		#undef CWP_BLOCK_MASK
	}
}




/*
The distance the unit will move before stopping,
starting from given speed and applying maximum
brake rate.
*/
float CGroundMoveType::BrakingDistance(float speed) const
{
	const float rate = reversing? accRate: decRate;
	const float time = speed / std::max(rate, 0.001f);
	const float dist = 0.5f * rate * time * time;
	return dist;
}

/*
Gives the position this unit will end up at with full braking
from current velocity.
*/
float3 CGroundMoveType::Here()
{
	const float dist = BrakingDistance(currentSpeed);
	const int   sign = int(!reversing) * 2 - 1;

	const float3 pos2D = float3(owner->pos.x, 0.0f, owner->pos.z);
	const float3 dir2D = flatFrontDir * dist * sign;

	return (pos2D + dir2D);
}






void CGroundMoveType::StartEngine() {
	// ran only if the unit has no path and is not already at goal
	if (pathId == 0 && !atGoal) {
		GetNewPath();

		// activate "engine" only if a path was found
		if (pathId != 0) {
			pathManager->UpdatePath(owner, pathId);

			owner->isMoving = true;
			owner->script->StartMoving();
		}
	}

	nextObstacleAvoidanceUpdate = gs->frameNum;
}

void CGroundMoveType::StopEngine() {
	if (pathId != 0) {
		pathManager->DeletePath(pathId);
		pathId = 0;

		if (!atGoal) {
			currWayPoint = Here();
		}

		// Stop animation.
		owner->script->StopMoving();

		LOG_L(L_DEBUG, "StopEngine: engine stopped for unit %i", owner->id);
	}

	owner->isMoving = false;
	wantedSpeed = 0.0f;
}



/* Called when the unit arrives at its goal. */
void CGroundMoveType::Arrived()
{
	// can only "arrive" if the engine is active
	if (progressState == Active) {
		// we have reached our goal
		StopEngine();

		#if (PLAY_SOUNDS == 1)
		if (owner->team == gu->myTeam) {
			Channels::General.PlayRandomSample(owner->unitDef->sounds.arrived, owner);
		}
		#endif

		// and the action is done
		progressState = Done;

		// FIXME:
		//   CAI sometimes does not update its queue correctly
		//   (probably whenever we are called "before" the CAI
		//   is ready to accept that a unit is at its goal-pos)
		owner->commandAI->GiveCommand(Command(CMD_WAIT));
		owner->commandAI->GiveCommand(Command(CMD_WAIT));

		if (!owner->commandAI->HasMoreMoveCommands()) {
			// update the position-parameter of our queue's front CMD_MOVE
			// this is needed in case we Arrive()'ed non-directly (through
			// colliding with another unit that happened to share our goal)
			static_cast<CMobileCAI*>(owner->commandAI)->SetFrontMoveCommandPos(owner->pos);
		}

		LOG_L(L_DEBUG, "Arrived: unit %i arrived", owner->id);
	}
}

/*
Makes the unit fail this action.
No more trials will be done before a new goal is given.
*/
void CGroundMoveType::Fail()
{
	LOG_L(L_DEBUG, "Fail: unit %i failed", owner->id);

	StopEngine();

	// failure of finding a path means that
	// this action has failed to reach its goal.
	progressState = Failed;

	eventHandler.UnitMoveFailed(owner);
	eoh->UnitMoveFailed(*owner);
}




void CGroundMoveType::HandleObjectCollisions()
{
	static const float3 sepDirMask = float3(1.0f, 0.0f, 1.0f);

	CUnit* collider = owner;

	// handle collisions for even-numbered objects on even-numbered frames and vv.
	// (temporal resolution is still high enough to not compromise accuracy much?)
	// if ((collider->id & 1) == (gs->frameNum & 1)) {
	{
		const UnitDef* colliderUD = collider->unitDef;
		const MoveDef* colliderMD = collider->moveDef;

		// NOTE:
		//   use the collider's MoveDef footprint as radius since it is
		//   always mobile (its UnitDef footprint size may be different)
		//
		//   0.75 * math::sqrt(2) ~= 1, so radius is always that of a circle
		//   _maximally bounded_ by the footprint rather than a circle
		//   _minimally bounding_ the footprint (assuming square shape)
		//
		const float colliderSpeed = collider->speed.Length();
		const float colliderRadius = FOOTPRINT_RADIUS(colliderMD->xsize, colliderMD->zsize, 0.75f);

		HandleUnitCollisions(collider, colliderSpeed, colliderRadius, sepDirMask, colliderUD, colliderMD);
		HandleFeatureCollisions(collider, colliderSpeed, colliderRadius, sepDirMask, colliderUD, colliderMD);
		HandleStaticObjectCollision(collider, collider, colliderMD, colliderRadius, 0.0f, ZeroVector, true, false, true);
	}

	collider->Block();
}

void CGroundMoveType::HandleStaticObjectCollision(
	CUnit* collider,
	CSolidObject* collidee,
	const MoveDef* colliderMD,
	const float colliderRadius,
	const float collideeRadius,
	const float3& separationVector,
	bool canRequestPath,
	bool checkYardMap,
	bool checkTerrain
) {
	if (checkTerrain && (!collider->isMoving || collider->inAir))
		return;

	// for factories, check if collidee's position is behind us (which means we are likely exiting)
	//
	// NOTE:
	//   allow units to move _through_ idle open factories by extending the collidee's footprint such
	//   that insideYardMap is true in a larger area (otherwise pathfinder and coldet would disagree)
	// 
	const int xext = ((collidee->xsize >> 1) + std::max(1, colliderMD->xsizeh));
	const int zext = ((collidee->zsize >> 1) + std::max(1, colliderMD->zsizeh));

	const bool exitingYardMap =
		((collider->frontdir.dot(separationVector) > 0.0f) &&
		 (collider->   speed.dot(separationVector) > 0.0f));
	const bool insideYardMap =
		(collider->pos.x >= (collidee->pos.x - xext * SQUARE_SIZE)) &&
		(collider->pos.x <= (collidee->pos.x + xext * SQUARE_SIZE)) &&
		(collider->pos.z >= (collidee->pos.z - zext * SQUARE_SIZE)) &&
		(collider->pos.z <= (collidee->pos.z + zext * SQUARE_SIZE));

	bool wantRequestPath = false;

	if ((checkYardMap && insideYardMap) || checkTerrain) {
		const int xmid = (collider->pos.x + collider->speed.x) / SQUARE_SIZE;
		const int zmid = (collider->pos.z + collider->speed.z) / SQUARE_SIZE;

		const int xmin = std::min(-1, -colliderMD->xsizeh), xmax = std::max(1, colliderMD->xsizeh);
		const int zmin = std::min(-1, -colliderMD->xsizeh), zmax = std::max(1, colliderMD->zsizeh);

		float3 strafeVec;
		float3 bounceVec;
		float3 sqCenterPosition;

		float sqPenDistanceSum = 0.0f;
		float sqPenDistanceCtr = 0.0f;

		if (DEBUG_DRAWING_ENABLED) {
			geometricObjects->AddLine(collider->pos + (UpVector * 25.0f), collider->pos + (UpVector * 100.0f), 3, 1, 4);
		}

		// check for blocked squares inside collider's MoveDef footprint zone
		// interpret each square as a "collidee" and sum up separation vectors
		//
		// NOTE:
		//   assumes the collider's footprint is still always axis-aligned
		// NOTE:
		//   the pathfinders only care about the CENTER square for terrain!
		//   this means paths can come closer to impassable terrain than is
		//   allowed by collision detection (more probable if edges between
		//   passable and impassable areas are hard instead of gradients or
		//   if a unit is not affected by slopes) --> can be solved through
		//   smoothing the cost-function, eg. blurring heightmap before PFS
		//   sees it
		for (int z = zmin; z <= zmax; z++) {
			for (int x = xmin; x <= xmax; x++) {
				const int xabs = xmid + x;
				const int zabs = zmid + z;

				if (checkTerrain) {
					if (CMoveMath::GetPosSpeedMod(*colliderMD, xabs, zabs) > 0.01f)
						continue;
				} else {
					if ((CMoveMath::SquareIsBlocked(*colliderMD, xabs, zabs, collider) & CMoveMath::BLOCK_STRUCTURE) == 0)
						continue;
				}

				const float3 squarePos = float3(xabs * SQUARE_SIZE + (SQUARE_SIZE >> 1), 0.0f, zabs * SQUARE_SIZE + (SQUARE_SIZE >> 1));
				const float3 squareVec = collider->pos - squarePos;

				if (squareVec.dot(collider->speed) > 0.0f)
					continue;	

				// radius of a square is the RHS magic constant (sqrt(2*(SQUARE_SIZE>>1)*(SQUARE_SIZE>>1)))
				const float  sqColRadiusSum = colliderRadius + 5.656854249492381f;
				const float   sqSepDistance = squareVec.Length2D() + 0.1f;
				const float   sqPenDistance = std::min(sqSepDistance - sqColRadiusSum, 0.0f);
				// const float  sqColSlideSign = ((squarePos.dot(collider->rightdir) - (collider->pos).dot(collider->rightdir)) < 0.0f) * 2.0f - 1.0f;

				// this tends to cancel out too much on average
				// strafeVec += (collider->rightdir * sqColSlideSign);
				bounceVec += (squareVec / sqSepDistance);

				sqPenDistanceSum += sqPenDistance;
				sqPenDistanceCtr += 1.0f;
				sqCenterPosition += squarePos;
			}
		}

		if (sqPenDistanceCtr > 0.0f) {
			sqCenterPosition /= sqPenDistanceCtr;
			sqPenDistanceSum /= sqPenDistanceCtr;

			const float strafeSign = ((sqCenterPosition.dot(collider->rightdir) - (collider->pos).dot(collider->rightdir)) < 0.0f) * 2.0f - 1.0f;
			const float strafeScale = std::min(currentSpeed, std::max(0.0f, -sqPenDistanceSum * 0.5f));
			const float bounceScale =                        std::max(0.0f, -sqPenDistanceSum        );

			strafeVec = collider->rightdir * strafeSign;
			strafeVec.y = 0.0f; strafeVec.SafeNormalize();
			bounceVec.y = 0.0f; bounceVec.SafeNormalize();

			if (colliderMD->TestMoveSquare(collider, collider->pos + strafeVec * strafeScale)) { collider->Move3D(strafeVec * strafeScale, true); }
			if (colliderMD->TestMoveSquare(collider, collider->pos + bounceVec * bounceScale)) { collider->Move3D(bounceVec * bounceScale, true); }
		}

		wantRequestPath = ((strafeVec + bounceVec) != ZeroVector);
	} else {
		const float  colRadiusSum = colliderRadius + collideeRadius;
		const float   sepDistance = separationVector.Length() + 0.1f;
		const float   penDistance = std::min(sepDistance - colRadiusSum, 0.0f);
		const float  colSlideSign = ((collidee->pos.dot(collider->rightdir) - collider->pos.dot(collider->rightdir)) <= 0.0f) * 2.0f - 1.0f;

		const float strafeScale = std::min(currentSpeed, std::max(0.0f, -penDistance * 0.5f)) * (1 -                exitingYardMap);
		const float bounceScale =                        std::max(0.0f, -penDistance        ) * (1 - checkYardMap * exitingYardMap);

		// when exiting a lab, insideYardMap goes from true to false
		// before we stop colliding and we get a slight unneeded push
		// --> compensate for this
		collider->Move3D((collider->rightdir * colSlideSign) * strafeScale, true);
		collider->Move3D((separationVector / sepDistance) *  bounceScale, true);

		wantRequestPath = (penDistance < 0.0f);
	}

	// NOTE:
	//   we want an initial speed of 0 to avoid ramming into the
	//   obstacle again right after the push, but if our leading
	//   command is not a CMD_MOVE then SetMaxSpeed will not get
	//   called later and 0 will immobilize us
	//
	if (canRequestPath && wantRequestPath) {
		if (UNIT_HAS_MOVE_CMD(owner)) {
			StartMoving(goalPos, goalRadius, 0.0f);
		} else {
			StartMoving(goalPos, goalRadius);
		}
	}
}



void CGroundMoveType::HandleUnitCollisions(
	CUnit* collider,
	const float colliderSpeed,
	const float colliderRadius,
	const float3& sepDirMask,
	const UnitDef* colliderUD,
	const MoveDef* colliderMD
) {
	const float searchRadius = std::max(colliderSpeed, 1.0f) * (colliderRadius * 1.0f);

	const std::vector<CUnit*>& nearUnits = quadField->GetUnitsExact(collider->pos, searchRadius);
	      std::vector<CUnit*>::const_iterator uit;

	// NOTE: probably too large for most units (eg. causes tree falling animations to be skipped)
	const int dirSign = int(!reversing) * 2 - 1;
	const float3 crushImpulse = collider->speed * collider->mass * dirSign;

	for (uit = nearUnits.begin(); uit != nearUnits.end(); ++uit) {
		CUnit* collidee = const_cast<CUnit*>(*uit);

		const UnitDef* collideeUD = collidee->unitDef;
		const MoveDef* collideeMD = collidee->moveDef;

		const bool colliderMobile = (colliderMD != NULL); // always true
		const bool collideeMobile = (collideeMD != NULL); // maybe true

		// use the collidee's MoveDef footprint as radius if it is mobile
		// use the collidee's Unit (not UnitDef) footprint as radius otherwise
		const float collideeSpeed = collidee->speed.Length();
		const float collideeRadius = collideeMobile?
			FOOTPRINT_RADIUS(collideeMD->xsize, collideeMD->zsize, 0.75f):
			FOOTPRINT_RADIUS(collidee  ->xsize, collidee  ->zsize, 0.75f);

		const float3 separationVector   = collider->pos - collidee->pos;
		const float separationMinDistSq = (colliderRadius + collideeRadius) * (colliderRadius + collideeRadius);

		if ((separationVector.SqLength() - separationMinDistSq) > 0.01f)
			continue;

		if (collidee == collider) continue;
		if (collidee->moveType->IsSkidding()) continue;
		if (collidee->moveType->IsFlying()) continue;

		// disable collisions between collider and collidee
		// if collidee is currently inside any transporter,
		// or if collider is being transported by collidee
		if (collider->GetTransporter() == collidee) continue;
		if (collidee->GetTransporter() != NULL) continue;
		// also disable collisions if either party currently
		// has an order to load units (TODO: do we want this
		// for unloading as well?)
		if (collider->loadingTransportId == collidee->id) continue;
		if (collidee->loadingTransportId == collider->id) continue;

		// NOTE:
		//    we exclude aircraft (which have NULL moveDef's) landed
		//    on the ground, since they would just stack when pushed
		bool pushCollider = colliderMobile;
		bool pushCollidee = collideeMobile;
		bool crushCollidee = false;

		const bool alliedCollision =
			teamHandler->Ally(collider->allyteam, collidee->allyteam) &&
			teamHandler->Ally(collidee->allyteam, collider->allyteam);
		const bool collideeYields = (collider->isMoving && !collidee->isMoving);
		const bool ignoreCollidee = (collideeYields && alliedCollision);

		// FIXME:
		//   allowPushingEnemyUnits is (now) useless because alliances are bi-directional
		//   ie. if !alliedCollision, pushCollider and pushCollidee BOTH become false and
		//   the collision is treated normally --> not what we want here, but the desired
		//   behavior (making each party stop and block the other) has many corner-cases
		//   this also happens when both parties are pushResistant --> make each respond
		//   to the other as a static obstacle so the tags still have some effect
		pushCollider &= (alliedCollision || modInfo.allowPushingEnemyUnits || !collider->blockEnemyPushing);
		pushCollidee &= (alliedCollision || modInfo.allowPushingEnemyUnits || !collidee->blockEnemyPushing);
		pushCollider &= (!collider->beingBuilt && !collider->usingScriptMoveType && !colliderUD->pushResistant);
		pushCollidee &= (!collidee->beingBuilt && !collidee->usingScriptMoveType && !collideeUD->pushResistant);

		crushCollidee |= (!alliedCollision || modInfo.allowCrushingAlliedUnits);
		crushCollidee &= ((colliderSpeed * collider->mass) > (collideeSpeed * collidee->mass));

		// don't push/crush either party if the collidee does not block the collider (or vv.)
		if (colliderMobile && CMoveMath::IsNonBlocking(*colliderMD, collidee, collider))
			continue;
		if (collideeMobile && CMoveMath::IsNonBlocking(*collideeMD, collider, collidee))
			continue;

		if (crushCollidee && !CMoveMath::CrushResistant(*colliderMD, collidee))
			collidee->Kill(crushImpulse, true);

		if (pathController->IgnoreCollision(collider, collidee))
			continue;

		eventHandler.UnitUnitCollision(collider, collidee);

		if ((!collideeMobile && !collideeUD->IsAirUnit()) || (!pushCollider && !pushCollidee)) {
			// building (always axis-aligned, possibly has a yardmap)
			// or semi-static collidee that should be handled as such
			HandleStaticObjectCollision(
				collider,
				collidee,
				colliderMD,
				colliderRadius,
				collideeRadius,
				separationVector,
				(gs->frameNum > pathRequestDelay),
				collideeUD->IsFactoryUnit(),
				false);

			continue;
		}

		if ((collider->moveType->goalPos - collidee->moveType->goalPos).SqLength2D() < 2.0f) {
			// if collidee shares our goal position and is no longer
			// moving along its path, trigger Arrived() to kill long
			// pushing contests
			// check the progress-states so collisions with units which
			// failed to reach goalPos for whatever reason do not count
			// (or those that still have orders)
			if (collider->isMoving && collider->moveType->progressState == AMoveType::Active) {
				if (!collidee->isMoving && collidee->moveType->progressState == AMoveType::Done) {
					if (UNIT_CMD_QUE_SIZE(collidee) == 0) {
						atEndOfPath = true; atGoal = true;
					}
				}
			}
		}

		const float colliderRelRadius = colliderRadius / (colliderRadius + collideeRadius);
		const float collideeRelRadius = collideeRadius / (colliderRadius + collideeRadius);
		const float collisionRadiusSum = modInfo.allowUnitCollisionOverlap?
			(colliderRadius * colliderRelRadius + collideeRadius * collideeRelRadius):
			(colliderRadius                     + collideeRadius                    );

		const float  sepDistance = separationVector.Length() + 0.1f;
		const float  penDistance = std::max(collisionRadiusSum - sepDistance, 1.0f);
		const float  sepResponse = std::min(SQUARE_SIZE * 2.0f, penDistance * 0.5f);

		const float3 sepDirection   = (separationVector / sepDistance);
		const float3 colResponseVec = sepDirection * sepDirMask * sepResponse;

		const float
			m1 = collider->mass,
			m2 = collidee->mass,
			v1 = std::max(1.0f, colliderSpeed),
			v2 = std::max(1.0f, collideeSpeed),
			c1 = 1.0f + (1.0f - math::fabs(collider->frontdir.dot(-sepDirection))) * 5.0f,
			c2 = 1.0f + (1.0f - math::fabs(collidee->frontdir.dot( sepDirection))) * 5.0f,
			s1 = m1 * v1 * c1,
			s2 = m2 * v2 * c2,
 			r1 = s1 / (s1 + s2 + 1.0f),
 			r2 = s2 / (s1 + s2 + 1.0f);

		// far from a realistic treatment, but works
		const float colliderMassScale = Clamp(1.0f - r1, 0.01f, 0.99f) * (modInfo.allowUnitCollisionOverlap? (1.0f / colliderRelRadius): 1.0f);
		const float collideeMassScale = Clamp(1.0f - r2, 0.01f, 0.99f) * (modInfo.allowUnitCollisionOverlap? (1.0f / collideeRelRadius): 1.0f);

		// try to prevent both parties from being pushed onto non-traversable
		// squares (without resetting their position which stops them dead in
		// their tracks and undoes previous legitimate pushes made this frame)
		//
		// if pushCollider and pushCollidee are both false (eg. if each party
		// is pushResistant), treat the collision as regular and push both to
		// avoid deadlocks
		#define SIGN(v) ((int(v >= 0.0f) * 2) - 1)
		const int colliderSlideSign = SIGN( separationVector.dot(collider->rightdir));
		const int collideeSlideSign = SIGN(-separationVector.dot(collidee->rightdir));
		#undef SIGN

		const float3 colliderPushVec  =  colResponseVec * colliderMassScale * int(!ignoreCollidee);
		const float3 collideePushVec  = -colResponseVec * collideeMassScale;
		const float3 colliderSlideVec = collider->rightdir * colliderSlideSign * (1.0f / penDistance) * r2;
		const float3 collideeSlideVec = collidee->rightdir * collideeSlideSign * (1.0f / penDistance) * r1;

		if ((pushCollider || !pushCollidee) && colliderMobile) {
			if (colliderMD->TestMoveSquare(collider, collider->pos + colliderPushVec)) {
				collider->Move3D(collider->pos + colliderPushVec, false);
			}
			// also push collider laterally
			if (colliderMD->TestMoveSquare(collider, collider->pos + colliderSlideVec)) {
				collider->Move3D(collider->pos + colliderSlideVec, false);
			}
		}

		if ((pushCollidee || !pushCollider) && collideeMobile) {
			if (collideeMD->TestMoveSquare(collidee, collidee->pos + collideePushVec)) {
				collidee->Move3D(collidee->pos + collideePushVec, false);
			}
			// also push collidee laterally
			if (collideeMD->TestMoveSquare(collidee, collidee->pos + collideeSlideVec)) {
				collidee->Move3D(collidee->pos + collideeSlideVec, false);
			}
		}
	}
}

void CGroundMoveType::HandleFeatureCollisions(
	CUnit* collider,
	const float colliderSpeed,
	const float colliderRadius,
	const float3& sepDirMask,
	const UnitDef* colliderUD,
	const MoveDef* colliderMD
) {
	const float searchRadius = std::max(colliderSpeed, 1.0f) * (colliderRadius * 1.0f);

	const std::vector<CFeature*>& nearFeatures = quadField->GetFeaturesExact(collider->pos, searchRadius);
	      std::vector<CFeature*>::const_iterator fit;

	const int dirSign = int(!reversing) * 2 - 1;
	const float3 crushImpulse = collider->speed * collider->mass * dirSign;

	for (fit = nearFeatures.begin(); fit != nearFeatures.end(); ++fit) {
		CFeature* collidee = const_cast<CFeature*>(*fit);
		// const FeatureDef* collideeFD = collidee->def;

		// use the collidee's Feature (not FeatureDef) footprint as radius
		// const float collideeRadius = FOOTPRINT_RADIUS(collideeFD->xsize, collideeFD->zsize, 0.75f);
		const float collideeRadius = FOOTPRINT_RADIUS(collidee->xsize, collidee->zsize, 0.75f);
		const float collisionRadiusSum = colliderRadius + collideeRadius;

		const float3 separationVector   = collider->pos - collidee->pos;
		const float separationMinDistSq = collisionRadiusSum * collisionRadiusSum;

		if ((separationVector.SqLength() - separationMinDistSq) > 0.01f)
			continue;

		if (CMoveMath::IsNonBlocking(*colliderMD, collidee, collider))
			continue;
		if (!CMoveMath::CrushResistant(*colliderMD, collidee))
			collidee->Kill(crushImpulse, true);

		if (pathController->IgnoreCollision(collider, collidee))
			continue;

		eventHandler.UnitFeatureCollision(collider, collidee);

		if (collidee->isMoving) {
			HandleStaticObjectCollision(
				collider,
				collidee,
				colliderMD,
				colliderRadius,
				collideeRadius,
				separationVector,
				(gs->frameNum > pathRequestDelay),
				false,
				false);

			continue;
		}

		const float  sepDistance    = separationVector.Length() + 0.1f;
		const float  penDistance    = std::max(collisionRadiusSum - sepDistance, 1.0f);
		const float  sepResponse    = std::min(SQUARE_SIZE * 2.0f, penDistance * 0.5f);

		const float3 sepDirection   = (separationVector / sepDistance);
		const float3 colResponseVec = sepDirection * sepDirMask * sepResponse;

		// multiply the collider's mass by a large constant (so that heavy
		// features do not bounce light units away like jittering pinballs;
		// collideeMassScale ~= 0.01 suppresses large responses)
		const float
			m1 = collider->mass,
			m2 = collidee->mass * 10000.0f,
			v1 = std::max(1.0f, colliderSpeed),
			v2 = 1.0f,
			c1 = (1.0f - math::fabs( collider->frontdir.dot(-sepDirection))) * 5.0f,
			c2 = (1.0f - math::fabs(-collider->frontdir.dot( sepDirection))) * 5.0f,
			s1 = m1 * v1 * c1,
			s2 = m2 * v2 * c2,
 			r1 = s1 / (s1 + s2 + 1.0f),
 			r2 = s2 / (s1 + s2 + 1.0f);

		const float colliderMassScale = Clamp(1.0f - r1, 0.01f, 0.99f);
		const float collideeMassScale = Clamp(1.0f - r2, 0.01f, 0.99f);

		quadField->RemoveFeature(collidee);
		collider->Move3D( colResponseVec * colliderMassScale, true);
		collidee->Move3D(-colResponseVec * collideeMassScale, true);
		quadField->AddFeature(collidee);
	}
}




void CGroundMoveType::CreateLineTable()
{
	// for every <xt, zt> pair, computes a set of regularly spaced
	// grid sample-points (int2 offsets) along the line from <start>
	// to <to>; <to> ranges from [x=-4.5, z=-4.5] to [x=+5.5, z=+5.5]
	//
	// TestNewTerrainSquare() and CanGetNextWayPoint() check whether
	// squares are blocked at these offsets to get a fast estimate of
	// terrain passability
	for (int yt = 0; yt < LINETABLE_SIZE; ++yt) {
		for (int xt = 0; xt < LINETABLE_SIZE; ++xt) {
			// center-point of grid-center cell
			const float3 start(0.5f, 0.0f, 0.5f);
			// center-point of target cell
			const float3 to((xt - (LINETABLE_SIZE / 2)) + 0.5f, 0.0f, (yt - (LINETABLE_SIZE / 2)) + 0.5f);

			const float dx = to.x - start.x;
			const float dz = to.z - start.z;
			float xp = start.x;
			float zp = start.z;

			if (math::floor(start.x) == math::floor(to.x)) {
				if (dz > 0.0f) {
					for (int a = 1; a <= math::floor(to.z); ++a)
						lineTable[yt][xt].push_back(int2(0, a));
				} else {
					for (int a = -1; a >= math::floor(to.z); --a)
						lineTable[yt][xt].push_back(int2(0, a));
				}
			} else if (math::floor(start.z) == math::floor(to.z)) {
				if (dx > 0.0f) {
					for (int a = 1; a <= math::floor(to.x); ++a)
						lineTable[yt][xt].push_back(int2(a, 0));
				} else {
					for (int a = -1; a >= math::floor(to.x); --a)
						lineTable[yt][xt].push_back(int2(a, 0));
				}
			} else {
				float xn, zn;
				bool keepgoing = true;

				while (keepgoing) {
					if (dx > 0.0f) {
						xn = (math::floor(xp) + 1.0f - xp) / dx;
					} else {
						xn = (math::floor(xp)        - xp) / dx;
					}
					if (dz > 0.0f) {
						zn = (math::floor(zp) + 1.0f - zp) / dz;
					} else {
						zn = (math::floor(zp)        - zp) / dz;
					}

					if (xn < zn) {
						xp += (xn + 0.0001f) * dx;
						zp += (xn + 0.0001f) * dz;
					} else {
						xp += (zn + 0.0001f) * dx;
						zp += (zn + 0.0001f) * dz;
					}

					keepgoing =
						math::fabs(xp - start.x) <= math::fabs(to.x - start.x) &&
						math::fabs(zp - start.z) <= math::fabs(to.z - start.z);
					int2 pt(int(math::floor(xp)), int(math::floor(zp)));

					static const int MIN_IDX = -int(LINETABLE_SIZE / 2);
					static const int MAX_IDX = -MIN_IDX;

					if (MIN_IDX > pt.x || pt.x > MAX_IDX) continue;
					if (MIN_IDX > pt.y || pt.y > MAX_IDX) continue;

					lineTable[yt][xt].push_back(pt);
				}
			}
		}
	}
}

void CGroundMoveType::DeleteLineTable()
{
	for (int yt = 0; yt < LINETABLE_SIZE; ++yt) {
		for (int xt = 0; xt < LINETABLE_SIZE; ++xt) {
			lineTable[yt][xt].clear();
		}
	}
}



void CGroundMoveType::LeaveTransport()
{
	oldPos = owner->pos + UpVector * 0.001f;
}



void CGroundMoveType::KeepPointingTo(float3 pos, float distance, bool aggressive) {
	mainHeadingPos = pos;
	useMainHeading = aggressive;

	if (!useMainHeading) return;
	if (owner->weapons.empty()) return;

	CWeapon* frontWeapon = owner->weapons.front();

	if (!frontWeapon->weaponDef->waterweapon) {
		mainHeadingPos.y = std::max(mainHeadingPos.y, 0.0f);
	}

	float3 dir1 = frontWeapon->mainDir;
	float3 dir2 = mainHeadingPos - owner->pos;

	// in this case aligning is impossible
	if (dir1 == UpVector)
		return;

	dir1.y = 0.0f; dir1.SafeNormalize();
	dir2.y = 0.0f; dir2.SafeNormalize();

	if (dir2 == ZeroVector)
		return;

	const short heading =
		GetHeadingFromVector(dir2.x, dir2.z) -
		GetHeadingFromVector(dir1.x, dir1.z);

	if (owner->heading == heading)
		return;

	if (!frontWeapon->TryTarget(mainHeadingPos, true, 0)) {
		progressState = Active;
	}
}

void CGroundMoveType::KeepPointingTo(CUnit* unit, float distance, bool aggressive) {
	//! wrapper
	KeepPointingTo(unit->pos, distance, aggressive);
}

/**
* @brief Orients owner so that weapon[0]'s arc includes mainHeadingPos
*/
void CGroundMoveType::SetMainHeading() {
	if (!useMainHeading) return;
	if (owner->weapons.empty()) return;

	CWeapon* frontWeapon = owner->weapons.front();

	float3 dir1 = frontWeapon->mainDir;
	float3 dir2 = mainHeadingPos - owner->pos;

	dir1.y = 0.0f;
	dir1.Normalize();
	dir2.y = 0.0f;
	dir2.SafeNormalize();

	ASSERT_SYNCED(dir1);
	ASSERT_SYNCED(dir2);

	if (dir2 == ZeroVector)
		return;

	short newHeading =
		GetHeadingFromVector(dir2.x, dir2.z) -
		GetHeadingFromVector(dir1.x, dir1.z);

	ASSERT_SYNCED(newHeading);

	if (progressState == Active) {
		if (owner->heading == newHeading) {
			// stop turning
			owner->script->StopMoving();
			progressState = Done;
		} else {
			ChangeHeading(newHeading);
		}
	} else {
		if (owner->heading != newHeading) {
			if (!frontWeapon->TryTarget(mainHeadingPos, true, 0)) {
				// start moving
				progressState = Active;
				owner->script->StartMoving();
				ChangeHeading(newHeading);
			}
		}
	}
}

bool CGroundMoveType::OnSlope(float minSlideTolerance) {
	const UnitDef* ud = owner->unitDef;
	const MoveDef* md = owner->moveDef;
	const float3& pos = owner->pos;

	if (ud->slideTolerance < minSlideTolerance) { return false; }
	if (owner->unitDef->floatOnWater && owner->inWater) { return false; }
	if (!pos.IsInBounds()) { return false; }

	// if minSlideTolerance is zero, do not multiply maxSlope by ud->slideTolerance
	// (otherwise the unit could stop on an invalid path location, and be teleported
	// back)
	const float gSlope = ground->GetSlope(pos.x, pos.z);
	const float uSlope = md->maxSlope * ((minSlideTolerance <= 0.0f)? 1.0f: ud->slideTolerance);

	return (gSlope > uSlope);
}



const float3& CGroundMoveType::GetGroundNormal(const float3& p) const
{
	if (owner->inWater && owner->unitDef->floatOnWater) {
		// return (ground->GetNormalAboveWater(p));
		return UpVector;
	}

	return (ground->GetNormal(p.x, p.z));
}

float CGroundMoveType::GetGroundHeight(const float3& p) const
{
	float h = 0.0f;

	if (owner->unitDef->floatOnWater) {
		// in [0, maxHeight]
		h += ground->GetHeightAboveWater(p.x, p.z);
		h -= (owner->unitDef->waterline * (h <= 0.0f));
	} else {
		// in [minHeight, maxHeight]
		h = ground->GetHeightReal(p.x, p.z);
	}

	return h;
}

void CGroundMoveType::AdjustPosToWaterLine()
{
	if (owner->falling)
		return;
	if (flying)
		return;

	if (modInfo.allowGroundUnitGravity) {
		if (owner->unitDef->floatOnWater) {
			owner->Move1D(std::max(ground->GetHeightReal(owner->pos.x, owner->pos.z), -owner->unitDef->waterline), 1, false);
		} else {
			owner->Move1D(std::max(ground->GetHeightReal(owner->pos.x, owner->pos.z),               owner->pos.y), 1, false);
		}
	} else {
		owner->Move1D(GetGroundHeight(owner->pos), 1, false);
	}
}

bool CGroundMoveType::UpdateDirectControl()
{
	const CPlayer* myPlayer = gu->GetMyPlayer();
	const FPSUnitController& selfCon = myPlayer->fpsController;
	const FPSUnitController& unitCon = owner->fpsControlPlayer->fpsController;
	const bool wantReverse = (unitCon.back && !unitCon.forward);
	float turnSign = 0.0f;

	currWayPoint.x = owner->pos.x + owner->frontdir.x * (wantReverse)? -100.0f: 100.0f;
	currWayPoint.z = owner->pos.z + owner->frontdir.z * (wantReverse)? -100.0f: 100.0f;
	currWayPoint.ClampInBounds();

	if (unitCon.forward) {
		ChangeSpeed(maxSpeed, wantReverse, true);

		owner->isMoving = true;
		owner->script->StartMoving();
	} else if (unitCon.back) {
		ChangeSpeed(maxReverseSpeed, wantReverse, true);

		owner->isMoving = true;
		owner->script->StartMoving();
	} else {
		// not moving forward or backward, stop
		ChangeSpeed(0.0f, false, true);

		owner->isMoving = false;
		owner->script->StopMoving();
	}

	if (unitCon.left ) { ChangeHeading(owner->heading + turnRate); turnSign =  1.0f; }
	if (unitCon.right) { ChangeHeading(owner->heading - turnRate); turnSign = -1.0f; }

	if (selfCon.GetControllee() == owner) {
		camera->rot.y += (turnRate * turnSign * TAANG2RAD);
	}

	return wantReverse;
}

float3 CGroundMoveType::GetNewSpeedVector(const float hAcc, const float vAcc) const
{
	float3 speedVector;

	if (modInfo.allowGroundUnitGravity) {
		const bool applyGravity = ((owner->pos.y + owner->speed.y) >= GetGroundHeight(owner->pos + owner->speed));

		// NOTE:
		//   the drag terms ensure speed-vector always
		//   decays if wantedSpeed and deltaSpeed are 0
		const float dragCoeff = (0.9999f * owner->inAir) + (0.99f * (1 - owner->inAir));
		const float slipCoeff = (0.9999f * owner->inAir) + (0.95f * (1 - owner->inAir));

		// use terrain-tangent vector because it does not
		// depend on UnitDef::upright (unlike o->frontdir)
		const float3& gndNormVec = GetGroundNormal(owner->pos);
		const float3  gndTangVec = gndNormVec.cross(owner->rightdir);
		const float3   flatSpeed = float3(owner->speed.x, 0.0f, owner->speed.z);

		// never drop below terrain
		owner->speed.y =
			(gndTangVec.y * owner->speed.dot(gndTangVec) * (1 - applyGravity)) +
			(  UpVector.y * owner->speed.dot(  UpVector) * (    applyGravity));

		if (owner->moveDef->moveFamily != MoveDef::Hover || !modInfo.allowHoverUnitStrafing) {
			const float3 accelVec = (gndTangVec * hAcc) + (UpVector * vAcc);
			const float3 speedVec = owner->speed + accelVec;

			speedVector += (flatFrontDir * speedVec.dot(flatFrontDir)) * dragCoeff;
			speedVector += (    UpVector * speedVec.dot(    UpVector));
		} else {
			// TODO: also apply to non-hovercraft on low-gravity maps?
			speedVector += (               gndTangVec * (  std::max(0.0f,   owner->speed.dot(gndTangVec) + hAcc * 1.0f))) * dragCoeff;
			speedVector += (   flatSpeed - gndTangVec * (/*std::max(0.0f,*/ owner->speed.dot(gndTangVec) - hAcc * 0.0f )) * slipCoeff;
			speedVector += UpVector * (owner->speed + UpVector * vAcc).dot(UpVector);
		}
	} else {
		// LuaSyncedCtrl::SetUnitVelocity directly assigns
		// to owner->speed which gets overridden below, so
		// need to calculate hSpeedScale from it (not from
		// currentSpeed) directly
		const int    speedSign  = int(!reversing) * 2 - 1;
		const float  speedScale = owner->speed.Length() * speedSign + hAcc;

		speedVector = owner->frontdir * speedScale;
	}

	return speedVector;
}

void CGroundMoveType::UpdateOwnerPos(bool wantReverse)
{
	const float3 speedVector = GetNewSpeedVector(deltaSpeed, mapInfo->map.gravity);

	// if being built, the nanoframe might not be exactly on
	// the ground and would jitter from gravity acting on it
	// --> nanoframes can not move anyway, just return early
	// (units that become reverse-built will stop instantly)
	if (owner->beingBuilt)
		return;

	if (speedVector != ZeroVector) {
		// use the simplest possible Euler integration
		owner->Move3D(owner->speed = speedVector, true);

		// NOTE:
		//   does not check for structure blockage, coldet handles that
		//   entering of impassable terrain is *also* handled by coldet
		if (!owner->moveDef->TestMoveSquare(owner, owner->pos, ZeroVector, true, false, true)) {
			owner->Move3D(owner->pos - speedVector, false);
		}
	}

	reversing    = (speedVector.dot(flatFrontDir) < 0.0f);
	currentSpeed = math::fabs(speedVector.dot(flatFrontDir));
	deltaSpeed   = 0.0f;

	assert(math::fabs(currentSpeed) < 1e6f);
}

bool CGroundMoveType::WantReverse(const float3& waypointDir2D) const
{
	if (!canReverse)
		return false;

	// these values are normally non-0, but LuaMoveCtrl
	// can override them and we do not want any div0's
	if (maxReverseSpeed <= 0.0f) return false;
	if (maxSpeed <= 0.0f) return true;

	if (accRate <= 0.0f) return false;
	if (decRate <= 0.0f) return false;
	if (turnRate <= 0.0f) return false;

	const float3 waypointDif  = float3(goalPos.x - owner->pos.x, 0.0f, goalPos.z - owner->pos.z); // use final WP for ETA
	const float waypointDist  = waypointDif.Length();                                             // in elmos
	const float waypointFETA  = (waypointDist / maxSpeed);                                        // in frames (simplistic)
	const float waypointRETA  = (waypointDist / maxReverseSpeed);                                 // in frames (simplistic)
	const float waypointDirDP = waypointDir2D.dot(owner->frontdir);
	const float waypointAngle = Clamp(waypointDirDP, -1.0f, 1.0f);                                // prevent NaN's
	const float turnAngleDeg  = math::acosf(waypointAngle) * (180.0f / PI);                       // in degrees
	const float turnAngleSpr  = (turnAngleDeg / 360.0f) * SPRING_CIRCLE_DIVS;                     // in "headings"
	const float revAngleSpr   = SHORTINT_MAXVALUE - turnAngleSpr;                                 // 180 deg - angle

	// units start accelerating before finishing the turn, so subtract something
	const float turnTimeMod   = 5.0f;
	const float turnAngleTime = std::max(0.0f, (turnAngleSpr / turnRate) - turnTimeMod); // in frames
	const float revAngleTime  = std::max(0.0f, (revAngleSpr  / turnRate) - turnTimeMod);

	const float apxSpeedAfterTurn  = std::max(0.f, currentSpeed - 0.125f * (turnAngleTime * decRate));
	const float apxRevSpdAfterTurn = std::max(0.f, currentSpeed - 0.125f * (revAngleTime  * decRate));
	const float decTime       = ( reversing * apxSpeedAfterTurn)  / decRate;
	const float revDecTime    = (!reversing * apxRevSpdAfterTurn) / decRate;
	const float accTime       = (maxSpeed        - !reversing * apxSpeedAfterTurn)  / accRate;
	const float revAccTime    = (maxReverseSpeed -  reversing * apxRevSpdAfterTurn) / accRate;
	const float revAccDecTime = revDecTime + revAccTime;

	const float fwdETA = waypointFETA + turnAngleTime + accTime + decTime;
	const float revETA = waypointRETA + revAngleTime + revAccDecTime;

	return (fwdETA > revETA);
}

