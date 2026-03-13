#pragma once

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include "EntityManager.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <map>
#include <random>
#include <string>


class PhysicsSystem {
public:
	static void update(EntityManager& em, b2WorldId worldId, float dt) {
		// 1. Крок симуляції - 60 разів на секунду
		float timeStep = 1.0f / 60.0f;
		int subStepCount = 6;
		b2World_Step(worldId, dt, subStepCount);

		const float screenWidth = 1280.f;
		const float screenHeight = 720.f;
		const float SCALE = 30.f;

		// 2. Синхронізація: копіюємо дані з Box2D в наш Transform
		for (size_t i = 0; i < em.physics.size(); ++i) {
			b2BodyId bodyId = em.physics[i].bodyId;

			// Отримуємо тип назад з void*
			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);

			// 1. Позиція (однаково для всіх)
			b2Vec2 pos = b2Body_GetPosition(bodyId);
			em.transforms[i].position = { pos.x * SCALE, pos.y * SCALE };

			// 2. Логіка повороту
			if (type == BodyType::Bullet) {
				// ТІЛЬКИ кулі рівняємо за швидкістю
				b2Vec2 vel = b2Body_GetLinearVelocity(bodyId);
				if (std::sqrt(vel.x * vel.x + vel.y * vel.y) > 0.1f) {
					float angleRad = std::atan2(vel.y, vel.x) + (3.14159f / 2.f);
					em.transforms[i].rotation = angleRad * 180.f / 3.14159f;
				}
			}
			else if (type == BodyType::Asteroid) {
				// Астероїди беруть поворот з фізики (вони крутяться при ударах)
				b2Rot rot = b2Body_GetRotation(bodyId);
				em.transforms[i].rotation = b2Rot_GetAngle(rot) * 180.f / 3.14159f;
			}
			else if (type == BodyType::Player) {
				// Гравець: ігноруємо фізичний поворот Box2D.
			}
		}
	}



	static void cleanup(EntityManager& em, b2WorldId worldId, size_t& playerID, sol::state& lua) {
		b2Vec2 playerPos = b2Body_GetPosition(em.physics[playerID].bodyId);

		// Радіус для астероїдів з asteroids.lua
		float astDesRadius = lua["spawn_settings"]["despawn_radius"].get_or(100.0f);
		float astDesRadiusSq = astDesRadius * astDesRadius;

		// Радіус для ворогів з Enemy.lua
		float enemyDesRadius = lua["enemy_config"]["despawn_radius"].get_or(250.0f);
		float enemyDesRadiusSq = enemyDesRadius * enemyDesRadius;

		for (size_t i = em.physics.size(); i-- > 0; ) {
			if (i == playerID) continue;

			b2BodyId bodyId = em.physics[i].bodyId;
			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);

			// Кулі не чіпаємо (у них своя логіка lifetime)
			if (type == BodyType::Bullet) continue;

			b2Vec2 pos = b2Body_GetPosition(bodyId);
			float dx = pos.x - playerPos.x;
			float dy = pos.y - playerPos.y;
			float distSq = dx * dx + dy * dy;

			bool shouldDestroy = false;

			if (type == BodyType::Asteroid) {
				if (distSq > astDesRadiusSq) shouldDestroy = true;
			}
			else if (type == BodyType::Enemy) {
				if (distSq > enemyDesRadiusSq) shouldDestroy = true;
			}

			if (shouldDestroy) {
				em.destroyEntity(i);
				if (i < playerID) playerID--;
			}
		}
	}
};

class RenderSystem {
public:
	static void draw(EntityManager& em, sf::RenderWindow& window, sol::state& lua, size_t playerID) { // Додали lua

		auto& playerTf = em.transforms[playerID];

		// 1. Оновлюємо камеру, щоб вона слідувала за гравцем
		sf::View view = window.getView();
		view.setCenter(playerTf.position);
		window.setView(view);

		for (size_t i = 0; i < em.renders.size(); ++i) {
			auto& tf = em.transforms[i];
			auto& rd = em.renders[i];
			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);

			rd.shape.setPosition(tf.position);
			rd.shape.setRotation(sf::degrees(tf.rotation));
			window.draw(rd.shape);

			if (type == BodyType::Player) {
				if (tf.dashCooldown > (tf.dashMaxCooldown - 0.15f)) {
					// Колір ривка
					sol::table flash = lua["dash_flash_color"];
					rd.shape.setFillColor(sf::Color(
						flash["r"].get_or(100),
						flash["g"].get_or(255),
						flash["b"].get_or(255),
						flash["a"].get_or(200)
					));
				}
				else {
					// Звичайний колір гравця
					sol::table clr = lua["color"];
					rd.shape.setFillColor(sf::Color(clr["r"], clr["g"], clr["b"]));
				}
			}
			else if (type == BodyType::Enemy) {
				sol::table clr = lua["enemy_config"]["color"];
				rd.shape.setFillColor(sf::Color(clr["r"], clr["g"], clr["b"]));
			}

			// 3. МАЛЮВАННЯ
			rd.shape.setPosition(tf.position);
			rd.shape.setRotation(sf::degrees(tf.rotation));
			window.draw(rd.shape);
		}
	}
};

