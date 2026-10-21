// Input buffer ("trajectory fix"), all gamemodes.
//
// When a player is about to die, try two fixes, in order:
//
// 1. Shift the last click/release. If the death comes shortly after an input,
//    estimate where they'd be if that input had happened a few ticks earlier
//    or later. Shifting an input by k ticks moves the player by about
//    k * (current y-velocity - the y-velocity they'd have without the input).
//    Exact for the wave; a close first-order guess for gravity modes.
//
// 2. Early release. If they're holding (wave or ship) and would have survived
//    by letting go up to a few ticks ago, move them to where that release
//    would have put them and release the click for them.
//
// Either way the new spot must be clear of hazards for a few ticks ahead.
// Moving hazards are tracked tick by tick while the buffer is on, so that
// check uses where each one will be, not just where it is now.
//
// F6 (in a level) cycles: OFF -> 2 -> 4 -> 8 -> 16 ticks (wave capped at 8) -> OFF.
// It starts OFF every session. While it's on, an on-screen label always shows
// the current stage; that label can't be turned off.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {
	// Same stages as the original: all gamemodes / wave.
	constexpr int kStageTicksAll[] = {0, 2, 4, 8, 16};
	constexpr int kStageTicksWave[] = {0, 2, 4, 8, 8};
	constexpr int kStageCount = 5;
	// A death counts as "caused by the last input" if it happens within this many ticks of it.
	constexpr long long kDeathWindow = 48;
	// After moving the player, the new path is checked this many ticks ahead for hazards.
	constexpr int kLookahead = 8;
	// Never move the player further than this (about 3 blocks).
	constexpr double kMaxShift = 90.0;
	// Velocity changes bigger than this between ticks are portals/teleports, not gravity.
	constexpr double kMaxAccel = 3.0;
	// Objects that jump further than this in one tick were teleported, not moved.
	constexpr float kMaxObjectSpeed = 40.f;
	// How often (in ticks) the list of objects near the player is rebuilt.
	constexpr long long kWatchRefresh = 10;

	int g_stage = 0;
	long long g_step = 0;
	// Set while the mod releases a click itself, so that release isn't treated as the player's.
	bool g_virtualRelease = false;

	struct PlayerState {
		bool hasPrev = false;
		CCPoint prev;
		double lastDx = 0;
		double lastDy = 0;
		double prevDy = 0;
		long long lastMoveStep = -1;
		long long prevMoveStep = -1;
		long long inputStep = -1;
		double dyBeforeInput = 0;
		double accelBeforeInput = 0;
		bool correctedThisInput = false;
		long long correctedStep = -1;
		bool holding = false;
		long long holdStartStep = -1;
		bool autoReleasedThisHold = false;
	};
	PlayerState g_states[2];

	// Ship acceleration while holding / not holding, learned from play.
	// Stored as if gravity were normal; index 0 = normal size, 1 = mini.
	struct ShipAccel {
		bool haveHeld = false;
		bool haveReleased = false;
		double held = 0;
		double released = 0;
	};
	ShipAccel g_shipAccel[2];

	OBB2D* g_playerBox = nullptr;

	// Moving-object tracking: where each nearby hazard/block was last tick and how fast it's moving.
	struct ObjectTrack {
		CCPoint pos;
		CCPoint vel;
		long long step = -1;
	};
	std::unordered_map<GameObject*, ObjectTrack> g_tracks;
	std::vector<GameObject*> g_watch;
	long long g_watchBuiltStep = -1;

	void clearTracks() {
		g_tracks.clear();
		g_watch.clear();
		g_watchBuiltStep = -1;
	}

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("input-buffer");
	}

	bool active() {
		return g_stage != 0 && enabled();
	}

	int ticksFor(PlayerObject* player) {
		return player && player->m_isDart ? kStageTicksWave[g_stage] : kStageTicksAll[g_stage];
	}

	void resetStates() {
		for (auto& s : g_states) s = PlayerState{};
	}

	PlayerState* stateFor(GJBaseGameLayer* layer, PlayerObject* player) {
		if (!layer || !player) return nullptr;
		if (player == layer->m_player1) return &g_states[0];
		if (player == layer->m_player2) return &g_states[1];
		return nullptr;
	}

	int sizeIndex(PlayerObject* player) {
		return player->m_vehicleSize < 1.f ? 1 : 0;
	}

	double gravitySign(PlayerObject* player) {
		return player->m_isUpsideDown ? -1.0 : 1.0;
	}

	std::string stageText() {
		if (g_stage == 0) return "OFF (100% raw input)";
		int all = kStageTicksAll[g_stage];
		int wave = kStageTicksWave[g_stage];
		auto text = std::to_string(all) + " ticks";
		if (wave != all) text += " (wave " + std::to_string(wave) + ")";
		return text;
	}

	void updateIndicator(PlayLayer* layer) {
		if (!layer) return;
		auto label = static_cast<CCLabelBMFont*>(layer->getChildByID("input-buffer-label"_spr));
		if (!label) {
			label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setID("input-buffer-label"_spr);
			label->setAnchorPoint({0.f, 1.f});
			label->setScale(0.4f);
			label->setColor({255, 120, 40});
			auto size = CCDirector::get()->getWinSize();
			label->setPosition({6.f, size.height - 6.f});
			layer->addChild(label, 10000);
		}
		bool on = active();
		label->setVisible(on);
		if (on) {
			std::string text = "INPUT BUFFER: " + stageText();
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			label->setString(text.c_str());
		}
	}

	double clampAccel(double a) {
		return std::abs(a) > kMaxAccel ? 0.0 : a;
	}

	void trackMovement(PlayerObject* player, PlayerState& s) {
		if (!player) return;
		CCPoint pos = player->m_position;
		if (s.hasPrev) {
			s.prevDy = s.lastDy;
			s.prevMoveStep = s.lastMoveStep;
			s.lastDx = pos.x - s.prev.x;
			s.lastDy = pos.y - s.prev.y;
			s.lastMoveStep = g_step;

			// Learn the ship's acceleration. Zero means it's at max speed or touching
			// a floor/ceiling, which says nothing about gravity, so skip it.
			if (player->m_isShip && s.prevMoveStep == s.lastMoveStep - 1) {
				double a = s.lastDy - s.prevDy;
				if (std::abs(a) > 1e-4 && std::abs(a) <= kMaxAccel) {
					auto& ship = g_shipAccel[sizeIndex(player)];
					double normalized = a * gravitySign(player);
					if (s.holding) {
						ship.held = normalized;
						ship.haveHeld = true;
					} else {
						ship.released = normalized;
						ship.haveReleased = true;
					}
				}
			}
		}
		s.prev = pos;
		s.hasPrev = true;
	}

	void noteInput(PlayerObject* player, PlayerButton button, bool isPush) {
		if (button != PlayerButton::Jump || g_virtualRelease) return;
		auto s = stateFor(PlayLayer::get(), player);
		if (!s) return;
		bool haveLast = s->lastMoveStep >= 0 && s->lastMoveStep < g_step;
		bool haveAccel = haveLast && s->prevMoveStep == s->lastMoveStep - 1;
		s->inputStep = g_step;
		s->dyBeforeInput = haveLast ? s->lastDy : 0.0;
		// The wave has no gravity; anything that looks like acceleration is a direction flip.
		s->accelBeforeInput = (haveAccel && !player->m_isDart) ? clampAccel(s->lastDy - s->prevDy) : 0.0;
		s->correctedThisInput = false;
		if (isPush && !s->holding) {
			s->holdStartStep = g_step;
			s->autoReleasedThisHold = false;
		}
		s->holding = isPush;
	}

	OBB2D* playerBox(CCRect const& r) {
		CCPoint center{r.getMidX(), r.getMidY()};
		if (!g_playerBox) {
			g_playerBox = OBB2D::create(center, r.size.width, r.size.height, 0.f);
			g_playerBox->retain();
		} else {
			g_playerBox->calculateWithCenter(center, r.size.width, r.size.height, 0.f);
		}
		return g_playerBox;
	}

	bool hitsHazard(CCRect const& r, GameObject* obj) {
		float radius = obj->getObjectRadius();
		if (radius > 0.f) {
			auto const& orc = obj->getObjectRect();
			float cx = orc.getMidX();
			float cy = orc.getMidY();
			float nx = std::clamp(cx, r.getMinX(), r.getMaxX());
			float ny = std::clamp(cy, r.getMinY(), r.getMaxY());
			float dx = cx - nx;
			float dy = cy - ny;
			return dx * dx + dy * dy <= radius * radius;
		}
		if (obj->m_orientedBox) {
			if (auto box = obj->getOrientedBox()) {
				return playerBox(r)->overlaps(box);
			}
		}
		return r.intersectsRect(obj->getObjectRect());
	}

	bool insideSolid(CCRect const& r, GameObject* obj) {
		// Shrink a little so standing on or sliding along a block doesn't count.
		CCRect inner{r.origin.x + 1.f, r.origin.y + 1.f, r.size.width - 2.f, r.size.height - 2.f};
		return inner.intersectsRect(obj->getObjectRect());
	}

	// 0 = not something that can kill or block, 1 = hazard, 2 = solid.
	int obstacleKind(PlayLayer* layer, GameObject* obj) {
		if (!obj) return 0;
		auto type = obj->m_objectType;
		bool hazard = type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard;
		bool solid = type == GameObjectType::Solid;
		if (!hazard && !solid) return 0;
		if (obj->m_isGroupDisabled || obj->m_isNoTouch || obj->m_isPassable) return 0;
		if (obj == layer->m_anticheatSpike) return 0;
		return hazard ? 1 : 2;
	}

	// Rebuild the list of obstacles near either player. Done every few ticks,
	// since scanning the whole level every tick would be too slow.
	void rebuildWatch(PlayLayer* layer) {
		g_watch.clear();
		g_watchBuiltStep = g_step;

		std::vector<CCRect> areas;
		for (auto player : {layer->m_player1, layer->m_player2}) {
			if (!player) continue;
			auto pos = player->m_position;
			areas.emplace_back(pos.x - 250.f, pos.y - 450.f, 900.f, 900.f);
		}
		if (areas.empty()) return;

		for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
			if (!obstacleKind(layer, obj)) continue;
			auto pos = obj->getPosition();
			for (auto const& area : areas) {
				if (area.containsPoint(pos)) {
					g_watch.push_back(obj);
					break;
				}
			}
		}

		// Forget objects that are no longer nearby.
		for (auto it = g_tracks.begin(); it != g_tracks.end();) {
			if (it->second.step < g_step - kWatchRefresh * 2) it = g_tracks.erase(it);
			else ++it;
		}
	}

	// Record where each nearby obstacle is this tick and how far it moved since last tick.
	void updateTracks(PlayLayer* layer) {
		if (!active()) {
			if (!g_tracks.empty() || !g_watch.empty()) clearTracks();
			return;
		}
		if (g_watchBuiltStep < 0 || g_step - g_watchBuiltStep >= kWatchRefresh) {
			rebuildWatch(layer);
		}
		for (auto obj : g_watch) {
			auto const& rect = obj->getObjectRect();
			CCPoint center{rect.getMidX(), rect.getMidY()};
			auto& track = g_tracks[obj];
			if (track.step == g_step - 1) {
				CCPoint vel = center - track.pos;
				bool teleported = std::abs(vel.x) > kMaxObjectSpeed || std::abs(vel.y) > kMaxObjectSpeed;
				track.vel = teleported ? CCPoint{0.f, 0.f} : vel;
			} else {
				track.vel = {0.f, 0.f};
			}
			track.pos = center;
			track.step = g_step;
		}
	}

	CCPoint velocityOf(GameObject* obj) {
		auto it = g_tracks.find(obj);
		if (it == g_tracks.end() || it->second.step < g_step - 1) return {0.f, 0.f};
		return it->second.vel;
	}

	struct MovingObstacle {
		GameObject* obj;
		CCPoint vel;
	};

	struct Obstacles {
		std::vector<MovingObstacle> hazards;
		std::vector<GameObject*> solids;
	};

	Obstacles gatherObstacles(PlayLayer* layer, CCRect const& base, float reachX, float reachY) {
		Obstacles out;
		// Extra margin so hazards that will move into the path are included.
		float margin = 150.f + kMaxObjectSpeed * kLookahead;
		float minX = base.getMinX() - margin;
		float maxX = base.getMaxX() + reachX + margin;
		float minY = base.getMinY() - reachY - margin;
		float maxY = base.getMaxY() + reachY + margin;

		for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
			int kind = obstacleKind(layer, obj);
			if (!kind) continue;
			auto pos = obj->getPosition();
			if (pos.x < minX || pos.x > maxX || pos.y < minY || pos.y > maxY) continue;
			if (kind == 1) out.hazards.push_back({obj, velocityOf(obj)});
			else out.solids.push_back(obj);
		}
		return out;
	}

	bool pathClear(CCRect const& base, Obstacles const& obs, double shift, double dx, double dy, double accel) {
		CCRect start = base;
		start.origin.y += static_cast<float>(shift);
		for (auto obj : obs.solids) {
			if (insideSolid(start, obj)) return false;
		}
		for (int j = 0; j <= kLookahead; ++j) {
			CCRect r = base;
			r.origin.x += static_cast<float>(j * dx);
			r.origin.y += static_cast<float>(shift + j * dy + 0.5 * j * (j + 1) * accel);
			for (auto const& hazard : obs.hazards) {
				// Instead of moving the hazard j ticks forward, move the player the opposite way.
				CCRect relative = r;
				relative.origin.x -= hazard.vel.x * j;
				relative.origin.y -= hazard.vel.y * j;
				if (hitsHazard(relative, hazard.obj)) return false;
			}
		}
		return true;
	}

	void movePlayer(PlayerObject* player, PlayerState* s, double shift) {
		CCPoint pos = player->m_position;
		pos.y += static_cast<float>(shift);
		player->m_position = pos;
		player->setPosition(pos);
		s->prev.y += static_cast<float>(shift);
		s->correctedStep = g_step;
	}

	// Fix 1: treat the last click/release as if it happened a few ticks earlier or later.
	bool shiftLastInput(PlayLayer* layer, PlayerObject* player, PlayerState* s) {
		if (s->inputStep < 0 || s->correctedThisInput) return false;

		long long since = g_step - s->inputStep;
		if (since < 0 || since > kDeathWindow) return false;
		s->correctedThisInput = true;

		bool wave = player->m_isDart;
		double dyNow;
		double accelNow = 0.0;
		if (s->lastMoveStep >= s->inputStep) {
			dyNow = s->lastDy;
			if (!wave && s->prevMoveStep >= s->inputStep && s->prevMoveStep == s->lastMoveStep - 1) {
				accelNow = clampAccel(s->lastDy - s->prevDy);
			}
		} else if (wave && s->dyBeforeInput != 0.0) {
			dyNow = -s->dyBeforeInput;
		} else {
			return false;
		}

		// What the y-velocity would be now if the input had never happened.
		double dyWithoutInput = s->dyBeforeInput + static_cast<double>(since) * s->accelBeforeInput;
		double delta = dyNow - dyWithoutInput;
		double dx = s->lastDx;
		if (std::abs(delta) < 0.05 || std::abs(dx) < 0.01) return false;

		int maxTicks = ticksFor(player);
		CCRect base = player->getObjectRect();
		float reachY = static_cast<float>(
			std::min(std::abs(delta) * maxTicks, kMaxShift)
			+ std::abs(dyNow) * kLookahead
			+ 0.5 * kLookahead * (kLookahead + 1) * std::abs(accelNow)
		);
		auto obstacles = gatherObstacles(layer, base, static_cast<float>(std::abs(dx) * kLookahead), reachY);

		for (int k = 1; k <= maxTicks; ++k) {
			for (int dir : {1, -1}) {
				// "Later" can't go past the moment the input actually happened.
				if (dir == -1 && k > since) continue;
				double shift = dir * k * delta;
				if (std::abs(shift) > kMaxShift) continue;
				if (!pathClear(base, obstacles, shift, dx, dyNow, accelNow)) continue;

				movePlayer(player, s, shift);
				log::info(
					"[InputBuffer] Trajectory fix ({}): input treated as {} tick(s) {} (shift {:.2f})",
					wave ? "wave" : "other", k, dir == 1 ? "earlier" : "later", shift
				);
				return true;
			}
		}
		return false;
	}

	// Fix 2: the player held too long; release for them as if they'd let go a few ticks ago.
	bool releaseEarly(PlayLayer* layer, PlayerObject* player, PlayerState* s) {
		if (!s->holding || s->autoReleasedThisHold) return false;
		bool wave = player->m_isDart;
		bool ship = player->m_isShip;
		if (!wave && !ship) return false;
		if (s->lastMoveStep < 0) return false;
		s->autoReleasedThisHold = true;

		double dx = s->lastDx;
		double dyNow = s->lastDy;
		if (std::abs(dx) < 0.01) return false;

		// Can't release before the click started.
		long long heldFor = s->holdStartStep >= 0 ? g_step - s->holdStartStep : kDeathWindow;
		int maxTicks = static_cast<int>(std::min<long long>(ticksFor(player), heldFor));
		if (maxTicks < 1) return false;

		// Per-tick effect of having released one tick earlier, and the motion right after release.
		double dyAfter;
		double accelAfter;
		double accelDiff = 0.0;
		if (wave) {
			if (std::abs(dyNow) < 0.05) return false;
			dyAfter = -dyNow;
			accelAfter = 0.0;
		} else {
			auto const& learned = g_shipAccel[sizeIndex(player)];
			if (!learned.haveHeld || !learned.haveReleased) return false;
			double sign = gravitySign(player);
			accelDiff = (learned.released - learned.held) * sign;
			dyAfter = dyNow;
			accelAfter = learned.released * sign;
		}

		auto shiftFor = [&](int k) {
			// Wave: k ticks going the other way. Ship: k ticks of different acceleration.
			return wave ? -2.0 * k * dyNow : 0.5 * k * (k + 1) * accelDiff;
		};

		CCRect base = player->getObjectRect();
		float reachY = static_cast<float>(
			std::min(std::abs(shiftFor(maxTicks)), kMaxShift)
			+ std::abs(dyAfter) * kLookahead
			+ 0.5 * kLookahead * (kLookahead + 1) * std::abs(accelAfter)
		);
		auto obstacles = gatherObstacles(layer, base, static_cast<float>(std::abs(dx) * kLookahead), reachY);

		for (int k = 1; k <= maxTicks; ++k) {
			double shift = shiftFor(k);
			if (std::abs(shift) < 0.01 || std::abs(shift) > kMaxShift) continue;
			if (!pathClear(base, obstacles, shift, dx, dyAfter, accelAfter)) continue;

			movePlayer(player, s, shift);

			g_virtualRelease = true;
			player->releaseButton(PlayerButton::Jump);
			g_virtualRelease = false;

			s->holding = false;
			s->inputStep = g_step;
			s->dyBeforeInput = dyNow;
			s->accelBeforeInput = 0.0;
			s->correctedThisInput = true;

			log::info(
				"[InputBuffer] Early release ({}): released {} tick(s) early (shift {:.2f})",
				wave ? "wave" : "ship", k, shift
			);
			return true;
		}
		return false;
	}

	// Returns true if the death was avoided.
	bool tryCorrect(PlayLayer* layer, PlayerObject* player, GameObject* killer) {
		if (!active() || !player || !killer) return false;
		if (killer == layer->m_anticheatSpike) return false;
		if (layer->m_isPlatformer || player->m_isSideways) return false;

		auto s = stateFor(layer, player);
		if (!s) return false;

		// Already moved the player this tick; GD is still finishing checks against the old spot.
		if (s->correctedStep == g_step) return true;

		if (shiftLastInput(layer, player, s)) return true;
		if (releaseEarly(layer, player, s)) return true;
		return false;
	}
}

