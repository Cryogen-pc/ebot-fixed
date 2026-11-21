#include "../../include/core.h"

extern ConVar ebot_humanize; // combat.cpp

// ---------------------------------------------------------------------------------------------
// Humanized movement helpers (human bots: dodging + flank glances, zombie bots: head weaving +
// ledge camper timeout). No new Bot members are used. Idle members are repurposed:
//   m_waiting       (zombie) human camping on an unreachable ledge that is temporarily ignored
//   m_rpm           (zombie) time until which m_waiting stays on the ignore list
//   m_zoomCheckTime (zombie) ledge camp episode, stored as a NEGATIVE start time (0 = no episode)
// ---------------------------------------------------------------------------------------------

// true when a side step in direction dir (+1 right, -1 left) would hit a wall or drop into a pit,
// angles = the heading the strafe is relative to
static bool StrafeBlocked(Bot *bot, const Vector &angles, const float dir, const float reach)
{
	MakeVectors(angles);
	const Vector right = g_pGlobals->v_right;
	const Vector start = bot->pev->origin;
	const Vector end = start + right * (dir * reach);

	TraceResult tr;
	TraceLine(start, end, TraceIgnore::Everything, bot->GetEntity(), &tr);
	if (tr.flFraction < 1.0f)
		return true; // wall

	TraceLine(end, end - Vector(0.0f, 0.0f, 112.0f), TraceIgnore::Everything, bot->GetEntity(), &tr);
	return tr.flFraction >= 1.0f; // no floor behind the step
}

// strafe-dodge and crouch-jump while a zombie closes in (retreating) or while reloading in the open (!retreating)
static void HumanEvade(Bot *bot, const bool retreating)
{
	if (!ebot_humanize.GetBool() || bot->m_isZombieBot || FNullEnt(bot->m_nearestEnemy))
		return;

	if (!bot->IsOnFloor() || bot->IsOnLadder() || bot->IsInWater() || bot->m_checkFall)
		return;

	if (bot->m_waypoint.flags & (WAYPOINT_FALLRISK | WAYPOINT_LADDER | WAYPOINT_LIFT | WAYPOINT_CROUCH))
		return;

	const float now = engine->GetTime();
	const float dist = (bot->pev->origin - bot->m_nearestEnemy->v.origin).GetLength2D();
	const float enemySpeed = bot->m_nearestEnemy->v.velocity.GetLength2D();

	if (retreating)
	{
		// only worth dodging when the zombie is really coming for us
		if (dist > 700.0f || (enemySpeed < 170.0f && dist > 320.0f))
			return;
	}
	else
	{
		// reloading with a zombie in sight: stay around the camp spot, but do not stand still
		if (dist > 900.0f || !(bot->GetAmmoInClip() <= 0 || (bot->m_oldButtons & IN_RELOAD)))
			return;

		if ((bot->m_waypoint.origin - bot->pev->origin).GetLength2D() > 90.0f)
			return;
	}

	// heading the strafe is relative to
	Vector angles = bot->m_moveAngles;
	const bool followingPath = retreating && !bot->m_navNode.IsEmpty() && !bot->m_destOrigin.IsNull();
	if (followingPath)
		angles = (bot->m_destOrigin - bot->pev->origin).ToAngles();
	else if (!retreating)
		angles = (bot->m_nearestEnemy->v.origin - bot->pev->origin).ToAngles();

	angles.x = 0.0f;
	angles.ClampAngles();

	// crouch-jump (MoveAction() ducks automatically while airborne after a jump)
	if (bot->m_jumpTime + 1.4f < now && dist < (retreating ? 360.0f : 500.0f) && chanceof(retreating ? 25 : 12) && (bot->m_destOrigin.IsNull() || !bot->IsDeadlyDrop(bot->m_destOrigin)))
		bot->m_buttons |= IN_JUMP;

	// time sliced pattern, different for every bot, with short "run straight" gaps so it does not look robotic
	const int segment = static_cast<int>((now + static_cast<float>(bot->m_index) * 0.173f) / 0.40f);
	const int pattern = (segment * 31 + bot->m_index * 17) & 7;
	if (pattern == 0 || pattern == 5)
		return;

	const float reach = retreating ? 64.0f : 96.0f;
	if (!retreating && (StrafeBlocked(bot, angles, 1.0f, reach) || StrafeBlocked(bot, angles, -1.0f, reach)))
		return; // not an open area, reloading in cover is fine

	float dir = (pattern & 1) ? 1.0f : -1.0f;
	if (bot->m_sidePreference < 0.0f)
		dir = -dir;

	if (StrafeBlocked(bot, angles, dir, reach))
	{
		dir = -dir;
		if (StrafeBlocked(bot, angles, dir, reach))
			return; // boxed in
	}

	// the dodge owns movement for this tick: steer at the current waypoint ourselves and keep the per frame
	// path code from zeroing the strafe (FollowPath() is called again on the next tick)
	if (followingPath)
		bot->MoveTo(bot->m_destOrigin, false);
	else if (!retreating)
	{
		bot->m_moveAngles = angles;
		bot->m_moveSpeed = 0.0f;
		bot->m_duckTime = 0.0f; // do not crouch while dodging
	}

	bot->m_strafeSpeed = dir * bot->pev->maxspeed * (retreating ? 0.9f : 0.6f);
	bot->m_navNode.Stop();
}