class DamageSystem {
public:
	static void update(EntityManager& em, b2WorldId worldId, size_t playerID, float dt, sol::state& lua) {
		auto& playerHp = em.healths[playerID];

		// 1. Таймери невразливості
		if (playerHp.invulTimer > 0) playerHp.invulTimer -= dt;
		if (playerHp.cheapInvulTimer > 0) playerHp.cheapInvulTimer -= dt;

		// 2. Події зіткнень
		b2ContactEvents events = b2World_GetContactEvents(worldId);
		b2BodyId playerBody = em.physics[playerID].bodyId;

		for (int i = 0; i < events.beginCount; ++i) {
			b2ContactBeginTouchEvent* event = events.beginEvents + i;

			b2BodyId bodyA = b2Shape_GetBody(event->shapeIdA);
			b2BodyId bodyB = b2Shape_GetBody(event->shapeIdB);

			BodyType typeA = (BodyType)(uintptr_t)b2Body_GetUserData(bodyA);
			BodyType typeB = (BodyType)(uintptr_t)b2Body_GetUserData(bodyB);

			b2BodyId bulletBody = b2_nullBodyId;
			b2BodyId targetBody = b2_nullBodyId;
			BodyType targetType;

			// Визначаємо, чи є в зіткненні куля і хто її ціль
			if (typeA == BodyType::Bullet) {
				bulletBody = bodyA; targetBody = bodyB; targetType = typeB;
			}
			else if (typeB == BodyType::Bullet) {
				bulletBody = bodyB; targetBody = bodyA; targetType = typeA;
			}

			// Якщо куля влучила в щось валідне
			if (b2Body_IsValid(bulletBody) && b2Body_IsValid(targetBody)) {
				size_t bulletIdx = (size_t)-1;
				size_t targetIdx = (size_t)-1;

				// Один спільний пошук індексів для будь-якої цілі
				for (size_t idx = 0; idx < em.physics.size(); ++idx) {
					if (B2_ID_EQUALS(em.physics[idx].bodyId, bulletBody)) bulletIdx = idx;
					if (B2_ID_EQUALS(em.physics[idx].bodyId, targetBody)) targetIdx = idx;
					if (bulletIdx != (size_t)-1 && targetIdx != (size_t)-1) break;
				}

				if (bulletIdx != (size_t)-1 && targetIdx != (size_t)-1) {
					if (!em.bullets[bulletIdx].markedForDestroy) {
						em.bullets[bulletIdx].markedForDestroy = true;
						sf::Vector2f hitPos = em.transforms[bulletIdx].position;
						sf::Vector2f hitVel = em.transforms[bulletIdx].velocity;

						if (targetType == BodyType::Asteroid) {
							em.healths[targetIdx].currentHp -= 15.0f;
							em.spawnImpact(hitPos, sf::Color(180, 180, 180), hitVel);
							std::cout << "HIT! Asteroid HP: " << em.healths[targetIdx].currentHp << std::endl;
						}
						else if (targetType == BodyType::Enemy) {
							em.healths[targetIdx].currentHp -= 25.0f;
							em.spawnImpact(hitPos, sf::Color::Yellow, hitVel);
							em.spawnExplosion(hitPos, sf::Color::Red, 5, 2.0f);
							std::cout << "[HIT] Pirate HP: " << em.healths[targetIdx].currentHp << std::endl;
						}
					}
				}
				continue; // Куля оброблена, йдемо до наступної події
			}

			// --- ЛОГІКА ГРАВЦЯ (Зіткнення корабля з об'єктами) ---
			bool isPlayerA = B2_ID_EQUALS(bodyA, playerBody);
			bool isPlayerB = B2_ID_EQUALS(bodyB, playerBody);

			if (isPlayerA || isPlayerB) {
				b2Vec2 vA = b2Body_GetLinearVelocity(bodyA);
				b2Vec2 vB = b2Body_GetLinearVelocity(bodyB);
				float relativeSpeed = std::sqrt(std::pow(vA.x - vB.x, 2) + std::pow(vA.y - vB.y, 2));

				if (relativeSpeed > 12.0f && playerHp.invulTimer <= 0) {
					playerHp.currentHp -= 15.0f;
					playerHp.invulTimer = 1.0f;
					std::cout << "BOOM! Damage. HP: " << playerHp.currentHp << std::endl;
				}
				else if (relativeSpeed > 1.5f && playerHp.invulTimer <= 0 && playerHp.cheapInvulTimer <= 0) {
					playerHp.currentHp -= 1.0f;
					playerHp.cheapInvulTimer = 0.2f;
				}
			}
		}

		// 3. ВИДАЛЕННЯ (Мертві астероїди, вороги)
		for (size_t i = em.physics.size(); i-- > 0; ) {
			if (i == playerID) continue;

			b2BodyId bodyId = em.physics[i].bodyId;
			if (!b2Body_IsValid(bodyId)) continue;

			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);
			bool shouldDestroy = false;

			if (em.healths[i].currentHp <= 0) {
				shouldDestroy = true;
				sf::Vector2f deathPos = em.transforms[i].position;

				if (type == BodyType::Asteroid) {
					int reward = em.scoreRewards[i];
					em.totalScore += reward;
					float pSize = (reward >= 200) ? 5.0f : (reward >= 50 ? 3.0f : 1.5f);
					int pCount = (reward >= 200) ? 40 : (reward >= 50 ? 25 : 15);

					em.spawnExplosion(deathPos, sf::Color(160, 160, 160), pCount, pSize);

					if (reward >= 200) {
						for (int j = 0; j < 3; ++j) spawnChild(em, worldId, lua, deathPos, "MEDIUM");
					}
					else if (reward >= 50) {
						for (int j = 0; j < 2; ++j) spawnChild(em, worldId, lua, deathPos, "SMALL");
					}
				}
				else if (type == BodyType::Enemy) {
					em.totalScore += em.scoreRewards[i];
					em.spawnExplosion(deathPos, sf::Color::Red, 35, 5.0f);
					em.spawnExplosion(deathPos, sf::Color::Yellow, 15, 2.5f);
					std::cout << "PIRATE ELIMINATED! Total Score: " << em.totalScore << std::endl;
				}
			}
			else if (type == BodyType::Bullet && (em.bullets[i].markedForDestroy || em.bullets[i].lifetime <= 0)) {
				shouldDestroy = true;
			}

			if (shouldDestroy) {
				em.destroyEntity(i);
				if (i < playerID) playerID--;
			}
		}
	}