class $modify(BufferPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		clearTracks();
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
			return false;
		}
		resetStates();
		updateIndicator(this);
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		resetStates();
		clearTracks();
		updateIndicator(this);
	}

	void onQuit() {
		clearTracks();
		PlayLayer::onQuit();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (tryCorrect(this, player, object)) {
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}
};

class $modify(BufferGameLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		auto playLayer = typeinfo_cast<PlayLayer*>(this);
		if (!playLayer) {
			return;
		}
		trackMovement(m_player1, g_states[0]);
		trackMovement(m_player2, g_states[1]);
		updateTracks(playLayer);
		++g_step;
	}
};

class $modify(BufferPlayer, PlayerObject) {
	bool pushButton(PlayerButton button) {
		noteInput(this, button, true);
		return PlayerObject::pushButton(button);
	}

	bool releaseButton(PlayerButton button) {
		noteInput(this, button, false);
		return PlayerObject::releaseButton(button);
	}
};

class $modify(BufferKeys, CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double time) {
		if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat && enabled()) {
			if (auto layer = PlayLayer::get()) {
				g_stage = (g_stage + 1) % kStageCount;
				updateIndicator(layer);
				auto text = "Input buffer: " + stageText();
				log::info("[InputBuffer] F6 Cycle -> {}", text);
				Notification::create(text.c_str(), NotificationIcon::Info, 1.f)->show();
			}
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, time);
	}
};
