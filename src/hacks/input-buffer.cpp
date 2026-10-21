// Input buffer: shifts the timing of your clicks/releases (never your player)
// so you don't crash, by up to the current stage's tick count.
//
// Predictions use a hidden copy of the player run through GD's own physics and
// collision code (the same method trajectory mods use). While simulating, the
// hidden player only collides with blocks, slopes and hazards; anything that
// would set off orbs, pads, portals, triggers or effects is blocked.
//
// What it does:
// - Hold back: when you click/release and doing it now would crash you soon,
//   but doing it up to N ticks later wouldn't, the input is delayed.
// - Switch early (wave and ship only): when you're about to crash within N
//   ticks and switching now would save you, it switches now and waits up to
//   N ticks for your real input. If yours doesn't come, that counts as the
//   crash you would have had (just switching back would keep the dodge).
//   One-shot modes (cube jump, ball/UFO/spider/swing) don't get this, since
//   a jump can't be taken back; that would be the mod playing for you.
// - If the hidden player touches an orb, pad or portal, the prediction isn't
//   trustworthy, so your input goes through untouched.
// - Moving hazards are tracked tick by tick, so predictions use where they'll
//   be, not where they are now.
//
// F6 (in a level) cycles: OFF -> 2 -> 4 -> 8 -> 16 ticks (wave capped at 8) -> OFF.
// It starts OFF every session. While it's on, an on-screen label always shows
// the current stage; that label can't be turned off.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>

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
	// How far ahead (ticks) a prediction runs.
	constexpr int kHorizon = 48;
	// A crash within (stage ticks + this) counts as "this input's fault".
	constexpr int kDangerExtra = 6;
	// Objects that jump further than this in one tick were teleported, not moved.
	constexpr float kMaxObjectSpeed = 40.f;
	// How often (in ticks) the list of objects near the player is rebuilt.
	constexpr long long kWatchRefresh = 10;

	int g_stage = 0;
	long long g_step = 0;

	// ---- input state --------------------------------------------------------

	struct QueuedInput {
		long long at;
		bool down;
	};

	struct InputState {
		bool physHeld = false;   // what your finger is doing
		bool virtHeld = false;   // what the game has been told
		// Held-back inputs, in order. Every input while something is held back
		// gets the same delay, so quick taps stay taps instead of being dropped.
		std::vector<QueuedInput> queue;
		long long queueDelay = 0;
		bool advanced = false;   // switched early, waiting for your real input
		long long advanceDeadline = -1;
	};
	InputState g_inputs[2];

	// Label state, so it can be re-shown every tick if anything hides it.
	bool g_labelOn = false;
	int g_labelStage = -1;

	// Set while the mod itself sends an input, so the hooks let it straight through.
	bool g_applying = false;

	// ---- simulation state ---------------------------------------------------

	PlayerObject* g_ghost = nullptr;
	bool g_simulating = false;
	bool g_simDead = false;
	bool g_simUncertain = false;
	float g_dt = 0.f;

	// ---- moving-object tracking ---------------------------------------------

	struct ObjectTrack {
		CCPoint pos;
		CCPoint vel;
		long long step = -1;
	};
	std::unordered_map<GameObject*, ObjectTrack> g_tracks;
	std::vector<GameObject*> g_watch;
	long long g_watchBuiltStep = -1;

	OBB2D* g_playerBox = nullptr;

	// ---- small helpers ------------------------------------------------------

	bool enabled() {
		return Mod::get()->getSettingValue<bool>("input-buffer");
	}

	bool active() {
		return g_stage != 0 && enabled();
	}

	int ticksFor(PlayerObject* player) {
		return player && player->m_isDart ? kStageTicksWave[g_stage] : kStageTicksAll[g_stage];
	}

	bool holdMode(PlayerObject* p) {
		return p->m_isDart || p->m_isShip;
	}

	bool oneShotMode(PlayerObject* p) {
		return p->m_isBall || p->m_isBird || p->m_isSpider || p->m_isSwing;
	}

	int indexOf(GJBaseGameLayer* layer, PlayerObject* p) {
		if (!layer || !p) return -1;
		if (p == layer->m_player1) return 0;
		if (p == layer->m_player2) return 1;
		return -1;
	}

	bool canBuffer(PlayLayer* layer, PlayerObject* p) {
		return layer && p && !layer->m_isPlatformer && !p->m_isSideways && !p->m_isDead;
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
		g_labelOn = on;
		g_labelStage = g_stage;
		if (on) {
			std::string text = "INPUT BUFFER: " + stageText();
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			label->setString(text.c_str());
		}
	}

	// Send an input to a real player, bypassing the buffer.
	void sendInput(PlayerObject* p, bool down) {
		g_applying = true;
		if (down) p->pushButton(PlayerButton::Jump);
		else p->releaseButton(PlayerButton::Jump);
		g_applying = false;
	}

	void resetInputs() {
		for (auto& s : g_inputs) s = InputState{};
	}

	// The label must be showing whenever the buffer is on. Checked every tick,
	// so turning the setting on mid-level (or anything hiding the label) can't
	// leave the buffer running without it.
	void syncIndicator(PlayLayer* layer) {
		bool on = active();
		if (on != g_labelOn || (on && g_labelStage != g_stage)) {
			updateIndicator(layer);
			return;
		}
		if (on) {
			auto label = layer->getChildByID("input-buffer-label"_spr);
			if (!label || !label->isVisible()) updateIndicator(layer);
		}
	}

	// Send every held-back input now, in order.
	void flushQueue(PlayerObject* p, InputState& s) {
		for (auto const& input : s.queue) {
			s.virtHeld = input.down;
			sendInput(p, input.down);
		}
		s.queue.clear();
	}

	// An early switch your input never confirmed. Switching back alone would
	// keep the dodge it already gave you, so it counts as the crash you'd have had.
	void failEarlySwitch(PlayLayer* layer, PlayerObject* p, InputState& s) {
		s.advanced = false;
		s.virtHeld = s.physHeld;
		sendInput(p, s.physHeld);
		if (!p->m_isDead) {
			log::info("[InputBuffer] Early switch not confirmed by your input; counting it as a crash");
			layer->destroyPlayer(p, nullptr);
		}
	}

	// ---- moving-object tracking ---------------------------------------------

	void clearTracks() {
		g_tracks.clear();
		g_watch.clear();
		g_watchBuiltStep = -1;
	}

	bool isHazardType(GameObjectType type) {
		return type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard;
	}

	// Rebuild the list of hazards/blocks near either player. Done every few ticks,
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
			if (!obj) continue;
			auto type = obj->m_objectType;
			if (!isHazardType(type) && type != GameObjectType::Solid) continue;
			auto pos = obj->getPosition();
			for (auto const& area : areas) {
				if (area.containsPoint(pos)) {
					g_watch.push_back(obj);
					break;
				}
			}
		}

		for (auto it = g_tracks.begin(); it != g_tracks.end();) {
			if (it->second.step < g_step - kWatchRefresh * 2) it = g_tracks.erase(it);
			else ++it;
		}
	}

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
				track.vel = CCPoint{0.f, 0.f};
			}
			track.pos = center;
			track.step = g_step;
		}
	}

	bool isMoving(GameObject* obj, CCPoint* velOut = nullptr) {
		auto it = g_tracks.find(obj);
		if (it == g_tracks.end() || it->second.step < g_step - 1) return false;
		auto vel = it->second.vel;
		if (std::abs(vel.x) < 0.01f && std::abs(vel.y) < 0.01f) return false;
		if (velOut) *velOut = vel;
		return true;
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

	// ---- simulation ---------------------------------------------------------

	bool isInteractiveType(GameObjectType type) {
		switch (type) {
			case GameObjectType::InverseGravityPortal:
			case GameObjectType::NormalGravityPortal:
			case GameObjectType::ShipPortal:
			case GameObjectType::CubePortal:
			case GameObjectType::YellowJumpPad:
			case GameObjectType::PinkJumpPad:
			case GameObjectType::GravityPad:
			case GameObjectType::YellowJumpRing:
			case GameObjectType::PinkJumpRing:
			case GameObjectType::GravityRing:
			case GameObjectType::InverseMirrorPortal:
			case GameObjectType::NormalMirrorPortal:
			case GameObjectType::BallPortal:
			case GameObjectType::RegularSizePortal:
			case GameObjectType::MiniSizePortal:
			case GameObjectType::UfoPortal:
			case GameObjectType::Modifier:
			case GameObjectType::DualPortal:
			case GameObjectType::SoloPortal:
			case GameObjectType::WavePortal:
			case GameObjectType::RobotPortal:
			case GameObjectType::TeleportPortal:
			case GameObjectType::GreenRing:
			case GameObjectType::DropRing:
			case GameObjectType::SpiderPortal:
			case GameObjectType::RedJumpPad:
			case GameObjectType::RedJumpRing:
			case GameObjectType::CustomRing:
			case GameObjectType::DashRing:
			case GameObjectType::GravityDashRing:
			case GameObjectType::SwingPortal:
			case GameObjectType::GravityTogglePortal:
			case GameObjectType::SpiderOrb:
			case GameObjectType::SpiderPad:
			case GameObjectType::TeleportOrb:
				return true;
			default:
				return false;
		}
	}

	void releaseGhost() {
		if (!g_ghost) return;
		g_ghost->removeFromParentAndCleanup(true);
		g_ghost->release();
		g_ghost = nullptr;
	}

	void ensureGhost(PlayLayer* layer) {
		if (g_ghost || !layer || !layer->m_objectLayer) return;
		g_ghost = PlayerObject::create(1, 1, layer, layer, true);
		if (!g_ghost) return;
		g_ghost->retain();
		g_ghost->setPosition({0.f, 105.f});
		g_ghost->setVisible(false);
		layer->m_objectLayer->addChild(g_ghost);
	}

	void resetCollisionLog(PlayerObject* p) {
		p->m_collisionLogTop->removeAllObjects();
		p->m_collisionLogBottom->removeAllObjects();
		p->m_collisionLogLeft->removeAllObjects();
		p->m_collisionLogRight->removeAllObjects();
	}

	struct SimResult {
		int survived;      // ticks before crashing (== horizon if it didn't)
		bool uncertain;    // touched an orb/pad/portal, so don't trust it
	};

	// Run the hidden player from the real player's current state.
	// startHeld: input state at the start. flipAt: tick at which the input
	// switches (-1 = never). Returns how long it survives.
	SimResult simulate(PlayLayer* layer, PlayerObject* real, bool startHeld, int flipAt, int horizon) {
		if (!g_ghost || g_dt <= 0.f) return {horizon, true};

		// Moving hazards are checked separately using their predicted positions.
		struct MovingHazard {
			GameObject* obj;
			CCPoint vel;
		};
		std::vector<MovingHazard> moving;
		for (auto obj : g_watch) {
			if (!isHazardType(obj->m_objectType)) continue;
			if (obj->m_isGroupDisabled || obj->m_isNoTouch) continue;
			CCPoint vel;
			if (isMoving(obj, &vel)) moving.push_back({obj, vel});
		}

		g_simulating = true;
		g_simDead = false;
		g_simUncertain = false;

		auto ghost = g_ghost;
		ghost->copyAttributes(real);
		ghost->m_gravityMod = real->m_gravityMod;
		ghost->m_isOnGround = real->m_isOnGround;
		ghost->setVisible(false);
		// In one-shot modes a press is an action, not a state, so don't replay it.
		if (startHeld && !oneShotMode(real)) ghost->pushButton(PlayerButton::Jump);

		int survived = horizon;
		for (int i = 0; i < horizon; ++i) {
			if (i == flipAt) {
				if (startHeld) ghost->releaseButton(PlayerButton::Jump);
				else ghost->pushButton(PlayerButton::Jump);
			}
			ghost->update(g_dt);
			resetCollisionLog(ghost);
			layer->checkCollisions(ghost, g_dt, false);

			if (!g_simDead && !moving.empty()) {
				auto const& rect = ghost->getObjectRect();
				for (auto const& hazard : moving) {
					// Instead of moving the hazard forward, move the player back the same amount.
					CCRect relative = rect;
					relative.origin.x -= hazard.vel.x * (i + 1);
					relative.origin.y -= hazard.vel.y * (i + 1);
					if (hitsHazard(relative, hazard.obj)) {
						g_simDead = true;
						break;
					}
				}
			}

			if (g_simDead) {
				survived = i;
				break;
			}
		}

		bool uncertain = g_simUncertain;
		g_simulating = false;
		g_simDead = false;
		g_simUncertain = false;
		return {survived, uncertain};
	}

	// ---- decisions ----------------------------------------------------------

	// Your real input arrived. Returns true if it's being held back.
	bool onPhysicalInput(PlayerObject* p, bool down) {
		auto layer = PlayLayer::get();
		int idx = indexOf(layer, p);
		if (idx < 0) return false;
		auto& s = g_inputs[idx];
		s.physHeld = down;

		// The mod already switched early; this is your input catching up.
		if (s.advanced && down == s.virtHeld) {
			s.advanced = false;
			log::info("[InputBuffer] Your input caught up with an early switch");
			return true;
		}

		if (!active() || !canBuffer(layer, p)) {
			// Anything still held back goes through first, so nothing is dropped or reordered.
			if (!s.queue.empty()) flushQueue(p, s);
			s.virtHeld = down;
			return false;
		}

		// Something is already held back: this input gets the same delay, so a
		// quick tap stays a tap instead of being swallowed.
		if (!s.queue.empty()) {
			s.queue.push_back({g_step + s.queueDelay, down});
			return true;
		}

		// Nothing changes for the game (e.g. GD re-sending a hold); let it through.
		if (down == s.virtHeld) return false;

		// Releases don't matter in one-shot modes.
		if (!down && oneShotMode(p)) {
			s.virtHeld = down;
			return false;
		}

		ensureGhost(layer);
		int n = ticksFor(p);
		int danger = n + kDangerExtra;

		auto now = simulate(layer, p, s.virtHeld, 0, kHorizon);
		if (now.uncertain || now.survived >= danger) {
			s.virtHeld = down;
			return false;
		}

		for (int k = 1; k <= n; ++k) {
			auto later = simulate(layer, p, s.virtHeld, k, kHorizon);
			if (later.uncertain) break;
			if (later.survived >= std::min(kHorizon, k + danger)) {
				s.queue.push_back({g_step + k, down});
				s.queueDelay = k;
				log::info(
					"[InputBuffer] {} held back {} tick(s) (now: crash in {}, later: {})",
					down ? "Click" : "Release", k, now.survived, later.survived
				);
				return true;
			}
		}

		// Nothing in range helps; your input goes through as-is.
		s.virtHeld = down;
		return false;
	}

	// Called at the start of every tick, before GD processes it.
	void beforeTick(PlayLayer* layer, PlayerObject* p, InputState& s) {
		if (!p) return;

		// Already dead: nothing to send; start clean next attempt.
		if (p->m_isDead) {
			s.queue.clear();
			s.advanced = false;
			s.virtHeld = s.physHeld;
			return;
		}

		// Buffer turned off (or not usable here): make the game match your finger.
		// An unconfirmed early switch still counts as a crash, so turning the
		// buffer off mid-switch can't be used to keep the dodge.
		if (!active() || !canBuffer(layer, p)) {
			if (!s.queue.empty()) flushQueue(p, s);
			if (s.advanced) {
				failEarlySwitch(layer, p, s);
			} else if (s.virtHeld != s.physHeld) {
				s.virtHeld = s.physHeld;
				sendInput(p, s.physHeld);
			}
			return;
		}

		// Send held-back inputs that are due, in order.
		while (!s.queue.empty() && s.queue.front().at <= g_step) {
			auto input = s.queue.front();
			s.queue.erase(s.queue.begin());
			s.virtHeld = input.down;
			sendInput(p, input.down);
		}
		if (!s.queue.empty()) return;

		if (s.advanced) {
			if (g_step >= s.advanceDeadline) failEarlySwitch(layer, p, s);
			return;
		}

		// Switch early: only for wave/ship, only when in sync with your finger.
		if (s.virtHeld != s.physHeld || !holdMode(p)) return;

		int n = ticksFor(p);
		ensureGhost(layer);
		auto keep = simulate(layer, p, s.virtHeld, -1, n + 1);
		if (keep.uncertain || keep.survived > n) return;

		auto flip = simulate(layer, p, s.virtHeld, 0, kHorizon);
		if (flip.uncertain || flip.survived < std::min(kHorizon, n + kDangerExtra)) return;

		s.advanced = true;
		s.advanceDeadline = g_step + n;
		s.virtHeld = !s.virtHeld;
		sendInput(p, s.virtHeld);
		log::info(
			"[InputBuffer] Early {} (crash in {} otherwise)",
			s.virtHeld ? "click" : "release", keep.survived
		);
	}
}