private:
	// Допоміжна функція для створення уламка
	static void spawnChild(EntityManager& em, b2WorldId worldId, sol::state& lua, sf::Vector2f pos, const char* typeKey) {
		sol::table config = lua["asteroid_types"][typeKey];

		// Випадковий напрямок розльоту
		float angle = (rand() % 360) * 3.14159f / 180.f;
		sol::table speedRange = config["speed_range"];
		float speed = speedRange[1].get<float>() + (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

		sf::Vector2f velocity(std::cos(angle) * speed * 0.5f, std::sin(angle) * speed * 0.5f);

		size_t newId = em.createAsteroid(pos, velocity, config["base_size"], config, worldId);

		// Додаємо закручування уламку
		float randomRotation = ((rand() % 200) - 100.f) / 50.f;
		b2Body_SetAngularVelocity(em.physics[newId].bodyId, randomRotation);
	}

};

class InputRegistry {
private:
	static inline std::map<std::string, sf::Keyboard::Key> keyMap;
	static inline std::map<std::string, sf::Mouse::Button> mouseMap;

public:
	static void init() {
		if (!keyMap.empty()) return;

		// 1. АЛФАВІТ (A-Z)
		for (int i = 0; i < 26; ++i) {
			std::string name(1, 'A' + i); // Створює "A", "B", "C"..."Z"
			keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::A) + i);
		}

		// 2. ЦИФРИ (0-9)
		for (int i = 0; i < 10; ++i) {
			std::string name = std::to_string(i);
			keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::Num0) + i);
		}

		// 3. СПЕЦІАЛЬНІ КЛАВІШІ
		keyMap["Space"] = sf::Keyboard::Key::Space;
		keyMap["Enter"] = sf::Keyboard::Key::Enter;
		keyMap["LShift"] = sf::Keyboard::Key::LShift;
		keyMap["RShift"] = sf::Keyboard::Key::RShift;
		keyMap["LControl"] = sf::Keyboard::Key::LControl;
		keyMap["Escape"] = sf::Keyboard::Key::Escape;
		keyMap["Tab"] = sf::Keyboard::Key::Tab;

		// 4. МИША (включаючи бічні кнопки)
		mouseMap["MouseLeft"] = sf::Mouse::Button::Left;
		mouseMap["MouseRight"] = sf::Mouse::Button::Right;
		mouseMap["MouseMiddle"] = sf::Mouse::Button::Middle;
		mouseMap["MouseX1"] = sf::Mouse::Button::Extra1;
		mouseMap["MouseX2"] = sf::Mouse::Button::Extra2;
	}

	static bool isPressed(const std::string& name) {
		if (keyMap.count(name)) return sf::Keyboard::isKeyPressed(keyMap[name]);
		if (mouseMap.count(name)) return sf::Mouse::isButtonPressed(mouseMap[name]);
		return false;
	}
};

