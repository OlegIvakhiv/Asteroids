-- enemy.lua
enemy_config = {
    -- Фізика
    density = 4.0,
    lineardrag_factor = 1.0,
    angulardgrag_factor = 2.0,
    
    -- Рух
    engine_power = 200.0,
    rotation_speed = 4.0,
   
    -- Характеристики
    hp = 250.0,
    score_reward = 500,

    color = { r = 255, g = 50, b = 50 },

    despawn_radius = 250.0
}
