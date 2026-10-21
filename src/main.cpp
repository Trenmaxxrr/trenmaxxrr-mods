#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

// Starter check: a button on the main menu that confirms the mod loaded.
// Each actual hack lives in its own file under src/hacks/.
class $modify(TrenMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) {
			return false;
		}

		if (!Mod::get()->getSettingValue<bool>("show-menu-button")) {
			return true;
		}

		auto menu = this->getChildByID("bottom-menu");
		if (!menu) {
			return true;
		}

		auto button = CCMenuItemSpriteExtra::create(
			CCSprite::createWithSpriteFrameName("GJ_likeBtn_001.png"),
			this,
			menu_selector(TrenMenuLayer::onHacksButton)
		);
		button->setID("hacks-button"_spr);
		menu->addChild(button);
		menu->updateLayout();

		return true;
	}

	void onHacksButton(CCObject*) {
		FLAlertLayer::create(
			"Trenmaxxrr Hacks",
			"It works! New hacks will show up as toggles in this mod's settings.",
			"OK"
		)->show();
	}
};