class InputSystem {
public:
	// ==========================================
	// 1. ЛОГІКА ГРАВЦЯ
	// ==========================================
	static void update(EntityManager& em, size_t id, float dt, sf::RenderWindow& window, sol::state& lua) {
		auto& tf = em.transforms[id];
		auto& phys = em.physics[id];
		b2BodyId bodyId = phys.bodyId;

		InputRegistry::init(); // Можна тут, але краще один раз в main()

		// --- 1. НАЛАШТУВАННЯ (Lua) ---
		float enginePower = lua["engine_power"].get_or(150.f);
		float rotationSpeed = lua["rotation_speed"].get_or(4.f);

		// Параметри Energy Drive
		float multiplier = lua["sprint_power_multiplier"].get_or(2.5f);
		float drainRate = lua["sprint_drain_speed"].get_or(40.f);
		float regenRate = lua["sprint_regen_speed"].get_or(20.f);
		float penaltyTime = lua["penalty_energy"].get_or(3.0f); // Час покарання за повний розряд

		// Параметри Dash
		float dashVel = lua["dash_velocity"].get_or(40.f);
		float dashCost = lua["dash_energy_cost"].get_or(30.f);
		tf.dashMaxCooldown = lua["dash_max_cooldown"].get_or(1.0f);

		sol::table binds = lua["key_bindings"];

		// --- 2. ТАЙМЕРИ ---
		if (tf.dashCooldown > 0) tf.dashCooldown -= dt;

		// Логіка перегріву (Overheat)
		if (tf.overheatTimer > 0) {
			tf.overheatTimer -= dt;
		}

		// --- 3. ЛОГІКА СПРИНТУ (TURBO) ---
		std::string sprintKey = binds["sprint"].get<std::string>();
		bool wantSprint = InputRegistry::isPressed(sprintKey);

		// Вмикаємо спринт, ТІЛЬКИ якщо: Тиснемо кнопку + Є енергія + Немає перегріву
		if (wantSprint && tf.energyDrive > 0 && tf.overheatTimer <= 0) {
			tf.isTurbo = true;
			tf.energyDrive -= drainRate * dt;

			// Якщо енергія закінчилася під час спринту
			if (tf.energyDrive <= 0) {
				tf.energyDrive = 0;
				tf.overheatTimer = penaltyTime; // БАХ! Перегрів на 5 секунд
				tf.isTurbo = false;
			}
		}
		else {
			tf.isTurbo = false;
		}

		// --- 4. РЕГЕНЕРАЦІЯ ЕНЕРГІЇ ---
		// Відновлюємо, тільки якщо: Не спринтимо + Немає перегріву + Бак не повний
		if (!tf.isTurbo && tf.energyDrive < tf.maxEnergyDrive) {
			tf.energyDrive += regenRate * dt;
			if (tf.energyDrive > tf.maxEnergyDrive) tf.energyDrive = tf.maxEnergyDrive;
		}

		// --- 5. ПОВОРОТ ---
		if (tf.isTurbo) rotationSpeed *= 0.3f; // Важко повертати на форсажі

		b2Vec2 b2Pos = b2Body_GetPosition(bodyId);
		sf::Vector2f currentPos(b2Pos.x * SCALE, b2Pos.y * SCALE);
		sf::Vector2i mousePos = sf::Mouse::getPosition(window);
		sf::Vector2f worldPos = window.mapPixelToCoords(mousePos);

		float targetAngle = std::atan2(worldPos.y - currentPos.y, worldPos.x - currentPos.x) * 180.f / 3.14159f + 90.f;
		float currentRotation = tf.rotation;
		float deltaAngle = targetAngle - currentRotation;

		while (deltaAngle > 180) deltaAngle -= 360;
		while (deltaAngle < -180) deltaAngle += 360;

		tf.rotation += deltaAngle * rotationSpeed * dt;
		float rad = tf.rotation * 3.14159f / 180.f;
		b2Body_SetTransform(bodyId, b2Pos, b2MakeRot(rad));

		// --- 6. РУХ ---
		float dx = 0.f, dy = 0.f;

		// Зчитуємо WASD завжди, щоб знати, куди гравець ХОЧЕ летіти (для ривка)
		if (InputRegistry::isPressed(binds["up"].get<std::string>()))    dy -= 1.f;
		if (InputRegistry::isPressed(binds["down"].get<std::string>()))  dy += 1.f;
		if (InputRegistry::isPressed(binds["left"].get<std::string>()))  dx -= 1.f;
		if (InputRegistry::isPressed(binds["right"].get<std::string>())) dx += 1.f;

		if (tf.isTurbo) {
			// Режим Форсажу: Рух тільки вперед за носом
			float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
			float finalPower = enginePower * multiplier;
			b2Body_ApplyForceToCenter(phys.bodyId, { std::cos(noseRad) * finalPower, std::sin(noseRad) * finalPower }, true);
		}
		else {
			// Звичайний режим
			if (dx != 0 || dy != 0) {
				float length = std::sqrt(dx * dx + dy * dy);
				b2Body_ApplyForceToCenter(phys.bodyId, { (dx / length) * enginePower, (dy / length) * enginePower }, true);
			}
		}

		// --- 7. РИВОК (DASH) ---
		// Умова: Натиснуто + КД ривка пройшов + Є ЕНЕРГІЯ + НЕМАЄ ПЕРЕГРІВУ
		std::string dashKey = binds["dash"].get<std::string>();

		if (InputRegistry::isPressed(dashKey) && tf.dashCooldown <= 0 && tf.overheatTimer <= 0) {
			// Перевіряємо чи вистачає енергії
			if (tf.energyDrive >= dashCost) {

				// 7.1 Виконуємо ривок
				float vx, vy;
				if (dx != 0 || dy != 0) {
					float len = std::sqrt(dx * dx + dy * dy);
					vx = (dx / len) * dashVel;
					vy = (dy / len) * dashVel;
				}
				else {
					float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
					vx = std::cos(noseRad) * dashVel;
					vy = std::sin(noseRad) * dashVel;
				}
				b2Body_SetLinearVelocity(bodyId, { vx, vy });

				// 7.2 Списуємо ресурси
				tf.energyDrive -= dashCost;
				tf.dashCooldown = tf.dashMaxCooldown;

				// 7.3 ПЕРЕВІРКА НА ПЕРЕГРІВ ВІД РИВКА
				// Ми перевіряємо, чи впала енергія до нуля (або нижче) саме після цього ривка
				// Але оскільки ми перевірили if (energy >= cost), вона може стати рівно 0.
				if (tf.energyDrive < 1.0f) { // Якщо залишилось менше 1 одиниці (практично 0)
					tf.energyDrive = 0;
					tf.overheatTimer = penaltyTime; // БАХ! Перегрів на 5 секунд
				}
			}
		}
	}

