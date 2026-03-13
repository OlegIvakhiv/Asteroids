#pragma once

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <vector>
#include <memory>
#include "components.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <random>


const float SCALE = 30.f;


enum CollisionCategory {
    CATEGORY_PLAYER = 0x0001,
    CATEGORY_ASTEROID = 0x0002,
    CATEGORY_BULLET = 0x0004,
    CATEGORY_ENEMY = 0x0008
};

enum class BodyType { Player, Asteroid, Bullet, Enemy };

struct EntityData {
    BodyType type;
    size_t id; 
};

class EntityManager {
 public:

    std::vector<TransformComponent> transforms;
    std::vector<RenderComponent> renders;
    std::vector<PhysicsComponent> physics;
    std::vector<HealthComponent> healths;
    std::vector<BulletComponent> bullets;
    
    std::vector<EnemyComponent> enemies;

    std::vector<Particle> particles;
    std::vector<Star> stars; 
    
    std::vector<int> scoreRewards; 
    int totalScore = 0;         



    void destroyEntity(size_t index) {
        if (index >= transforms.size()) return;

        // Знищуємо фізичне тіло Box2D
        b2DestroyBody(physics[index].bodyId);

        // Видаляємо з усіх векторів одночасно
        transforms.erase(transforms.begin() + index);
        renders.erase(renders.begin() + index);
        physics.erase(physics.begin() + index);
        scoreRewards.erase(scoreRewards.begin() + index);

        // Перевіряємо наявність у векторах, які можуть бути меншими
        if (index < healths.size()) healths.erase(healths.begin() + index);
        if (index < bullets.size()) bullets.erase(bullets.begin() + index);
    }



    void initBackground(sf::Vector2u winSize, int count = 400) {
        stars.clear();
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> distX(0.f, (float)winSize.x);
        std::uniform_real_distribution<float> distY(0.f, (float)winSize.y);
        std::uniform_real_distribution<float> distSpeed(0.05f, 0.5f);
        std::uniform_int_distribution<int> distAlpha(100, 255);

        for (int i = 0; i < count; ++i) {
            Star s;
            s.parallaxFactor = distSpeed(rng);
            s.position = { distX(rng), distY(rng) };
            s.size = s.parallaxFactor * 4.0f;

            int g = 255 - (rand() % 50);
            int a = distAlpha(rng);

            if (s.parallaxFactor < 0.2f) {
                a /= 2;
                s.size = 1.0f;
            }

            s.color = sf::Color(255, g, 255, a);
            stars.push_back(s);
        }
    }



    void spawnExplosion(sf::Vector2f pos, sf::Color color, int count, float baseSize) {
        for (int i = 0; i < count; ++i) {
            float angle = (rand() % 360) * 3.14159f / 180.f;
            float speed = (rand() % 100) / 10.f + 2.f;
            float life = 0.5f + (rand() % 50) / 100.f;
            float pSize = baseSize * (0.5f + (rand() % 100) / 100.f);

            particles.push_back({
                pos, 
                { std::cos(angle) * speed * 20.f, std::sin(angle) * speed * 20.f },
                color,
                life,
                life,
                pSize
                });
        }
    }



    void spawnImpact(sf::Vector2f pos, sf::Color color, sf::Vector2f bulletVelocity) {
        int count = 5 + (rand() % 4);

        // Отримуємо зворотний напрямок від кулі 
        sf::Vector2f reverseDir = -bulletVelocity;
        float bulletSpeed = std::sqrt(reverseDir.x * reverseDir.x + reverseDir.y * reverseDir.y);
        if (bulletSpeed > 0) reverseDir /= bulletSpeed;

        for (int i = 0; i < count; ++i) {
            // Додаємо рандом до напрямку розлітання конусом
            float spread = 1.2f; 
            sf::Vector2f dir = reverseDir + sf::Vector2f(
                ((rand() % 100) / 50.f - 1.f) * spread,
                ((rand() % 100) / 50.f - 1.f) * spread
            );

            float speed = (rand() % 80) / 10.f + 5.f;
            float life = 0.3f + (rand() % 30) / 100.f; 
            float pSize = 3.0f + (rand() % 30) / 10.f;

            sf::Color sparkColor = color;
            sparkColor.r = std::min(255, sparkColor.r + 50);
            sparkColor.g = std::min(255, sparkColor.g + 50);
            sparkColor.b = std::min(255, sparkColor.b + 50);

            particles.push_back({
                pos,
                dir * speed * 25.f,
                sparkColor,
                life,
                life,
                pSize
                });
        }
    }