// ---- hooks ------------------------------------------------------------------

class $modify(BufferPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		releaseGhost();
		clearTracks();
		resetInputs();
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
			return false;
		}
		if (active()) ensureGhost(this);
		updateIndicator(this);
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		resetInputs();
		clearTracks();
		updateIndicator(this);
	}

	void onQuit() {
		releaseGhost();
		clearTracks();
		resetInputs();
		PlayLayer::onQuit();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		// Only the hidden player's crashes are caught. The mod never stops a real death.
		if (g_simulating && player == g_ghost) {
			g_simDead = true;
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void playEndAnimationToPos(CCPoint position) {
		if (g_simulating) return;
		PlayLayer::playEndAnimationToPos(position);
	}

	void levelComplete() {
		if (g_simulating) return;
		PlayLayer::levelComplete();
	}
};

class $modify(BufferGameLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto playLayer = typeinfo_cast<PlayLayer*>(this);
		if (playLayer) {
			syncIndicator(playLayer);
			beforeTick(playLayer, m_player1, g_inputs[0]);
			beforeTick(playLayer, m_player2, g_inputs[1]);
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		if (playLayer) {
			updateTracks(playLayer);
			++g_step;
		}
	}

	// While simulating, the hidden player only collides with blocks, slopes and
	// still hazards (moving ones are checked separately). Touching anything that
	// would do something (orbs, pads, portals) marks the prediction as unreliable.
	void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
		if (!g_simulating) {
			GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
			return;
		}
		gd::vector<GameObject*> kept;
		auto const& playerRect = player->getObjectRect();
		for (int i = 0; i < objectCount; ++i) {
			GameObject* obj = objects->at(i);
			if (!obj) continue;
			auto type = obj->m_objectType;
			if (type == GameObjectType::Solid || type == GameObjectType::Slope) {
				kept.push_back(obj);
			} else if (isHazardType(type)) {
				if (!isMoving(obj)) kept.push_back(obj);
			} else if (isInteractiveType(type) && !obj->m_isGroupDisabled) {
				if (obj->getObjectRect().intersectsRect(playerRect)) g_simUncertain = true;
			}
		}
		GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
	}

	// The hidden player's jumps and landings must not set off the level's event triggers.
	void gameEventTriggered(GJGameEvent event, int material, int playerID) {
		if (g_simulating) return;
		GJBaseGameLayer::gameEventTriggered(event, material, playerID);
	}

	bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
		if (g_simulating) return false;
		return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
	}

	void playerTouchedRing(PlayerObject* player, RingObject* ring) {
		if (g_simulating) return;
		GJBaseGameLayer::playerTouchedRing(player, ring);
	}

	void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
		if (g_simulating) {
			// Flip only the hidden player, with no effects and nothing else touched.
			static bool inFlip = false;
			if (player == g_ghost && !inFlip) {
				inFlip = true;
				player->flipGravity(flip, true);
				inFlip = false;
			}
			return;
		}
		GJBaseGameLayer::flipGravity(player, flip, noEffects);
	}
};