	// ==========================================
	// 2. ФІЗИКА
	// ==========================================
	static void updatePhysics(EntityManager& em, b2WorldId worldId, float dt) {
		b2World_Step(worldId, dt, 6);

		for (size_t i = 0; i < em.physics.size(); ++i) {
			b2BodyId bodyId = em.physics[i].bodyId;
			b2Vec2 pos = b2Body_GetPosition(bodyId);

			em.transforms[i].position = { pos.x * SCALE, pos.y * SCALE };

			// ВИПРАВЛЕННЯ 3: b2Body_GetAngle замінено на b2Rot_GetAngle
			// Якщо це не гравець (у гравця ми керуємо кутом вручну вище)
			if (em.physics[i].bodyId.index1 != em.physics[0].bodyId.index1) {
				b2Rot rotation = b2Body_GetRotation(bodyId);
				float angleRad = b2Rot_GetAngle(rotation);
				em.transforms[i].rotation = angleRad * 180.f / 3.14159f;
			}
		}
	}
};

class EnemySystem {
private:
	static inline sf::Clock asteroidSpawnClock;
	static inline sf::Clock pirateSpawnClock;

public:
	static void update(EntityManager& em, sol::state& lua, b2WorldId worldId, size_t playerID) {
		auto& playerTf = em.transforms[playerID];

		sol::table astSettings = lua["spawn_settings"];
		float astInterval = astSettings["interval"].get_or(1.0f);
		int maxAstCount = astSettings["max_count"].get_or(40);
		float spawnRadius = astSettings["spawn_radius"].get_or(1500.f);

		// Рахуємо поточну кількість об'єктів
		int currentAsteroids = 0;
		int currentPirates = 0;
		for (const auto& p : em.physics) {
			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(p.bodyId);
			if (type == BodyType::Asteroid) currentAsteroids++;
			if (type == BodyType::Enemy) currentPirates++;
		}

		// --- 1. СПАВН АСТЕРОЇДІВ ---
		if (asteroidSpawnClock.getElapsedTime().asSeconds() > astInterval && currentAsteroids < maxAstCount) {
			sol::table types = lua["asteroid_types"];
			const char* typeKeys[] = { "SMALL","MEDIUM", "LARGE" };
			sol::table config = types[typeKeys[rand() % 3]];

			float angle = (rand() % 360) * 3.14159f / 180.f;
			sf::Vector2f spawnPos = playerTf.position + sf::Vector2f(std::cos(angle) * spawnRadius, std::sin(angle) * spawnRadius);

			sf::Vector2f offset((rand() % 400) - 200.f, (rand() % 400) - 200.f);
			sf::Vector2f targetPos = playerTf.position + offset;
			sf::Vector2f dir = targetPos - spawnPos;
			float len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));

			sol::table speedRange = config["speed_range"];
			float speed = speedRange[1].get<float>() + (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

			size_t newId = em.createAsteroid(spawnPos, (dir / len) * speed, config["base_size"], config, worldId);
			b2Body_SetAngularVelocity(em.physics[newId].bodyId, ((rand() % 200) - 100.f) / 50.f);

			asteroidSpawnClock.restart();
		}

		// --- 2. СПАВН ПІРАТІВ (Enemy) ---
		const float pirateInterval = 4.0f;
		const int maxPirates = 6;