    size_t createPlayer(sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        // 1. Transform
        
        transforms.push_back({ pos, {0.f, 0.f}, {0.f, 0.f}, 0.f, 0.f, 0.f });
        bullets.push_back({});
        scoreRewards.push_back({});
        healths.push_back({ 100.f, 100.f, 0.f, 0.f });

        // 2. Physics (Box2D)
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody; // Тіло, що рухається
        bodyDef.userData = (void*)(uintptr_t)BodyType::Player;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };

        // LinearDamping 
        bodyDef.linearDamping = lua["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = lua["angulardgrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();

        shapeDef.filter.categoryBits = CATEGORY_PLAYER;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_PLAYER;

        shapeDef.enableContactEvents = true;
        shapeDef.density = lua["density"].get_or(0.5f);


        // Створюємо форму (хітбокс)
        b2Circle circle = { {0.0f, 0.0f}, 0.8f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        physics.push_back({ bid });


        // 3. Зовнішній вигляд 
        RenderComponent rc;

        sol::table shapeTable = lua["ship_shape"];
        if (shapeTable.valid()) {
            rc.shape.setPointCount(shapeTable.size());
            for (size_t i = 1; i <= shapeTable.size(); ++i) {
                sol::table point = shapeTable[i];

                float px = point["x"].get<float>();
                float py = point["y"].get<float>();

                rc.shape.setPoint(i - 1, sf::Vector2f(px, py));
            }
        }
        else {
            // Резервний варіант 
            rc.shape.setPointCount(3);
            rc.shape.setPoint(0, { 0, -15 });
            rc.shape.setPoint(1, { 10, 10 });
            rc.shape.setPoint(2, { -10, 10 });
        }

        // Завантаження кольорів 
        sol::table luaColor = lua["color"];
        rc.shape.setFillColor(sf::Color(
            luaColor["r"].get_or(40),
            luaColor["g"].get_or(100),
            luaColor["b"].get_or(255),
            luaColor["a"].get_or(255)
        ));

        sol::table luaOutline = lua["outline_color"];
        rc.shape.setOutlineColor(sf::Color(
            luaOutline["r"].get_or(255),
            luaOutline["g"].get_or(255),
            luaOutline["b"].get_or(255)
        ));

        rc.shape.setOutlineThickness(2.5f);

        renders.push_back(rc);

        return transforms.size() - 1;
    }



    size_t createAsteroid(sf::Vector2f pos, sf::Vector2f vel, float baseSize, sol::table config, b2WorldId worldId) {
        // 1. Transform
        transforms.push_back({ pos, {0.f, 0.f}, {0.f, 0.f}, 0.f, 0.f, 0.f });

        // 2. Physics (Box2D Body)
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.userData = (void*)(uintptr_t)BodyType::Asteroid;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { vel.x, vel.y };
        bodyDef.linearDamping = 0.0f;
        bodyDef.angularDamping = 0.05f;

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // --- НОВА ЛОГІКА ГЕНЕРАЦІЇ ТОЧОК ---
        RenderComponent rc;
        std::vector<b2Vec2> physicsPoints;

        // Обмежуємо до 8 точок для фізики (ліміт Box2D для стабільності)
        int numPoints = 8;
        rc.shape.setPointCount(numPoints);

        float pixelRadius = baseSize * SCALE;

        for (int i = 0; i < numPoints; ++i) {
            float angle = (i / (float)numPoints) * 2.f * 3.14159f;

            // Рандом для форми
            float noise = (rand() % 100) / 100.f;
            float dist = pixelRadius * (0.85f + noise * 0.4f);

            // Координати для SFML (пікселі)
            float px = std::cos(angle) * dist;
            float py = std::sin(angle) * dist;
            rc.shape.setPoint(i, { px, py });

            // Координати для Box2D (метри)
            physicsPoints.push_back({ px / SCALE, py / SCALE });
        }

        // Створення фізичного полігону замість кола
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ASTEROID;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(1.0f);
        shapeDef.material.friction = 0.1f;
        shapeDef.material.restitution = 0.8f;

        b2Hull hull = b2ComputeHull(physicsPoints.data(), (int)physicsPoints.size());
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        physics.push_back({ bid });

        // 3. Решта параметрів
        float hpValue = config["hp"].get_or(20.0f);
        healths.push_back({ hpValue, hpValue, 0.f, 0.f });
        bullets.push_back({});
        scoreRewards.push_back(config["score_reward"].get_or(10));

        // Колір та візуал
        int gray = 40 + (rand() % 30);
        rc.shape.setFillColor(sf::Color(gray, gray, gray + (rand() % 5)));
        rc.shape.setOutlineColor(sf::Color(gray + 40, gray + 40, gray + 45));
        rc.shape.setOutlineThickness(2.0f);
        renders.push_back(rc);

        float randomSpin = ((rand() % 200) - 100.f) / 50.f;
        b2Body_SetAngularVelocity(bid, randomSpin);

        return transforms.size() - 1;
    }



    size_t createBullet(sf::Vector2f pos, sf::Vector2f velocity, float angle, sol::state& lua, b2WorldId worldId) {

        // 1. Зчитуємо налаштування з Lua
        float speed = lua["bullet_speed"].get_or(800.0f);
        float lifetime = lua["bullet_lifetime"].get_or(1.5f);
        sol::table col = lua["bullet_color"];

        // Коригуємо швидкість: беремо напрямок з кута і множимо на нову швидкість
        float rad = (angle - 90.f) * 3.14159f / 180.f;
        sf::Vector2f newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };

        // 1. Transform
        transforms.push_back({ pos, newVelocity, {0.f, 0.f}, angle, 0.f, 0.f });

        // 2. Physics (Box2D)
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.userData = (void*)(uintptr_t)BodyType::Bullet;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { newVelocity.x / SCALE, newVelocity.y / SCALE };
        bodyDef.rotation = b2MakeRot(angle * 3.14159f / 180.f);
        bodyDef.isBullet = true; 

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_BULLET;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;

        b2Circle circle = { {0.0f, 0.0f}, 0.1f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        physics.push_back({ bid });
        bullets.push_back({ lifetime, false, true }); // додаємо isActive = true
        healths.push_back({});
        scoreRewards.push_back({});

        // 3. Render 
        RenderComponent rc;
        rc.shape.setPointCount(4);
        rc.shape.setPoint(0, { 0, -10 }); // Ніс 
        rc.shape.setPoint(1, { 1.5f, 0 }); // Бік 
        rc.shape.setPoint(2, { 0, 10 });  // Хвіст
        rc.shape.setPoint(3, { -1.5f, 0 });// Бік

        rc.shape.setFillColor(sf::Color::White);
        rc.shape.setOutlineThickness(1.5f); 
        rc.shape.setOutlineColor(sf::Color(
            col["r"].get_or(0),
            col["g"].get_or(255),
            col["b"].get_or(255),
            180
        ));

        renders.push_back(rc);

        return transforms.size() - 1;
    }
    
    

    size_t createEnemy(sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        sol::table config = lua["enemy_config"];

        TransformComponent tf;
        tf.position = pos;
        transforms.push_back(tf);

        RenderComponent rc;
        rc.shape.setPointCount(12);

        // Центральний ніс (виїмка)
        rc.shape.setPoint(0, { 0, -10 });
        // Праві "ікла" носа
        rc.shape.setPoint(1, { 8, -25 });
        rc.shape.setPoint(2, { 12, -10 });
        // Праве крило
        rc.shape.setPoint(3, { 25, 5 });
        rc.shape.setPoint(4, { 25, 15 });
        rc.shape.setPoint(5, { 15, 10 });
        // Корма 
        rc.shape.setPoint(6, { 0, 20 });
        // Ліве крило 
        rc.shape.setPoint(7, { -15, 10 });
        rc.shape.setPoint(8, { -25, 15 });
        rc.shape.setPoint(9, { -25, 5 });
        // Ліві "ікла" носа
        rc.shape.setPoint(10, { -12, -10 });
        rc.shape.setPoint(11, { -8, -25 });

        rc.shape.setFillColor(sf::Color(config["color"]["r"], config["color"]["g"], config["color"]["b"]));
        rc.shape.setOutlineThickness(1.5f);
        rc.shape.setOutlineColor(sf::Color(255, 255, 255, 150)); 
        renders.push_back(rc);

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.userData = (void*)(uintptr_t)BodyType::Enemy;

        bodyDef.linearDamping = config["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = config["angulardgrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ENEMY;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_PLAYER | CATEGORY_BULLET | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(3.0f); 
        shapeDef.material.restitution = 0.4f;

        b2Vec2 physicsPoints[6] = {
    {0.0f, -25.0f / SCALE},          // Кінчик носа
    {25.0f / SCALE, 5.0f / SCALE},   // Верхня частина правого крила
    {25.0f / SCALE, 15.0f / SCALE},  // Нижня частина правого крила
    {0.0f, 20.0f / SCALE},           // Хвіст
    {-25.0f / SCALE, 15.0f / SCALE}, // Нижня частина лівого крила
    {-25.0f / SCALE, 5.0f / SCALE}   // Верхня частина лівого крила
        };
        b2Hull hull = b2ComputeHull(physicsPoints, 6);
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        physics.push_back({ bid });
        healths.push_back({ config["hp"].get_or(50.f), config["hp"].get_or(50.f) });
        bullets.push_back({});
        scoreRewards.push_back(config["score_reward"].get_or(100));

        return transforms.size() - 1;
    }

};
















