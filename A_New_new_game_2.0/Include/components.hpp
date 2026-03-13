//components.hpp

#pragma once
#include <SFML/Graphics.hpp>
#include <box2d/box2d.h>

// Дані про розташування
struct TransformComponent {
	sf::Vector2f position;
	sf::Vector2f velocity; // інерції
	sf::Vector2f acceleration; // для плавності ходу
	float rotation = 0.f;      // кут повороту

	// Механіка ривка
	float dashCooldown;
	float dashMaxCooldown;

	// --- СИСТЕМА ЕНЕРГІЇ (ENERGY DRIVE) ---
	float energyDrive = 100.f;       // Поточна енергія
	float maxEnergyDrive = 100.f;    // Максимум
	float overheatTimer = 0.f;       // Таймер штрафу (коли енергія впала в 0)
	// Стани
	bool isTurbo = false;            // Чи летимо ми на спринті прямо зараз
};

struct BulletComponent {
	float lifetime = 2.0f;
	bool markedForDestroy = false;
	bool isActive = false;
};


struct RenderComponent {
	sf::ConvexShape shape;
};

struct PhysicsComponent {
	b2BodyId bodyId; // ID тіла  Box2D
};

struct Particle {
	sf::Vector2f position;
	sf::Vector2f velocity;
	sf::Color color;
	float lifetime;
	float maxLifetime;
	float size;
};

struct HealthComponent {
	float maxHp = 100.f;
	float currentHp = 100.f;

	// Таймери невразливості (i-frames)
	float invulTimer = 0.f;
	float cheapInvulTimer = 0.f;
};

struct Star {
	sf::Vector2f position;
	float parallaxFactor;
	float size;
	sf::Color color;
};

enum class EnemyState {
	PATROL, // Блукає, розслаблений
	ALERT,  // Летить на останню позицію, де бачив гравця
	COMBAT  // Бачить гравця, атакує
};

struct EnemyComponent {
	enum State { IDLE, CHASE, AVOID } state = IDLE;
	float detectionRadius = 600.f;
	// Стрільба
	float fireTimer = 0.f;
	float fireRate = 1.5f;
	float attackRange = 500.f;

};

struct AIState {
	EnemyState currentState = EnemyState::PATROL;

	// Пам'ять
	sf::Vector2f lastKnownPlayerPos;
	float searchTimer = 0.f;      // Скільки часу шукає гравця в зоні Alert

	// Патрулювання
	sf::Vector2f patrolTarget;
	float patrolWaitTimer = 0.f;  // Час "тупняка" перед зміною точки патруля

	// "Людський фактор" (Помилки та реакція)
	float reactionTimer = 0.f;    // Затримка перед оновленням курсу
	float reactionDelay = 0.2f;   // Оновлюємо "мізки" лише 5 разів на секунду
	sf::Vector2f smoothedDesiredVel; // Щоб рух був плавним
};