		if (pirateSpawnClock.getElapsedTime().asSeconds() > pirateInterval && currentPirates < maxPirates) {
			// Спавним пірата з випадкового боку навколо гравця
			float angle = (rand() % 360) * 3.14159f / 180.f;
			float pirateSpawnDist = 1200.f; // Трохи далі за межами екрану
			sf::Vector2f spawnPos = playerTf.position + sf::Vector2f(std::cos(angle) * pirateSpawnDist, std::sin(angle) * pirateSpawnDist);

			// ВИКОРИСТОВУЄМО ТВОЮ НОВУ ФУНКЦІЮ (з параметрами з enemy.lua)
			em.createEnemy(spawnPos, lua, worldId);

			std::cout << "[SPAWNER] Pirate spawned! Current total: " << currentPirates + 1 << std::endl;
			pirateSpawnClock.restart();
		}
	}
};

class WeaponSystem {
public:
	static void update(EntityManager& em, b2WorldId worldId, size_t playerID, float dt, sol::state& lua) {
		static float shootTimer = 0.f;
		float fireRate = 0.2f; // Пауза між пострілами
		shootTimer -= dt;
		sol::table binds = lua["key_bindings"];

		if (InputRegistry::isPressed(binds["fire"].get<std::string>()) && shootTimer <= 0) {
			auto& playerTf = em.transforms[playerID];

			// Напрямок стрільби за поворотом гравця
			float angleRad = (playerTf.rotation - 90.f) * 3.14159f / 180.f;
			sf::Vector2f direction(std::cos(angleRad), std::sin(angleRad));

			float bulletSpeed = lua["bullet_speed"].get_or(800.f);
			sf::Vector2f bulletVel = direction * bulletSpeed;

			sf::Vector2f spawnPos = playerTf.position + direction * 50.f;

			// Додаємо BodyType::Player в кінці, щоб куля знала, що вона "дружня" і не вбила гравця
			em.createBullet(spawnPos, bulletVel, playerTf.rotation, lua, worldId);

			shootTimer = fireRate;
		}

		// Логіка "час життя" кулі
		for (size_t i = em.bullets.size(); i-- > 0; ) {
			b2BodyId bodyId = em.physics[i].bodyId;
			if (!b2Body_IsValid(bodyId)) continue;

			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);
			if (type != BodyType::Bullet) continue;

			auto& bullet = em.bullets[i];
			bullet.lifetime -= dt;

			// --- ЕФЕКТ ЗГАСАННЯ (Fade Out) ---
			float maxLifetime = lua["bullet_lifetime"].get_or(1.5f);
			float ratio = bullet.lifetime / maxLifetime;
			if (ratio < 0.f) ratio = 0.f;

			auto& shape = em.renders[i].shape;
			sf::Color outlineCol = shape.getOutlineColor();
			sf::Color fillCol = shape.getFillColor();

			outlineCol.a = static_cast<std::uint8_t>(ratio * 255);
			fillCol.a = static_cast<std::uint8_t>(ratio * 255);

			shape.setOutlineColor(outlineCol);
			shape.setFillColor(fillCol);

			// Важливо: видалення куль тепер зазвичай відбувається в DamageSystem, 
			// але ми залишаємо тут видалення за таймером життя
			if (bullet.lifetime <= 0 || bullet.markedForDestroy) {
				em.destroyEntity(i);
				// Оскільки WeaponSystem не знає про зміщення індексу гравця в межах циклу,
				// краще переконатися, що playerID актуальний (якщо гравця видалили раніше)
			}
		}
	}
};

class BackgroundSystem {
public:
	static void update(EntityManager& em, sf::Vector2f playerVelocity, sf::Vector2u windowSize, float dt) {
		for (auto& star : em.stars) {
			// Рухаємо зірки залежно від швидкості гравця
			star.position -= playerVelocity * dt * star.parallaxFactor;

			// Зациклення екрану (Infinite Scroll)
			if (star.position.x < 0) star.position.x += windowSize.x;
			if (star.position.x > windowSize.x) star.position.x -= windowSize.x;
			if (star.position.y < 0) star.position.y += windowSize.y;
			if (star.position.y > windowSize.y) star.position.y -= windowSize.y;
		}
	}

	static void draw(sf::RenderWindow& window, EntityManager& em) {
		sf::VertexArray va(sf::PrimitiveType::Triangles);
		// Резервуємо місце (6 вершин на кожну зірку) для швидкодії
		// va.resize(em.stars.size() * 6); // опціонально

		for (const auto& star : em.stars) {
			float r = star.size / 2.0f;
			sf::Vector2f p = star.position;

			sf::Vertex v0({ p.x - r, p.y - r }, star.color);
			sf::Vertex v1({ p.x + r, p.y - r }, star.color);
			sf::Vertex v2({ p.x + r, p.y + r }, star.color);
			sf::Vertex v3({ p.x - r, p.y + r }, star.color);

			// Два трикутники (Quad)
			va.append(v0); va.append(v1); va.append(v2);
			va.append(v2); va.append(v3); va.append(v0);
		}
		window.draw(va);
	}
};