// anti tunnel vision: every few seconds look 55-100 degrees off the travel heading, or at a teammate a zombie is chewing on
static void HumanFlankGlance(Bot *bot)
{
	if (!ebot_humanize.GetBool() || bot->m_isZombieBot || bot->m_hasEnemiesNear || bot->m_aimingAtEnemy || bot->m_hasEntitiesNear)
		return;

	if (bot->IsOnLadder() || bot->IsInWater() || (bot->m_waypoint.flags & (WAYPOINT_USEBUTTON | WAYPOINT_LADDER)))
		return;

	const float now = engine->GetTime();
	if (bot->m_pauseTime > now) // already looking at a sound / noise source
		return;

	bool friendUnderAttack = false;
	if (bot->m_hasFriendsNear && !FNullEnt(bot->m_nearestFriend) && IsAlive(bot->m_nearestFriend))
	{
		const Vector friendOrigin = bot->m_nearestFriend->v.origin;
		if ((friendOrigin - bot->pev->origin).GetLengthSquared() < squaredf(1100.0f) && bot->GetNearbyEnemiesNearPosition(friendOrigin, squaredf(320.0f)) > 0)
			friendUnderAttack = true;
	}

	const float cycle = friendUnderAttack ? 1.6f : (3.4f + static_cast<float>(bot->m_index % 5) * 0.55f);
	const float t = now + static_cast<float>(bot->m_index) * 0.91f;
	const int segment = static_cast<int>(t / cycle);
	const float local = t - static_cast<float>(segment) * cycle;
	if (local > (friendUnderAttack ? 0.8f : 0.55f))
		return;

	if (friendUnderAttack)
	{
		bot->LookAt(bot->m_nearestFriend->v.origin + bot->m_nearestFriend->v.view_ofs);
		return;
	}

	float yaw = (bot->pev->velocity.GetLengthSquared2D() > 400.0f) ? bot->pev->velocity.ToAngles().y : bot->pev->v_angle.y;
	const float side = (((segment + (bot->m_sidePreference < 0.0f ? 1 : 0)) & 1) ? 1.0f : -1.0f);
	yaw += side * (55.0f + static_cast<float>((segment * 7 + bot->m_index * 3) % 6) * 9.0f);

	const float radians = yaw * 0.0174532925f;
	const Vector eye = bot->EyePosition();
	bot->LookAt(Vector(eye.x + ccosf(radians) * 600.0f, eye.y + csinf(radians) * 600.0f, eye.z - 8.0f));
}

