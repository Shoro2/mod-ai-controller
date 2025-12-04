# AzerothCore: Server-Side AI Controller Mod

![C++](https://img.shields.io/badge/Language-C++-blue) ![AzerothCore](https://img.shields.io/badge/Core-AzerothCore%203.3.5-red) ![Status](https://img.shields.io/badge/Status-Experimental-orange)

This module implements a **server-side AI controller** for AzerothCore, allowing external agents (e.g., Python Reinforcement Learning models) to control in-game player characters via a TCP socket connection.

It acts as a bridge between the World of Warcraft server logic and an external AI environment (Gymnasium / Stable Baselines3).

## Features

### Core Functionality
* **Socket Communication:** Establishes a TCP server on port `5000` to exchange JSON data with external clients.
* **Real-time State Export:** Sends player data (HP, Mana, Position, Combat State, Nearby Mobs) at a configurable tick rate (Fast: 400ms, Radar: 2000ms).
* **Command Execution:** Receives and executes high-level actions from the AI:
    * `move_forward`, `turn_left`, `turn_right`, `stop`
    * `move_to:x:y:z` (Smart navigation using MMaps/Pathfinding)
    * `cast:spellID` (Automatic target selection and facing)
    * `target_guid`, `loot_guid`, `sell_grey`
    * `reset` (Teleport to homebind, restore HP/Mana for training loops)

### Advanced AI Logic
* **Auto-Targeting:** Detects nearby attackable targets and filters critters/pets.
* **Auto-Looting:** Simulates server-side looting behavior (Money & Items) without client interaction.
* **Auto-Equip:** Automatically equips looted items if they provide better stats (based on a simple ItemScore heuristic).
* **Vendor Interaction:** Detects vendors and sells junk items automatically to free up bag space.
* **Combat Tracking:** Automatically faces the target during combat/casting to prevent "Target not in front" errors.

## Work in progress
* **Qusting:** Detects nearby available quests and do them
* **State management:** Depending on the characters needs, the AI is going to deceide what state is the most efficent (questing, exploring, moving to town/next zone etc.)
* **Combat logic:** Reward the Ai for efficent/fast fights, provide a baseline rotation but let AI control cds.

## Architecture

The module hooks into the AzerothCore engine at two points:

1.  **`AIControllerPlayerScript`**: Handles game events like XP gain, Level Up (auto-reset for training), and Money changes.
2.  **`AIControllerWorldScript`**: Runs the main update loop:
    * **Fast Tick (400ms):** Processes the command queue (thread-safe) and broadcasts the player state.
    * **Slow Tick (2000ms):** Performs expensive grid scans (`Cell::VisitObjects`) to detect nearby entities.
    * **Face Tick (150ms):** Keeps the player facing their target during combat.

## Installation

1.  Copy the `mod-ai-controller` folder into your `azerothcore/modules/` directory.
2.  Re-run CMake to generate the build files.
3.  Compile the core.
4.  Enable the module in `worldserver.conf` (if configuration flags are added later).

## Protocol (JSON)

**Server -> Client (State):**
```json
{
  "players": [
    {
      "name": "BotName",
      "hp": 100,
      "max_hp": 100,
      "power": 50,
      "combat": "true",
      "target_status": "alive",
      "nearby_mobs": [
        {"guid": "12345", "name": "Wolf", "hp": 50, "attackable": "1"}
      ],
      "free_slots": 10,
      "equipped_upgrade": "false",
      "xp_gained": 0
    }
  ]
}
```
### Client -> Server (Command):

Format: PlayerName:Action:Value

Example: BotName:cast:585 (Cast Smite)

Example: BotName:move_to:-8949:-132:83

## Requirements
* AzerothCore 3.3.5a (WotLK)

* MMaps must be extracted and enabled in worldserver.conf for pathfinding (move_to) to work correctly.

## License
This code is part of a custom AI research project and is provided "as is".
