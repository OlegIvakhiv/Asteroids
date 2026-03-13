#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <SFML/Graphics.hpp>
#include "EntityManager.hpp"
#include "Systems.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <vector>
#include <random>

int score = 0;


int main() {

	b2WorldDef worldDef = b2DefaultWorldDef();
	worldDef.gravity = { 0.0f, 0.0f };
	b2WorldId worldId = b2CreateWorld(&worldDef);

	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::math);
	InputRegistry::init();

	try {
		lua.script_file("scripts/player.lua");
		lua.script_file("scripts/asteroids.lua");
		lua.script_file("scripts/enemy.lua");
	}
	catch (const std::exception& e) {
		std::cerr << "Could not load Lua script: " << e.what() << std::endl;
	}


	sf::RenderWindow window(sf::VideoMode({ 1920, 1080 }), "Modular Space Engine");
	window.setFramerateLimit(60);
	EntityManager em;
	size_t playerID = em.createPlayer({ 640.f, 360.f }, lua, worldId);

	em.initBackground(window.getSize(), 400);

	sf::Clock clock;

	sf::Font font;
	if (!font.openFromFile("assets/upheavtt.ttf")) {
		std::cout << "Error loading font!" << std::endl;
	}

	sf::Text scoreText(font);
	scoreText.setCharacterSize(30);
	scoreText.setFillColor(sf::Color::Yellow);
	scoreText.setPosition({ 20.f, 60.f });

	// sf::View camera(sf::FloatRect({ 0.f, 0.f }, { 1280.f, 720.f }));
	sf::View gameView = window.getDefaultView();



	if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F5)) {
		lua.script_file("scripts/enemy.lua");
		std::cout << "AI Script Reloaded!" << std::endl;
	}
	

	while (window.isOpen()) {
		while (const std::optional event = window.pollEvent()) {
			if (event->is<sf::Event::Closed>()) {
				window.close();
			}
			if (event->is<sf::Event::FocusLost>()) {
				// Гра неактивна (згорнута)
			}
		}

		float dt = clock.restart().asSeconds();

		sol::table binds = lua["key_bindings"];

		// 1. ОНОВЛЕННЯ ЛОГІКИ
		InputSystem::update(em, playerID, dt, window, lua);
		PhysicsSystem::update(em, worldId, dt);
		PhysicsSystem::cleanup(em, worldId, playerID, lua);
		EnemySystem::update(em, lua, worldId, playerID);

		DamageSystem::update(em, worldId, playerID, dt, lua);
		WeaponSystem::update(em, worldId, playerID, dt, lua);

		ParticleSystem::update(em, dt);

		AISystem::update(em, playerID, dt, lua);

		// Оновлення зірок 
		// Отримуємо реальну лінійну швидкість з Box2D
		auto& playerPhysics = em.physics[playerID];
		b2Vec2 b2Vel = b2Body_GetLinearVelocity(playerPhysics.bodyId);
		sf::Vector2f playerVel(b2Vel.x * SCALE, b2Vel.y * SCALE);


		BackgroundSystem::update(em, playerVel, window.getSize(), dt);

		// 2. РЕНДЕР
		window.clear(sf::Color(10, 10, 15));

		scoreText.setString("Score: " + std::to_string(em.totalScore));

		window.setView(window.getDefaultView());
		window.draw(scoreText);

		// 3. МАЛЮЄМО ФОН (Зірки)

		window.setView(window.getDefaultView());

		sf::RectangleShape healthBarBack({ 200.f, 20.f });
		healthBarBack.setPosition({ 20.f, 20.f });
		healthBarBack.setFillColor(sf::Color(50, 50, 50)); // Сірий фон

		auto& hp = em.healths[playerID];

		float displayHp = std::max(0.f, std::min(hp.currentHp, hp.maxHp));

		float barWidth_HP = (displayHp / hp.maxHp) * 200.f;

		sf::RectangleShape healthBarFront({ barWidth_HP, 20.f });
		healthBarFront.setPosition({ 20.f, 20.f });
		healthBarFront.setFillColor(sf::Color::Red);

		window.draw(healthBarBack);
		window.draw(healthBarFront);

		auto& tf = em.transforms[playerID];

		// 2. Фон для смужки палива (сірий)
		sf::RectangleShape energyBarBack({ 200.f, 10.f });
		energyBarBack.setPosition({ 20.f, 45.f });
		energyBarBack.setFillColor(sf::Color(50, 50, 50));

		float displayEnergy = std::max(0.f, std::min(tf.energyDrive, tf.maxEnergyDrive));
		float barWidth_energy = (displayEnergy / tf.maxEnergyDrive) * 200.f;

		sf::RectangleShape energyBarFront({ barWidth_energy, 10.f });
		energyBarFront.setPosition({ 20.f, 45.f });

		// Логіка кольорів
		if (tf.overheatTimer > 0) {
			// Якщо ПЕРЕГРІВ - смужка червона або помаранчева
			energyBarFront.setFillColor(sf::Color(255, 69, 0)); // OrangeRed
		}
		else {
			// Звичайний стан - блакитна
			energyBarFront.setFillColor(sf::Color(0, 191, 255)); // DeepSkyBlue
		}

		window.draw(energyBarBack);
		window.draw(energyBarFront);

		// 4. МАЛЮЄМО ГРУ (Гравця і астероїди)
		sf::Vector2f playerPos = em.transforms[playerID].position;

		gameView.setCenter(playerPos);
		window.setView(gameView);

		ParticleSystem::draw(window, em);

		window.setView(window.getDefaultView());
		BackgroundSystem::draw(window, em);

		RenderSystem::draw(em, window, lua, playerID);

		window.display();
	}
	return 0;
};