// zombie: lateral head weaving when a human is aiming at us at close range
static void ZombieHeadWeave(Bot *bot)
{
	if (!ebot_humanize.GetBool() || !bot->m_isZombieBot || FNullEnt(bot->m_nearestEnemy))
		return;

	if (!bot->IsOnFloor() || bot->IsOnLadder() || bot->IsInWater() || (bot->m_waypoint.flags & (WAYPOINT_FALLRISK | WAYPOINT_LADDER)))
		return;

	edict_t *enemy = bot->m_nearestEnemy;
	const float dist = (bot->pev->origin - enemy->v.origin).GetLength2D();
	if (dist > 420.0f || dist < 72.0f) // far: just run, point blank: we have to connect
		return;

	// only when the human looks at us and is about to shoot (or already shooting)
	if (!IsInViewCone(bot->pev->origin, enemy) || (dist > 260.0f && !bot->IsAttacking(enemy)))
		return;

	const float wave = csinf(engine->GetTime() * 5.0f + static_cast<float>(bot->m_index) * 1.3f);
	if (cabsf(wave) < 0.25f) // brief straight bursts, a perfect sine looks fake
		return;

	float dir = wave > 0.0f ? 1.0f : -1.0f;
	const Vector angles = bot->m_moveAngles; // MoveTo() has just aimed it at the human
	if (StrafeBlocked(bot, angles, dir, 56.0f))
	{
		dir = -dir;
		if (StrafeBlocked(bot, angles, dir, 56.0f))
			return;
	}

	bot->m_strafeSpeed = dir * bot->pev->maxspeed * 0.6f;
}

// zombie: do not stare at an unreachable high ledge human forever.
// 1. human above jump reach, zombie stuck below for 4s: switch to the closest reachable human
// 2. nobody else: crouch-jump at the ledge for up to 6 more seconds
// 3. still nothing: ignore that human for 6s and roam
// returns true while the jump-at-ledge mode owns movement
static bool ZombieLedgeUpdate(Bot *bot)
{
	if (!ebot_humanize.GetBool())
		return false;

	edict_t *target = bot->m_nearestEnemy;
	if (FNullEnt(target) || !IsAlive(target) || GetTeam(target) == bot->m_team)
	{
		if (bot->m_zoomCheckTime < 0.0f)
			bot->m_zoomCheckTime = 0.0f;

		return false;
	}

	const float now = engine->GetTime();
	const float height = target->v.origin.z - bot->pev->origin.z;
	const float flat = (bot->pev->origin - target->v.origin).GetLength2D();
	const bool stuck = height > 70.0f && flat < 300.0f && bot->pev->waterlevel < 2 && !bot->IsOnLadder(); // jump + duck reach is about 62

	if (bot->m_isSlowThink)
	{
		if (!stuck)
		{
			if (bot->m_zoomCheckTime < 0.0f)
				bot->m_zoomCheckTime = 0.0f;

			return false;
		}

		if (bot->m_zoomCheckTime >= 0.0f) // new episode
		{
			bot->m_zoomCheckTime = -now;
			return false;
		}

		// still travelling (stairs, ladder, around a corner), not stuck: push the start time back
		if (bot->pev->velocity.GetLength2D() > 140.0f)
		{
			bot->m_zoomCheckTime -= 0.5f;
			return false;
		}

		const float elapsed = now + bot->m_zoomCheckTime;
		if (elapsed > 4.0f)
		{
			edict_t *alternative = nullptr;
			float best = 999999999.0f;
			for (const Clients &client : g_clients)
			{
				if (client.ignore || !(client.flags & CFLAG_USED) || !(client.flags & CFLAG_ALIVE) || client.team == bot->m_team || client.ent == target)
					continue;

				if (FNullEnt(client.ent) || !IsAlive(client.ent) || (client.ent->v.flags & FL_NOTARGET) || bot->IsEnemyInvincible(client.ent))
					continue;

				// same height window CheckReachable() uses
				const float dz = client.ent->v.origin.z - bot->pev->origin.z;
				if (dz > 62.0f || dz < -100.0f)
					continue;

				const float distance = (bot->pev->origin - client.ent->v.origin).GetLengthSquared();
				if (distance < best)
				{
					best = distance;
					alternative = client.ent;
				}
			}

			if (alternative != nullptr)
			{
				bot->m_waiting = target;
				bot->m_rpm = now + 8.0f;
				bot->m_nearestEnemy = alternative;
				bot->m_isEnemyReachable = false;
				bot->m_zoomCheckTime = 0.0f;
				bot->m_navNode.Clear();
				return false;
			}

			if (elapsed > 10.0f) // jumping did not help and nobody else to chase: forget him for a while
			{
				bot->m_waiting = target;
				bot->m_rpm = now + 6.0f;
				bot->m_nearestEnemy = nullptr;
				bot->m_isEnemyReachable = false;
				bot->m_zoomCheckTime = 0.0f;
				bot->m_navNode.Clear();
				return false;
			}
		}
	}
	else if (!stuck)
		return false;

	// jump-at-the-ledge mode, derived from the episode timer
	if (bot->m_zoomCheckTime >= 0.0f || now + bot->m_zoomCheckTime <= 4.0f)
		return false;

	bot->MoveTo(target->v.origin, false);
	bot->m_aimingAtEnemy = true;
	bot->LookAt(target->v.origin + target->v.view_ofs);

	if (bot->IsOnFloor() && bot->m_jumpTime + 0.9f < now)
		bot->m_buttons |= IN_JUMP; // MoveAction() turns it into a crouch-jump

	bot->KnifeAttack();
	bot->m_navNode.Stop();
	return true;
}

