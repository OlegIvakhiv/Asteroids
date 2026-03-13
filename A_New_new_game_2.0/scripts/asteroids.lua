-- asteroid.lua

asteroid_types = {
    SMALL = {
       color = { r = 100, g = 100, b = 110 },
        base_size = 0.3,
        density = 2.0,
        hp = 10,
        score_reward = 10,
        speed_range = {7.0, 9.0} 
    },
    MEDIUM = {
       color = { r = 60, g = 55, b = 50 },
        base_size = 0.7,
        density = 5.5,
        hp = 35,
        score_reward = 50,
        speed_range = {6.0, 7.0}
    },
    LARGE = {
        color = { r = 60, g = 55, b = 50 }, 
        base_size = 1.5,
        density = 10.0,    
        hp = 120,
        score_reward = 200,
        speed_range = {5.0, 6.0}  
    }
}


spawn_settings = {
    interval = 0.5,    
    despawn_radius = 100.0,
    max_count = 60,    
    spawn_radius = 2500 
}