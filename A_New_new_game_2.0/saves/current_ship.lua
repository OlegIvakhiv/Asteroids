-- Void Hunter ship. Drop this file in the game's ships/ folder.
return {
    format = "voidhunter.ship.v1",
    name   = "UNNAMED",
    class  = "medium",
    hull = { {-28,15}, {0,-30}, {28,15}, {18,28}, {-18,28}, },
    mounts = { guns = { 1, 0, 2, }, drives = { 3, }, spinal = 1, rift_shared = false },
    model = { {0,-42}, {5,-26}, {20,2}, {28,-3}, {31,10}, {36,20}, {24,24}, {20,34}, {8,30}, {0,32}, {-8,30}, {-20,34}, {-24,24}, {-36,20}, {-31,10}, {-28,-3}, {-20,2}, {-5,-26}, },
    paint = {
        hull = "#2864FFFF", outline = "#FFFFFFFF",
        plasma = "#78E6FFFF", thrust = "#3CA0FFB4",
        turbo = "#B4DCFFDC", dodge = "#00DCFFE6",
        parry = "#00FFC8FF", cockpit = "#B4F5FFEB",
        homing = "#00FFC8FF",
    },
    decals = {
    },
    plates = {
    },
    cockpit = { style = "none", x = 0, y = -6, w = 13, h = 17, angle = 0 },
}
