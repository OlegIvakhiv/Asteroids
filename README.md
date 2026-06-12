<img width="800" height="450" alt="ezgif-6024e53e2437849d" src="https://github.com/user-attachments/assets/6c89f26f-5f2c-46a8-97cf-b8edb269c1d3" />


# 🚀 Modular Space Engine

> Модульний ігровий рушій на основі ECS-архітектури для 2D космічної аркади, натхненої класичними "Spacewar!" та "Asteroids".

---

## 👤 Автор

- **ПІБ**: Івахів Олег Олегович
- **Група**: ФЕП-43
- **Керівник**: ас. Футей О.В.
- **Рецензент**: доц. Дуфанець М.В.
- **Дата виконання**: 2026

---

## 📌 Загальна інформація

- **Тип проєкту**: Ігровий рушій / Гра
- **Мова програмування**: C++
- **Бібліотеки**: SFML 3.0, Box2D v3.0, Lua 5.4, sol2
- **Архітектура**: Entity Component System (ECS), Struct-of-Arrays (SoA)
- **Патерни проектування**: Factory, Observer, Singleton, Strategy

---

## 🧠 Опис функціоналу

- 🛸 Керування кораблем гравця — рух, прицілювання мишею, турбо-режим, ривок
- 💥 Астероїди трьох розмірів з каскадним розпадом та магматичний вибуховий тип
- 🤖 Штучний інтелект ворогів на основі FSM (PATROL → ALERT → COMBAT)
- 🔫 Система зброї: звичайний постріл та Rift Bolt з детонацією
- 🥋 Механіка парирування з відбиттям астероїдів і перетворенням їх на ракети
- ⭐ Паралакс-фон з 400 зірками трьох рівнів глибини
- 💫 Система частинок для вибухів, іскор та шлейфів двигунів
- 📜 Lua-скрипти для конфігурації без перекомпіляції ядра (гаряче перезавантаження F5)
- 🏆 Система нарахування очок

---

## 🧱 Опис основних файлів

| Файл | Призначення |
|------|-------------|
| `game.cpp` | Точка входу, головний ігровий цикл, ініціалізація підсистем |
| `EntityManager.hpp` | ECS-ядро: паралельні вектори компонентів, swap-and-pop видалення, частинки |
| `EntityFactory.hpp` | Фабрика сутностей: гравець, астероїди, снаряди, вороги |
| `Systems.hpp` | Усі ігрові системи: фізика, рендеринг, AI, зброя, пошкодження тощо |
| `components.hpp` | POD-структури компонентів: Transform, Physics, Health, Render і т.д. |
| `GameConfig.hpp` | Константи UI, розмір вікна, кольори |
| `scripts/player.lua` | Параметри гравця: фізика, форма корабля, зброя, клавіші |
| `scripts/asteroids.lua` | Типи астероїдів (SMALL/MEDIUM/LARGE/MAGMATIC), налаштування спавну |
| `scripts/enemy.lua` | Параметри ворога: фізика, бойові характеристики, AI |

---

## ▶️ Як запустити проєкт "з нуля"

### 1. Встановлення інструментів