class $modify(BufferPlayer, PlayerObject) {
	void update(float dt) {
		PlayerObject::update(dt);
		if (!g_simulating && this != g_ghost) {
			auto layer = PlayLayer::get();
			if (layer && indexOf(layer, this) >= 0) g_dt = dt;
		}
	}

	bool pushButton(PlayerButton button) {
		if (button == PlayerButton::Jump && !g_applying && !g_simulating && this != g_ghost) {
			if (onPhysicalInput(this, true)) return true;
		}
		return PlayerObject::pushButton(button);
	}

	bool releaseButton(PlayerButton button) {
		if (button == PlayerButton::Jump && !g_applying && !g_simulating && this != g_ghost) {
			if (onPhysicalInput(this, false)) return true;
		}
		return PlayerObject::releaseButton(button);
	}

	void playSpiderDashEffect(CCPoint from, CCPoint to) {
		if (g_simulating) return;
		PlayerObject::playSpiderDashEffect(from, to);
	}

	void incrementJumps() {
		if (g_simulating) return;
		PlayerObject::incrementJumps();
	}

	void ringJump(RingObject* ring, bool skipCheck) {
		if (g_simulating) return;
		PlayerObject::ringJump(ring, skipCheck);
	}
};

class $modify(BufferHardStreak, HardStreak) {
	void addPoint(CCPoint point) {
		if (g_simulating) return;
		HardStreak::addPoint(point);
	}
};

class $modify(BufferEffectObject, EffectGameObject) {
	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		if (g_simulating) return;
		EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
	}
};

class $modify(BufferGameObject, GameObject) {
	void playShineEffect() {
		if (g_simulating) return;
		GameObject::playShineEffect();
	}
};

class $modify(BufferKeys, CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double time) {
		if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat && enabled()) {
			if (auto layer = PlayLayer::get()) {
				g_stage = (g_stage + 1) % kStageCount;
				// Off means nothing of the buffer stays in the level.
				if (active()) ensureGhost(layer);
				else {
					releaseGhost();
					clearTracks();
				}
				updateIndicator(layer);
				auto text = "Input buffer: " + stageText();
				log::info("[InputBuffer] F6 Cycle -> {}", text);
				Notification::create(text.c_str(), NotificationIcon::Info, 1.f)->show();
			}
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, time);
	}
};
