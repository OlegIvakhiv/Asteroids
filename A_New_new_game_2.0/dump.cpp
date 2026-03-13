/*

size_t createEnemy(sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
    // Створюємо як гравця, але з типом Enemy
    size_t id = createPlayer(pos, lua, worldId);

    // Перезаписуємо тип у Box2D userData
    b2Body_SetUserData(physics[id].bodyId, (void*)(uintptr_t)BodyType::Enemy);

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.filter.categoryBits = CATEGORY_ENEMY;
    shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_PLAYER | CATEGORY_BULLET | CATEGORY_ENEMY;

    shapeDef.material.friction = 0.3f;
    shapeDef.material.restitution = 0.5f;

    shapeDef.enableContactEvents = true;
    // Скидаємо кулдаун ривка, щоб вони не світилися синім
    transforms[id].dashCooldown = 0.0f;
    transforms[id].dashMaxCooldown = 1.0f;

    // Встановлюємо червоний колір за замовчуванням
    renders[id].shape.setFillColor(sf::Color::Red);

    return id;
}

*/