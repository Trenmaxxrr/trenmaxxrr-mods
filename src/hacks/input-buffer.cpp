// Input buffer ("trajectory fix"), all gamemodes.
//
// When a player is about to die shortly after a click or release, estimate
// where they'd be if that input had happened a few ticks earlier or later. If
// one of those spots is clear of hazards (and stays clear for a few ticks),
// move the player there instead of dying.
//
// The estimate: shifting an input by k ticks moves the player by about
// k * (current y-velocity - the y-velocity they'd have without the input).
// For the wave that's exact; for gravity modes it's a close first-order guess.
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

	int g_stage = 0;
	long long g_step = 0;

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
	};
	PlayerState g_states[2];

	OBB2D* g_playerBox = nullptr;

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
		}
		s.prev = pos;
		s.hasPrev = true;
	}

	void noteInput(PlayerObject* player, PlayerButton button) {
		if (button != PlayerButton::Jump) return;
		auto s = stateFor(PlayLayer::get(), player);
		if (!s) return;
		bool haveLast = s->lastMoveStep >= 0 && s->lastMoveStep < g_step;
		bool haveAccel = haveLast && s->prevMoveStep == s->lastMoveStep - 1;
		s->inputStep = g_step;
		s->dyBeforeInput = haveLast ? s->lastDy : 0.0;
		// The wave has no gravity; anything that looks like acceleration is a direction flip.
		s->accelBeforeInput = (haveAccel && !player->m_isDart) ? clampAccel(s->lastDy - s->prevDy) : 0.0;
		s->correctedThisInput = false;
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

	struct Obstacles {
		std::vector<GameObject*> hazards;
		std::vector<GameObject*> solids;
	};

	Obstacles gatherObstacles(PlayLayer* layer, CCRect const& base, float reachX, float reachY) {
		Obstacles out;
		float minX = base.getMinX() - 150.f;
		float maxX = base.getMaxX() + reachX + 150.f;
		float minY = base.getMinY() - reachY - 150.f;
		float maxY = base.getMaxY() + reachY + 150.f;

		for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
			if (!obj) continue;
			auto type = obj->m_objectType;
			bool hazard = type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard;
			bool solid = type == GameObjectType::Solid;
			if (!hazard && !solid) continue;
			if (obj->m_isGroupDisabled || obj->m_isNoTouch || obj->m_isPassable) continue;
			if (obj == layer->m_anticheatSpike) continue;
			auto pos = obj->getPosition();
			if (pos.x < minX || pos.x > maxX || pos.y < minY || pos.y > maxY) continue;
			(hazard ? out.hazards : out.solids).push_back(obj);
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
			for (auto obj : obs.hazards) {
				if (hitsHazard(r, obj)) return false;
			}
		}
		return true;
	}

	// Returns true if the death was avoided by shifting the input's timing.
	bool tryCorrect(PlayLayer* layer, PlayerObject* player, GameObject* killer) {
		if (!active() || !player || !killer) return false;
		if (killer == layer->m_anticheatSpike) return false;
		if (layer->m_isPlatformer || player->m_isSideways) return false;

		auto s = stateFor(layer, player);
		if (!s) return false;

		// Already moved the player this tick; GD is still finishing checks against the old spot.
		if (s->correctedStep == g_step) return true;
		if (s->inputStep < 0 || s->correctedThisInput) return false;

		long long since = g_step - s->inputStep;
		if (since < 0 || since > kDeathWindow) return false;

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

				CCPoint pos = player->m_position;
				pos.y += static_cast<float>(shift);
				player->m_position = pos;
				player->setPosition(pos);
				s->prev.y += static_cast<float>(shift);
				s->correctedThisInput = true;
				s->correctedStep = g_step;
				log::info(
					"[InputBuffer] Trajectory fix ({}): input treated as {} tick(s) {} (shift {:.2f})",
					wave ? "wave" : "other", k, dir == 1 ? "earlier" : "later", shift
				);
				return true;
			}
		}

		s->correctedThisInput = true;
		return false;
	}
}

class $modify(BufferPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
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
		updateIndicator(this);
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
		if (!typeinfo_cast<PlayLayer*>(this)) {
			return;
		}
		trackMovement(m_player1, g_states[0]);
		trackMovement(m_player2, g_states[1]);
		++g_step;
	}
};

class $modify(BufferPlayer, PlayerObject) {
	bool pushButton(PlayerButton button) {
		noteInput(this, button);
		return PlayerObject::pushButton(button);
	}

	bool releaseButton(PlayerButton button) {
		noteInput(this, button);
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