class ParticleSystem {
public:
	static void update(EntityManager& em, float dt) {
		for (size_t i = em.particles.size(); i-- > 0; ) {
			auto& p = em.particles[i];
			p.position += p.velocity * dt;
			p.lifetime -= dt;

			if (p.lifetime <= 0) {
				// Замінюємо поточну частинку останньою і видаляємо останню
				em.particles[i] = em.particles.back();
				em.particles.pop_back();
				continue; // переходимо до наступної
			}

			float ratio = p.lifetime / p.maxLifetime;
			p.color.a = static_cast<uint8_t>(255 * ratio);
		}
	}

	static void draw(sf::RenderWindow& window, EntityManager& em) {
		if (em.particles.empty()) return;

		sf::VertexArray va(sf::PrimitiveType::Triangles, em.particles.size() * 6);

		for (size_t i = 0; i < em.particles.size(); ++i) {
			size_t idx = i * 6;
			const auto& p = em.particles[i];
			float s = p.size / 2.0f;

			va[idx + 0] = { {p.position.x - s, p.position.y - s}, p.color };
			va[idx + 1] = { {p.position.x + s, p.position.y - s}, p.color };
			va[idx + 2] = { {p.position.x - s, p.position.y + s}, p.color };

			va[idx + 3] = { {p.position.x + s, p.position.y - s}, p.color };
			va[idx + 4] = { {p.position.x + s, p.position.y + s}, p.color };
			va[idx + 5] = { {p.position.x - s, p.position.y + s}, p.color };
		}
		window.draw(va);
	}
};



class AISystem {
	static inline std::map<size_t, AIState> aiCache;

public:
	static void update(EntityManager& em, size_t playerID, float dt, sol::state& lua) {
		sf::Vector2f playerPos = em.transforms[playerID].position;

		// Налаштування з Lua
		sol::table config = lua["enemy_config"];
		float enginePower = config["engine_power"].get_or(150.0f);
		float maxSpeed = config["max_speed"].get_or(20.0f); // Макс. швидкість Box2D (м/с)

		// Радіуси сприйняття
		float visionRange = 200.f;  // Бачу гравця
		float combatRange = 200.f;  // Починаю стріляти/кружляти

		for (size_t i = 0; i < em.physics.size(); ++i) {
			BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);
			if (type != BodyType::Enemy) continue;

			auto& tf = em.transforms[i];
			auto& ai = aiCache[i];
			b2BodyId bodyId = em.physics[i].bodyId;

			sf::Vector2f enemyPos = tf.position;
			sf::Vector2f toPlayer = playerPos - enemyPos;
			float distToPlayer = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);

			// ==========================================
			// КРОК 1: СЕНСОРИ ТА ЗМІНА СТАНУ
			// ==========================================

			// Чи бачимо ми гравця? (Проста перевірка дистанції, пізніше можна додати Raycast для стін)
			bool canSeePlayer = (distToPlayer < visionRange);

			switch (ai.currentState) {
			case EnemyState::PATROL:
				if (canSeePlayer) {
					ai.currentState = EnemyState::COMBAT; // Помітив -> в бій!
					ai.reactionTimer = 0.4f; // Ефект "Здивування" (затуп перед реакцією)
				}
				break;

			case EnemyState::COMBAT:
				if (!canSeePlayer) {
					ai.currentState = EnemyState::ALERT; // Втратив з виду -> шукати
					ai.lastKnownPlayerPos = playerPos;   // Запам'ятав де бачив востаннє
					ai.searchTimer = 5.0f;               // Шукатиму 5 секунд
				}
				else {
					ai.lastKnownPlayerPos = playerPos;   // Оновлюю інформацію
				}
				break;

			case EnemyState::ALERT:
				if (canSeePlayer) {
					ai.currentState = EnemyState::COMBAT;
				}
				else {
					ai.searchTimer -= dt;
					// Якщо час пошуку вийшов і дійшли до точки -> повертаємось в патруль
					sf::Vector2f toLastKnown = ai.lastKnownPlayerPos - enemyPos;
					float distToLast = std::sqrt(toLastKnown.x * toLastKnown.x + toLastKnown.y * toLastKnown.y);

					if (ai.searchTimer <= 0 || distToLast < 100.f) {
						ai.currentState = EnemyState::PATROL;
					}
				}
				break;
			}

			// ==========================================
			// КРОК 2: ПРИЙНЯТТЯ РІШЕННЯ (Раз на X секунд)
			// ==========================================
			// Це імітує час реакції пілота. Він не міняє курс кожен мілісекунду.