- Компілятор C++17 або новіший (MSVC 2022 рекомендовано)
- [CMake](https://cmake.org/) v3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) — для встановлення Box2D, Lua, sol2
- [SFML 3.0](https://www.sfml-dev.org/) — встановлюється окремо (див. нижче)

### 2. Встановлення vcpkg

Якщо vcpkg ще не встановлено:

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

Створіть змінну середовища `VCPKG_ROOT`, що вказує на цю папку:

```powershell
setx VCPKG_ROOT "C:\шлях\до\vcpkg"
```

> ⚠️ Після цього потрібно **відкрити новий термінал**, щоб зміна набула чинності.

Залежності `box2d`, `lua`, `sol2` встановлюються автоматично через `vcpkg.json` (manifest mode) під час конфігурації CMake — окремо їх встановлювати не потрібно.

### 3. Встановлення SFML 3.0

SFML встановлюється окремо (наприклад, завантаженням готових бінарників з [sfml-dev.org](https://www.sfml-dev.org/download/sfml/3.0.0/) або збіркою з джерела).

Шлях до SFML вказується через `SFML_ROOT` у `CMakeLists.txt` (за замовчуванням або через `-DSFML_ROOT=...` при конфігурації), або змінну середовища `CMAKE_PREFIX_PATH`.

### 4. Клонування репозиторію

```bash
git clone https://github.com/your-user/modular-space-engine.git
cd modular-space-engine
```

### 5. Конфігурація та збірка

Проєкт містить `CMakePresets.json`, що автоматично підключає vcpkg toolchain:

```bash
cmake --preset default
cmake --build build --config Release
```

При першій конфігурації vcpkg автоматично завантажить і встановить `box2d`, `lua`, `sol2` — це може зайняти кілька хвилин.

### 6. Запуск

Виконуваний файл та всі необхідні DLL, `assets/` і `scripts/` копіюються автоматично після збірки:

```bash
cd build/Release
./ModularSpaceEngine.exe
```
---

## 🎮 Управління

| Клавіша | Дія |
|---------|-----|
| `W / A / S / D` | Рух корабля |
| `Миша` | Прицілювання |
| `ЛКМ` | Стрільба / детонація Rift Bolt |
| `ПКМ` (утримувати) | Заряджання Rift Bolt |
| `Shift` | Турбо-прискорення (споживає енергію) |
| `Space` | Ривок (dash) |
| `R` | Парирування |
| `F5` | Гаряче перезавантаження .lua |

---

## 📜 Конфігурація через Lua

Усі ігрові параметри можна змінювати без перекомпіляції. Приклад з `player.lua`:

```lua
engine_power = 150.0      -- тяга двигуна
sprint_power_multiplier = 2.5
hp = 100
bullet_speed = 800.0
fire_rate = 0.2

key_bindings = {
    up    = "W",
    fire  = "MouseLeft",
    dash  = "Space",
    parry = "R"
}
```

Приклад нового типу астероїда в `asteroids.lua`:

```lua
asteroid_types = {
    MAGMATIC = {
        color = { r = 255, g = 80, b = 40 },
        base_size = 0.9,
        hp = 30,
        explosive = true,
        explosion_radius = 300.0,
        explosion_damage = 120.0
    }
}
```

---

## 🏗️ Архітектура

Рушій побудований на чотирьох рівнях:

Конфігурація (.lua)  →  Фабрика (EntityFactory)
↓
EntityManager (паралельні вектори SoA)
↓
Системи (Physics, AI, Damage, Render...)


Видалення сутностей виконується за алгоритмом **swap-and-pop** — O(1) замість O(n):
1. Скопіювати останній елемент на місце видаленого
2. Оновити `entityIdMap` для переміщеної сутності
3. Викликати `pop_back()` для всіх векторів

---

## 🧪 Проблеми і рішення

| Проблема | Рішення |
|----------|---------|
| Снаряди "пролітають крізь" астероїди | Увімкнено `isBullet = true` в Box2D (Continuous Collision Detection) |
| Індекс сутності стає невалідним після видалення | Усі посилання між сутностями зберігаються через `entityId`, а не індекс |
| Десинхронізація векторів після додавання компонента | При кожному новому компоненті обов'язково додати `swap` та `pop_back()` у `destroyEntity` |
| Зависання при зміні параметрів балансу | Lua hot-reload через F5 — зміни застосовуються без перезапуску |

---

## 📊 Продуктивність

Виміряно на Intel Core i5-10400F, 16 ГБ RAM, Release build (-O2):

| Сценарій | Сутностей | FPS |
|----------|-----------|-----|
| Мінімальне навантаження | ~20 | 60 |
| Стандартний бій | ~52 | 60 |
| Пікове навантаження | ~103 | 60 |

PhysicsSystem + AISystem разом займають менше **0.5% кадрового бюджету** при 52 сутностях.

---

## 🧾 Використані джерела

- [SFML 3.0 Documentation](https://www.sfml-dev.org/documentation/)
- [Box2D v3.0 Documentation](https://box2d.org/documentation/)
- [sol2 Documentation](https://sol2.readthedocs.io/)
- [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/)
- Gregory J. *Game Engine Architecture*, 3rd ed., CRC Press, 2018
- Nystrom R. *Game Programming Patterns*, 2014 — [gameprogrammingpatterns.com](https://gameprogrammingpatterns.com/)
- Fabian R. *Data-Oriented Design*, 2018 — [dataorienteddesign.com](https://www.dataorienteddesign.com/dodbook/)

---

## 📷 Геймплей проекту

<img width="800" height="450" alt="ezgif-66049663d8fffc05" src="https://github.com/user-attachments/assets/de188c57-e369-4bab-9ab2-6ae02b1576ec" />



<img width="800" height="450" alt="ezgif-69e91a413470ef26" src="https://github.com/user-attachments/assets/16d9d5cb-5eaf-4817-9f1d-17ac77859142" />



<img width="800" height="450" alt="ezgif-6024e53e2437849d" src="https://github.com/user-attachments/assets/d7510688-f3d8-4b2f-97c5-2cf1c8d0ef88" />