void Bot::DefaultStart(void)
{

}

void Bot::DefaultUpdate(void)
{
	if (m_isZombieBot)
	{
		m_aimingAtEnemy = false;

		// nearest enemy never resets to nullptr, so bot always know where are alive humans
		if (IsAlive(m_nearestEnemy) && GetTeam(m_nearestEnemy) != m_team)
		{
			// human camping on an unreachable high ledge: switch target / jump at it / give up after a timeout
			if (ZombieLedgeUpdate(this))
			{
				if (m_isSlowThink)
				{
					FindEnemyEntities();
					FindFriendsAndEnemiens();
				}

				return;
			}

			if (m_isSlowThink)
			{
				CheckReachable();
				if (!CheckVisibility(m_nearestEnemy))
					m_isEnemyReachable = false;
			}
			else if (m_enemyDistance < 512.0f && chanceof(m_skill))
				KnifeAttack();

			// path matrix returns 0 if we are on the same waypoint, so basically its reachable
			if (m_hasEnemiesNear && (Math::FltZero(m_enemyDistance) || m_isEnemyReachable))
			{
				Vector nextVec;
				nextVec.x = m_enemyOrigin.x + m_nearestEnemy->v.velocity.x;
				nextVec.y = m_enemyOrigin.y + m_nearestEnemy->v.velocity.y;
				nextVec.z = m_enemyOrigin.z;
				if ((pev->origin - m_enemyOrigin).GetLengthSquared2D() < (pev->origin - nextVec).GetLengthSquared2D())
					MoveTo(nextVec);
				else
					MoveTo(m_enemyOrigin);

				m_aimingAtEnemy = true;
				LookAt(m_enemyOrigin);

				if (m_isSlowThink)
				{
					m_navNode.Clear();
					FindEnemyEntities();
					FindFriendsAndEnemiens();
					FindWaypoint();
				}
				else
					KnifeAttack();

				ZombieHeadWeave(this);
				m_navNode.Stop();
				return;
			}
			else if (!m_navNode.IsEmpty())
			{
				if (m_isSlowThink && m_navNode.HasNext() && (g_waypoint->m_paths[m_navNode.Last()].origin - pev->origin).GetLengthSquared() < (g_waypoint->m_paths[m_navNode.Last()].origin - m_nearestEnemy->v.origin).GetLengthSquared())
				{
					KnifeAttack();
					int16_t index = g_clients[ENTINDEX(m_nearestEnemy) - 1].wp;
					if (IsValidWaypoint(index))
					{
						m_currentGoalIndex = index;
						FindPath(m_currentWaypointIndex, index);
					}
					else
					{
						index = g_waypoint->FindNearest(m_nearestEnemy->v.origin);
						if (IsValidWaypoint(index))
						{
							m_currentGoalIndex = index;
							FindPath(m_currentWaypointIndex, index);
						}
					}
				}
				else
					FollowPath();
			}
			else
			{
				int16_t index = g_clients[ENTINDEX(m_nearestEnemy) - 1].wp;
				if (IsValidWaypoint(index))
				{
					m_currentGoalIndex = index;
					FindPath(m_currentWaypointIndex, index);
				}
				else
				{
					index = g_waypoint->FindNearest(m_nearestEnemy->v.origin);
					if (IsValidWaypoint(index))
					{
						m_currentGoalIndex = index;
						FindPath(m_currentWaypointIndex, index);
					}
					else
					{
						index = static_cast<int16_t>(crandomint(0, g_numWaypoints - 1));
						m_currentGoalIndex = index;
						FindPath(m_currentWaypointIndex, index);
					}
				}
			}
		}
		else
		{
			m_isEnemyReachable = false;

			// search other bots to get valid enemy
			if (m_isSlowThink)
			{
				for (auto bot : g_botManager->m_bots)
				{
					if (bot && bot->m_team == m_team && bot->m_hasEnemiesNear && IsAlive(bot->m_nearestEnemy) && GetTeam(bot->m_nearestEnemy) != m_team)
					{
						m_nearestEnemy = bot->m_nearestEnemy;
						return;
					}
				}
			}

			if (!m_navNode.IsEmpty())
				FollowPath();
			else
			{
				KnifeAttack();
				int16_t ref = FindGoalZombie();
				FindPath(m_currentWaypointIndex, ref);
			}
		}

		if (m_isSlowThink)
		{
			m_zhCampPointIndex = -1;

			FindEnemyEntities();
			FindFriendsAndEnemiens();
		}
		else
			UpdateLooking();
	}
		else
		{
			UpdateLooking();
			HumanFlankGlance(this);

			if (IsValidWaypoint(m_zhCampPointIndex))
				m_currentGoalIndex = m_zhCampPointIndex;

			if (m_isSlowThink)
			{
				FindEnemyEntities();
				FindFriendsAndEnemiens();
			CheckReachable();

			// revert the zoom to normal
			if (!m_hasEnemiesNear && !m_hasEntitiesNear && UsesSniper() && pev->fov != 90.0f)
				m_buttons |= IN_ATTACK2;
		}
		else if (m_hasEnemiesNear && (m_isEnemyReachable || Math::FltZero(m_enemyDistance)))
		{
			if (!m_navNode.HasNext())
			{
				// find new safe spot
				m_myMeshWaypoint = -1;
				m_zhCampPointIndex = -1;
				FindGoalHuman();

				// use known waypoint first, then switch to auto
				FindEscapePath(m_currentWaypointIndex, m_enemyOrigin);

				if (m_navNode.IsEmpty())
				{
					MoveOut(m_enemyOrigin);
					m_navNode.Stop();
				}
				else
					FollowPath();

				m_currentWaypointIndex = -1;
			}
			else if (m_navNode.Length() > 1)
			{
				const int16_t firstWP = m_navNode.First();
				const int16_t nextWP = m_navNode.Next();
				if (IsValidWaypoint(firstWP) && IsValidWaypoint(nextWP) && ((pev->origin - g_waypoint->m_paths[firstWP].origin).GetLengthSquared() > ((m_nearestEnemy->v.origin + m_nearestEnemy->v.velocity) - g_waypoint->m_paths[firstWP].origin).GetLengthSquared() || (pev->origin - g_waypoint->m_paths[nextWP].origin).GetLengthSquared() > ((m_nearestEnemy->v.origin + m_nearestEnemy->v.velocity) - g_waypoint->m_paths[nextWP].origin).GetLengthSquared()) && ::IsInViewCone(pev->origin, m_nearestEnemy))
				{
					// find new safe spot if possible
					m_myMeshWaypoint = -1;
					m_zhCampPointIndex = -1;
					FindGoalHuman();

					m_navNode.Clear();
					FindEscapePath(m_currentWaypointIndex, m_enemyOrigin);
					MoveOut(m_enemyOrigin);
					m_navNode.Stop();

					return;
				}
			}

			// if our enemy is closer to this waypoint, just skip it otherwise we will get infected
			const int16_t firstWP = m_navNode.First();
			if (IsValidWaypoint(firstWP) && (g_waypoint->m_paths[firstWP].origin - m_enemyOrigin).GetLengthSquared() < (g_waypoint->m_paths[firstWP].origin - pev->origin).GetLengthSquared())
				m_navNode.Shift();

			if (m_navNode.IsEmpty())
			{
				MoveOut(m_enemyOrigin);
				m_navNode.Stop();
			}
			else
				FollowPath();

			// strafe-dodge / crouch-jump while the zombie closes in
			HumanEvade(this, true);
			return;
		}

		if (m_currentWaypointIndex == m_zhCampPointIndex && IsValidWaypoint(m_zhCampPointIndex))
		{
			if (!m_navNode.IsEmpty())
			{
				if (m_hasFriendsNear && !FNullEnt(m_nearestFriend) && (pev->origin - m_nearestFriend->v.origin).GetLengthSquared() < squaredf(48.0f))
				{
					const Bot* bot = g_botManager->GetBot(m_nearestFriend);
					if (bot && bot->m_navNode.IsEmpty() && m_zhCampPointIndex == bot->m_zhCampPointIndex)
					{
						m_navNode.Clear();
						MoveTo(m_nearestFriend->v.origin);
						return;
					}
				}

				FollowPath();
				return;
			}

			const Path zhPath = g_waypoint->m_paths[m_zhCampPointIndex];
			if (!(zhPath.flags & WAYPOINT_ZMHMCAMP) && !(zhPath.flags & WAYPOINT_HMCAMPMESH))
			{
				m_moveSpeed = pev->maxspeed;
				m_zhCampPointIndex = -1;
				m_myMeshWaypoint = -1;
				FindGoalHuman();
				return;
			}

			if (m_isSlowThink)
			{
				const float maxRange = zhPath.flags & WAYPOINT_CROUCH ? 125.0f : 200.0f;
				if (((zhPath.origin - pev->origin).GetLengthSquared2D() > squaredf(maxRange) || (zhPath.origin.z - 54.0f > pev->origin.z)))
				{
					FindWaypoint();
					FindPath(m_currentWaypointIndex, m_currentGoalIndex);
					return;
				}
			}
			else if (zhPath.flags & WAYPOINT_CROUCH)
				m_duckTime = engine->GetTime() + 1.0f;

			m_moveSpeed = 0.0f;
			m_strafeSpeed = 0.0f;

			ResetStuck();

			if (!g_waypoint->m_hmMeshPoints.IsEmpty())
			{
				const float time2 = engine->GetTime();
				if (m_currentProcessTime < time2 + 1.0f || m_currentProcessTime > time2 + 60.0f)
				{
					int16_t i, index, myCampPoint;
					CArray<int16_t>MeshWaypoints;

					for (i = 0; i < g_waypoint->m_hmMeshPoints.Size(); i++)
					{
						index = g_waypoint->m_hmMeshPoints.Get(i);
						if (!g_waypoint->GetPath(index)->mesh)
							continue;

						if (zhPath.mesh != g_waypoint->GetPath(index)->mesh)
							continue;

						MeshWaypoints.Push(index);
					}

					if (!MeshWaypoints.IsEmpty())
					{
						myCampPoint = MeshWaypoints.Random();
						m_myMeshWaypoint = myCampPoint;
						MeshWaypoints.Destroy();

						float max = 16.0f;
						if (m_hasEnemiesNear)
						{
							if (m_personality == Personality::Rusher)
								max = 20.0f;
							else if (m_personality != Personality::Careful)
								max = 12.0f;
						}

						m_currentProcessTime = time2 + crandomfloat(8.0f, max);
						m_zhCampPointIndex = m_myMeshWaypoint;
						m_currentGoalIndex = m_zhCampPointIndex;
						FindPath(m_currentWaypointIndex, m_myMeshWaypoint);
					}
				}
			}

			// standing still
			if (m_hasEnemiesNear && m_currentWeapon != Weapon::Knife && m_personality != Personality::Rusher && pev->velocity.GetLengthSquared2D() < 20.0f)
			{
				bool crouch = true;
				if (m_currentWeapon != Weapon::M3 &&
					m_currentWeapon != Weapon::Xm1014 &&
					m_currentWeapon != Weapon::G3SG1 &&
					m_currentWeapon != Weapon::Scout &&
					m_currentWeapon != Weapon::Awp &&
					m_currentWeapon != Weapon::M249 &&
					m_currentWeapon != Weapon::Sg550)
					crouch = false;

				if (m_personality == Personality::Normal && m_enemyDistance < 512.0f)
					crouch = false;

				if (crouch && IsVisible(m_enemyOrigin, GetEntity()))
					m_duckTime = engine->GetTime() + 1.0f;
			}

			// reloading in an open area with a zombie in sight: do not stand still
			if (m_hasEnemiesNear)
				HumanEvade(this, false);
		}
		else
		{
			if (m_navNode.HasNext())
				FollowPath();
			else if (IsValidWaypoint(m_zhCampPointIndex))
				FindPath(m_currentWaypointIndex, m_zhCampPointIndex);
			else
				m_zhCampPointIndex = FindGoalHuman();
		}
	}
}

void Bot::DefaultEnd(void)
{

}

bool Bot::DefaultReq(void)
{
	return true;
}