			ai.reactionTimer -= dt;
			if (ai.reactionTimer <= 0) {
				ai.reactionTimer = 0.1f + (rand() % 10) / 100.f; // Рандомна затримка 0.1-0.2с

				sf::Vector2f targetVel(0.f, 0.f);

				if (ai.currentState == EnemyState::COMBAT) {
					// --- ЛОГІКА БОЮ ---
					sf::Vector2f dir = toPlayer / distToPlayer;
					if (distToPlayer > combatRange) {
						targetVel = dir * maxSpeed * 10.f; // Перехоплення
					}
					else {
						// Орбіта (кружляння)
						sf::Vector2f orbit(-dir.y, dir.x);
						// Трохи наближаємось (0.3) і сильно кружляємо (1.0)
						targetVel = (dir * 0.3f + orbit) * (maxSpeed * 8.f);
					}
				}
				else if (ai.currentState == EnemyState::ALERT) {
					// --- ЛОГІКА ПОШУКУ ---
					// Летимо туди, де бачили гравця
					sf::Vector2f toTarget = ai.lastKnownPlayerPos - enemyPos;
					float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
					if (d > 10.f) targetVel = (toTarget / d) * (maxSpeed * 12.f); // Швидше, бо тривога
				}
				else {
					// --- ЛОГІКА ПАТРУЛЯ ---
					ai.patrolWaitTimer -= 0.15f; // Таймер зменшується дискретно
					if (ai.patrolWaitTimer <= 0) {
						// Вибираємо нову точку недалеко
						float angle = (rand() % 360) * 3.14159f / 180.f;
						ai.patrolTarget = enemyPos + sf::Vector2f(std::cos(angle), std::sin(angle)) * 300.f;
						ai.patrolWaitTimer = 4.0f;
					}

					sf::Vector2f toTarget = ai.patrolTarget - enemyPos;
					float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
					if (d > 50.f) {
						targetVel = (toTarget / d) * (maxSpeed * 5.f); // Повільно
					}
				}

				// Зберігаємо бажану швидкість
				ai.smoothedDesiredVel = targetVel;
			}

			// ==========================================
			// КРОК 3: УНИКНЕННЯ (Працює кожен кадр для безпеки)
			// ==========================================
			sf::Vector2f avoidance(0.f, 0.f);
			for (size_t j = 0; j < em.physics.size(); ++j) {
				if ((BodyType)(uintptr_t)b2Body_GetUserData(em.physics[j].bodyId) == BodyType::Asteroid) {
					sf::Vector2f diff = enemyPos - em.transforms[j].position;
					float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
					if (d < 300.f) {
						avoidance += (diff / d) * 600.f * (1.0f - d / 300.f);
					}
				}
			}

			// Уникнення має пріоритет над бажанням АІ
			sf::Vector2f finalDesiredVel = ai.smoothedDesiredVel + avoidance;


			// ==========================================
			// КРОК 4: ФІЗИКА (Плавне керування)
			// ==========================================
			b2Vec2 currentVel = b2Body_GetLinearVelocity(bodyId);
			b2Vec2 desiredB2 = { finalDesiredVel.x / SCALE, finalDesiredVel.y / SCALE };

			// Steering force = Desired - Current
			b2Vec2 impulse = {
				(desiredB2.x - currentVel.x),
				(desiredB2.y - currentVel.y)
			};

			// Обмежуємо силу, щоб не було ривків (Steering clamping)
			float impulseLen = std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y);
			float maxForce = enginePower * dt; // Максимальна сила за кадр

			if (impulseLen > maxForce) {
				float scale = maxForce / impulseLen;
				impulse.x *= scale;
				impulse.y *= scale;
			}

			// Застосовуємо силу (не імпульс, щоб врахувати масу)
			// Множимо на 50, щоб компенсувати dt і слабкість enginePower
			b2Body_ApplyForceToCenter(bodyId, { impulse.x * 50.0f, impulse.y * 50.0f }, true);


			// ==========================================
			// КРОК 5: ПОВОРОТ
			// ==========================================
			// Повертаємо ніс туди, куди летимо (або стріляємо)
			float targetAngle = tf.rotation;

			// Якщо швидкість мала - не крутимось дарма
			if (std::abs(finalDesiredVel.x) > 10.f || std::abs(finalDesiredVel.y) > 10.f) {
				targetAngle = std::atan2(finalDesiredVel.y, finalDesiredVel.x) * 180.f / 3.14159f + 90.f;
			}
			// Якщо бій - дивимось строго на гравця (ігноруємо вектор руху)
			if (ai.currentState == EnemyState::COMBAT) {
				targetAngle = std::atan2(toPlayer.y, toPlayer.x) * 180.f / 3.14159f + 90.f;
			}

			float rotSpeed = config["rotation_speed"].get_or(4.0f);
			float deltaAngle = targetAngle - tf.rotation;
			while (deltaAngle > 180) deltaAngle -= 360;
			while (deltaAngle < -180) deltaAngle += 360;

			// Додаємо "похибку" повороту (недовертає або перевертає)
			float error = std::sin(ai.searchTimer * 2.0f) * 5.0f; // Легке похитування

			tf.rotation += (deltaAngle + error) * rotSpeed * dt;

			// Синхронізація з Box2D
			b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId), b2MakeRot(tf.rotation * 3.14159f / 180.f));
		}
	}
};