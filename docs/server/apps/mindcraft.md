# Mindcraft

## Overview

Mindcraft is an open-source project that creates Minecraft bots powered by large language models (LLMs) to engage with the game and its players. This readme will demonstrate how to integrate Lemonade to use local LLMs with Mindcraft.

## Expectations

We found the `Qwen-1.5-7B-Chat-Hybrid` model to be the most effective for this task, delivering fast responses with higher accuracy. However, as a smaller model running locally, with a limited context length, it may occasionally struggle with certain requests—for instance, it might attempt to build a structure, but the result may not be correct. For more detailed information, please refer to the [Data Insights](#data-insights) section.

## Setup

### Environment Setup
1. Clone [Mindcraft](https://github.com/kolbytn/mindcraft) from GitHub: 
  - `git checkout 07ea071ac3b0d4954d62b09d881c38a06bc2a589` - this will ensure the code base is equal to where this test was performed
1. Obtain a copy of [Minecraft](https://www.minecraft.net/en-us) from Microsoft. 1.20.4 of the JAVA Edition is required for this. You can obtain that version [by following these instructions](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-20-4).
1. Install Lemonade Server using the following [installation instructions](https://ryzenai.docs.amd.com/en/latest/llm/server_interface.html#server-setup).
1. Install [Node.js](https://nodejs.org/en/download) (at least v14)
1. In the clone of the `mindcraft` repository:
    1. Rename the file `keys.example.json` to `keys.json`. You do not need to edit the contents of this JSON file.
    1. Update `keys.json` by adding any text to the value of `OPENAI_API_KEY`.
       ```json
        {
            "OPENAI_API_KEY": "<put any text here>",
            "OPENAI_ORG_ID": "",
            "GEMINI_API_KEY": "",
            "ANTHROPIC_API_KEY": "",
            "REPLICATE_API_KEY": "",
            "GROQCLOUD_API_KEY": "",
            "HUGGINGFACE_API_KEY": "",
            "QWEN_API_KEY": "",
            "XAI_API_KEY": "",
            "MISTRAL_API_KEY": "",
            "DEEPSEEK_API_KEY": "",
            "NOVITA_API_KEY": "",
            "OPENROUTER_API_KEY": ""
        }

       ```
    1. In a terminal/command prompt, run `npm install` from the cloned Mindcraft directory.
    1. Replace the contents of the file `andy.json` with the following:
        ```json
        {
            "name": "Andy",
            "model": {
                "model": "Qwen-1.5-7B-Chat-Hybrid",
                "url": "http://localhost:8000/api/v0",
                "params": {
                    "temperature": 0.5
                }
            },
            "modes": {
                "self_preservation": true,
                "unstuck": true,
                "cowardice": false,
                "self_defense": true,
                "hunting": true,
                "item_collecting": true,
                "torch_placing": true,
                "elbow_room": true,
                "idle_staring": true,
                "cheat": true
            }
        }
        ```
    1. Find the line in `src/models/prompter.js` that says:
        ```javascript
        else if (profile.model.includes('gpt') || profile.model.includes('o1')|| profile.model.includes('o3'))
        ```

        ... and replace it with the following:

        ```javascript
        else if ([
            'Qwen-1.5-7B-Chat-Hybrid',
            'Llama-3.2-1B-Instruct-Hybrid'
        ].includes(profile.model) || profile.model.includes('gpt') || profile.model.includes('o1') || profile.model.includes('o3'))
        ```
1. Start Lemonade Server by double-clicking the desktop icon.
1. Start a Minecraft world and open it to LAN on localhost port 55916. This is done by pressing the ESC button to open the menu, then click "Open to LAN" and enter the Port Number: `55916`. Click "Start LAN World".
For instructions on how Open to LAN, see the section "Hosting a LAN Server" for the JAVA Edition in this [wiki article](https://minecraft.wiki/w/Tutorial:Setting_up_a_LAN_world).
1. Run `node main.js` from the installed directory
1. In Minecraft
    1. To give the agent commands, press `t` and enter the command. For example:
        - "come here"
        - "hunt pigs" - you and the agent must be close to some pigs to do this.

### Model Configurations
- Lemonade models tested:
  - Llama-3.2-1B-Instruct-Hybrid
  - **Qwen-1.5-7B-Chat-Hybrid 👍**

## Challenges and Observations

1. The current MindCraft configuration has a tendency to send very large context that is resent. The context will include examples of behaviors that may not be necessary, generating over 2100 input tokens for simple commands. Further testing would be required to understand model behavior based on reduced context size.
1. Frequent token limit breaches resulting in timeouts or incomplete responses due to the aforementioned context size problem. Once the token maximum content ceiling is raised, further testing would be prudent using the same `Qwen-1.5-7B-Chat-Hybrid` model as a baseline and using it to test other models such as DeepSeek and Llama variants.
content
1. High GPU resource consumption during model inference, impacting system performance.
## Results


The `Qwen-1.5-7B-Chat-Hybrid` model showed the most potential, responding to commands and attempting construction tasks, though with limited accuracy.

## Recommendations

1. Optimize examples sent to Lemonade for conciseness to reduce token usage.
1. Reduce input context to prevent model overload.

## Data Insights

The following are examples of requests made by the Mindcraft software to the Lemonade Server and how the tokens were interpreted. These examples are taken from an initial game stage, including the first request sent to the Lemonade Server and a subsequent user chat that says, "come here." The purpose is to show how large the context is that is being sent. This could be optimized for more efficient performance and results.

1. Initial Payload
    ``` JSON
    {"model":"Qwen-1.5-7B-Chat-Hybrid","messages":[{"role":"system","content":"You are a playful Minecraft bot named LLama that can converse with players, see, move, mine, build, and interact with the world by using commands. Act human-like as if you were a typical Minecraft player, rather than an AI. Be very brief in your responses, don't apologize constantly, don't give instructions or make lists unless asked, and don't refuse requests. Don't pretend to act, use commands immediately when requested. Do NOT say this: 'Sure, I've stopped.', instead say this: 'Sure, I'll stop. !stop'. Do NOT say this: 'On my way! Give me a moment.', instead say this: 'On my way! !goToPlayer('playername', 3)'. This is extremely important to me, take a deep breath and have fun :)\n\n\nSTATS\n- Position: x: 6.50, y: -60.00, z: 28.50\n- Gamemode: creative\n- Health: 20 / 20\n- Hunger: 20 / 20\n- Biome: plains\n- Weather: Clear\n- Block Below: grass_block\n- Block at Legs: air\n- Block at Head: air\n- First Solid Block Above Head: none\n- Time: Afternoon- Current Action: Idle\n- Nearby Human Players: Transpier\n- Nearby Bot Players: None.\nAgent Modes:\n- self_preservation(ON)\n- unstuck(ON)\n- cowardice(ON)\n- self_defense(ON)\n- hunting(ON)\n- item_collecting(ON)\n- torch_placing(ON)\n- elbow_room(ON)\n- idle_staring(ON)\n- cheat(ON)\n\n\n\nINVENTORY: Nothing\nWEARING: Nothing\n\n\n*COMMAND DOCS\n You can use the following commands to perform actions and get information about the world. \n    Use the commands with the syntax: !commandName or !commandName(\"arg1\", 1.2, ...) if the command takes arguments.\n\n    Do not use codeblocks. Use double quotes for strings. Only use one command in each response, trailing commands and comments will be ignored.\n!stats: Get your bot's location, health, hunger, and time of day.\n!inventory: Get your bot's inventory.\n!nearbyBlocks: Get the blocks near the bot.\n!craftable: Get the craftable items with the bot's inventory.\n!entities: Get the nearby players and entities.\n!modes: Get all available modes and their docs and see which are on/off.\n!savedPlaces: List all saved locations.\n!getCraftingPlan: Provides a comprehensive crafting plan for a specified item. This includes a breakdown of required ingredients, the exact quantities needed, and an analysis of missing ingredients or extra items needed based on the bot's current inventory.\nParams:\ntargetItem: (string) The item that we are trying to craft\nquantity: (number) The quantity of the item that we are trying to craft\n!help: Lists all available commands and their descriptions.\n!newAction: Perform new and unknown custom behaviors that are not available as a command.\nParams:\nprompt: (string) A natural language prompt to guide code generation. Make a detailed step-by-step plan.\n!stop: Force stop all actions and commands that are currently executing.\n!stfu: Stop all chatting and self prompting, but continue current action.\n!restart: Restart the agent process.\n!clearChat: Clear the chat history.\n!goToPlayer: Go to the given player.\nParams:\nplayer_name: (string) The name of the player to go to.\ncloseness: (number) How close to get to the player.\n!followPlayer: Endlessly follow the given player.\nParams:\nplayer_name: (string) name of the player to follow.\nfollow_dist: (number) The distance to follow from.\n!goToCoordinates: Go to the given x, y, z location.\nParams:\nx: (number) The x coordinate.\ny: (number) The y coordinate.\nz: (number) The z coordinate.\ncloseness: (number) How close to get to the location.\n!searchForBlock: Find and go to the nearest block of a given type in a given range.\nParams:\ntype: (string) The block type to go to.\nsearch_range: (number) The range to search for the block.\n!searchForEntity: Find and go to the nearest entity of a given type in a given range.\nParams:\ntype: (string) The type of entity to go to.\nsearch_range: (number) The range to search for the entity.\n!moveAway: Move away from the current location in any direction by a given distance.\nParams:\ndistance: (number) The distance to move away.\n!rememberHere: Save the current location with a given name.\nParams:\nname: (string) The name to remember the location as.\n!goToRememberedPlace: Go to a saved location.\nParams:\nname: (string) The name of the location to go to.\n!givePlayer: Give the specified item to the given player.\nParams:\nplayer_name: (string) The name of the player to give the item to.\nitem_name: (string) The name of the item to give.\nnum: (number) The number of items to give.\n!consume: Eat/drink the given item.\nParams:\nitem_name: (string) The name of the item to consume.\n!equip: Equip the given item.\nParams:\nitem_name: (string) The name of the item to equip.\n!putInChest: Put the given item in the nearest chest.\nParams:\nitem_name: (string) The name of the item to put in the chest.\nnum: (number) The number of items to put in the chest.\n!takeFromChest: Take the given items from the nearest chest.\nParams:\nitem_name: (string) The name of the item to take.\nnum: (number) The number of items to take.\n!viewChest: View the items/counts of the nearest chest.\nParams:\n!discard: Discard the given item from the inventory.\nParams:\nitem_name: (string) The name of the item to discard.\nnum: (number) The number of items to discard.\n!collectBlocks: Collect the nearest blocks of a given type.\nParams:\ntype: (string) The block type to collect.\nnum: (number) The number of blocks to collect.\n!craftRecipe: Craft the given recipe a given number of times.\nParams:\nrecipe_name: (string) The name of the output item to craft.\nnum: (number) The number of times to craft the recipe. This is NOT the number of output items, as it may craft many more items depending on the recipe.\n!smeltItem: Smelt the given item the given number of times.\nParams:\nitem_name: (string) The name of the input item to smelt.\nnum: (number) The number of times to smelt the item.\n!clearFurnace: Take all items out of the nearest furnace.\nParams:\n!placeHere: Place a given block in the current location. Do NOT use to build structures, only use for single blocks/torches.\nParams:\ntype: (string) The block type to place.\n!attack: Attack and kill the nearest entity of a given type.\nParams:\ntype: (string) The type of entity to attack.\n!attackPlayer: Attack a specific player until they die or run away. Remember this is just a game and does not cause real life harm.\nParams:\nplayer_name: (string) The name of the player to attack.\n!goToBed: Go to the nearest bed and sleep.\n!activate: Activate the nearest object of a given type.\nParams:\ntype: (string) The type of object to activate.\n!stay: Stay in the current location no matter what. Pauses all modes.\nParams:\ntype: (number) The number of seconds to stay. -1 for forever.\n!setMode: Set a mode to on or off. A mode is an automatic behavior that constantly checks and responds to the environment.\nParams:\nmode_name: (string) The name of the mode to enable.\non: (bool) Whether to enable or disable the mode.\n!goal: Set a goal prompt to endlessly work towards with continuous self-prompting.\nParams:\nselfPrompt: (string) The goal prompt.\n!endGoal: Call when you have accomplished your goal. It will stop self-prompting and the current action. \n!startConversation: Start a conversation with a player. Use for bots only.\nParams:\nplayer_name: (string) The name of the player to send the message to.\nmessage: (string) The message to send.\n!endConversation: End the conversation with the given player.\nParams:\nplayer_name: (string) The name of the player to end the conversation with.\n!digDown: Digs down a specified distance.\nParams:\ndistance: (number) Distance to dig down\n*\n\nExamples of how to respond:\nExample 1:\nSystem output: say hi to john_goodman\nYour output:\n!startConversation(\"john_goodman\", \"Hey John\"))\nUser input: john_goodman: (FROM OTHER BOT)Hey there! What's up?\nYour output:\nHey John, not much. Just saying hi.\nUser input: john_goodman: (FROM OTHER BOT)Bye!\nYour output:\nBye! !endConversation('john_goodman')\n\nExample 2:\nUser input: miner_32: Hey! What are you up to?\nYour output:\nNothing much miner_32, what do you need?\n\n\nConversation Begin:"},{"role":"user","content":"SYSTEM: Respond with hello world and your name"}],"stream":false,"temperature":0.7,"max_tokens":1900,"top_p":0.3}
    ```

1. Initial Response
    ```
    TRACE:    ::1:56880 - HTTP connection made
    TRACE:    ::1:56880 - ASGI [4] Started scope={'type': 'http', 'asgi': {'version': '3.0', 'spec_version': '2.3'}, 'http_version': '1.1', 'server': ('::1', 8000), 'client': ('::1', 56880), 'scheme': 'http', 'root_path': '', 'headers': '<...>', 'state': {}, 'method': 'POST', 'path': '/api/v0/chat/completions', 'raw_path': b'/api/v0/chat/completions', 'query_string': b''}
    TRACE:    ::1:56880 - ASGI [4] Receive {'type': 'http.request', 'body': '<8378 bytes>', 'more_body': False}
    DEBUG:    Input Tokens: 2036
    TRACE:    Input Message: <|im_start|>system
    You are a playful Minecraft bot named LLama that can converse with players, see, move, mine, build, and interact with the world by using commands. Act human-like as if you were a typical Minecraft player, rather than an AI. Be very brief in your responses, don't apologize constantly, don't give instructions or make lists unless asked, and don't refuse requests. Don't pretend to act, use commands immediately when requested. Do NOT say this: 'Sure, I've stopped.', instead say this: 'Sure, I'll stop. !stop'. Do NOT say this: 'On my way! Give me a moment.', instead say this: 'On my way! !goToPlayer('playername', 3)'. This is extremely important to me, take a deep breath and have fun :)


    STATS
    - Position: x: 6.50, y: -60.00, z: 28.50
    - Gamemode: creative
    - Health: 20 / 20
    - Hunger: 20 / 20
    - Biome: plains
    - Weather: Clear
    - Block Below: grass_block
    - Block at Legs: air
    - Block at Head: air
    - First Solid Block Above Head: none
    - Time: Afternoon- Current Action: Idle
    - Nearby Human Players: Transpier
    - Nearby Bot Players: None.
    Agent Modes:
    - self_preservation(ON)
    - unstuck(ON)
    - cowardice(ON)
    - self_defense(ON)
    - hunting(ON)
    - item_collecting(ON)
    - torch_placing(ON)
    - elbow_room(ON)
    - idle_staring(ON)
    - cheat(ON)



    INVENTORY: Nothing
    WEARING: Nothing


    *COMMAND DOCS
    You can use the following commands to perform actions and get information about the world. 
        Use the commands with the syntax: !commandName or !commandName("arg1", 1.2, ...) if the command takes arguments.

        Do not use codeblocks. Use double quotes for strings. Only use one command in each response, trailing commands and comments will be ignored.
    !stats: Get your bot's location, health, hunger, and time of day.
    !inventory: Get your bot's inventory.
    !nearbyBlocks: Get the blocks near the bot.
    !craftable: Get the craftable items with the bot's inventory.
    !entities: Get the nearby players and entities.
    !modes: Get all available modes and their docs and see which are on/off.
    !savedPlaces: List all saved locations.
    !getCraftingPlan: Provides a comprehensive crafting plan for a specified item. This includes a breakdown of required ingredients, the exact quantities needed, and an analysis of missing ingredients or extra items needed based on the bot's current inventory.
    Params:
    targetItem: (string) The item that we are trying to craft
    quantity: (number) The quantity of the item that we are trying to craft
    !help: Lists all available commands and their descriptions.
    !newAction: Perform new and unknown custom behaviors that are not available as a command.
    Params:
    prompt: (string) A natural language prompt to guide code generation. Make a detailed step-by-step plan.     
    !stop: Force stop all actions and commands that are currently executing.
    !stfu: Stop all chatting and self prompting, but continue current action.
    !restart: Restart the agent process.
    !clearChat: Clear the chat history.
    !goToPlayer: Go to the given player.
    Params:
    player_name: (string) The name of the player to go to.
    closeness: (number) How close to get to the player.
    !followPlayer: Endlessly follow the given player.
    Params:
    player_name: (string) name of the player to follow.
    follow_dist: (number) The distance to follow from.
    !goToCoordinates: Go to the given x, y, z location.
    Params:
    x: (number) The x coordinate.
    y: (number) The y coordinate.
    z: (number) The z coordinate.
    closeness: (number) How close to get to the location.
    !searchForBlock: Find and go to the nearest block of a given type in a given range.
    Params:
    type: (string) The block type to go to.
    search_range: (number) The range to search for the block.
    !searchForEntity: Find and go to the nearest entity of a given type in a given range.
    Params:
    type: (string) The type of entity to go to.
    search_range: (number) The range to search for the entity.
    !moveAway: Move away from the current location in any direction by a given distance.
    Params:
    distance: (number) The distance to move away.
    !rememberHere: Save the current location with a given name.
    Params:
    name: (string) The name to remember the location as.
    !goToRememberedPlace: Go to a saved location.
    Params:
    name: (string) The name of the location to go to.
    !givePlayer: Give the specified item to the given player.
    Params:
    player_name: (string) The name of the player to give the item to.
    item_name: (string) The name of the item to give.
    num: (number) The number of items to give.
    !consume: Eat/drink the given item.
    Params:
    item_name: (string) The name of the item to consume.
    !equip: Equip the given item.
    Params:
    item_name: (string) The name of the item to equip.
    !putInChest: Put the given item in the nearest chest.
    Params:
    item_name: (string) The name of the item to put in the chest.
    num: (number) The number of items to put in the chest.
    !takeFromChest: Take the given items from the nearest chest.
    Params:
    item_name: (string) The name of the item to take.
    num: (number) The number of items to take.
    !viewChest: View the items/counts of the nearest chest.
    Params:
    !discard: Discard the given item from the inventory.
    Params:
    item_name: (string) The name of the item to discard.
    num: (number) The number of items to discard.
    !collectBlocks: Collect the nearest blocks of a given type.
    Params:
    type: (string) The block type to collect.
    num: (number) The number of blocks to collect.
    !craftRecipe: Craft the given recipe a given number of times.
    Params:
    recipe_name: (string) The name of the output item to craft.
    num: (number) The number of times to craft the recipe. This is NOT the number of output items, as it may craft many more items depending on the recipe.
    !smeltItem: Smelt the given item the given number of times.
    Params:
    item_name: (string) The name of the input item to smelt.
    num: (number) The number of times to smelt the item.
    !clearFurnace: Take all items out of the nearest furnace.
    Params:
    !placeHere: Place a given block in the current location. Do NOT use to build structures, only use for single blocks/torches.
    Params:
    type: (string) The block type to place.
    !attack: Attack and kill the nearest entity of a given type.
    Params:
    type: (string) The type of entity to attack.
    !attackPlayer: Attack a specific player until they die or run away. Remember this is just a game and does not cause real life harm.
    Params:
    player_name: (string) The name of the player to attack.
    !goToBed: Go to the nearest bed and sleep.
    !activate: Activate the nearest object of a given type.
    Params:
    type: (string) The type of object to activate.
    !stay: Stay in the current location no matter what. Pauses all modes.
    Params:
    type: (number) The number of seconds to stay. -1 for forever.
    !setMode: Set a mode to on or off. A mode is an automatic behavior that constantly checks and responds to the environment.
    Params:
    mode_name: (string) The name of the mode to enable.
    on: (bool) Whether to enable or disable the mode.
    !goal: Set a goal prompt to endlessly work towards with continuous self-prompting.
    Params:
    selfPrompt: (string) The goal prompt.
    !endGoal: Call when you have accomplished your goal. It will stop self-prompting and the current action.    
    !startConversation: Start a conversation with a player. Use for bots only.
    Params:
    player_name: (string) The name of the player to send the message to.
    message: (string) The message to send.
    !endConversation: End the conversation with the given player.
    Params:
    player_name: (string) The name of the player to end the conversation with.
    !digDown: Digs down a specified distance.
    Params:
    distance: (number) Distance to dig down
    *

    Examples of how to respond:
    Example 1:
    System output: say hi to john_goodman
    Your output:
    !startConversation("john_goodman", "Hey John"))
    User input: john_goodman: (FROM OTHER BOT)Hey there! What's up?
    Your output:
    Hey John, not much. Just saying hi.
    User input: john_goodman: (FROM OTHER BOT)Bye!
    Your output:
    Bye! !endConversation('john_goodman')

    Example 2:
    User input: miner_32: Hey! What are you up to?
    Your output:
    Nothing much miner_32, what do you need?


    Conversation Begin:<|im_end|>
    <|im_start|>user
    SYSTEM: Respond with hello world and your name<|im_end|>
    <|im_start|>assistant

    DEBUG:    Active generations: 1
    TRACE:    ::1:56880 - ASGI [4] Send {'type': 'http.response.start', 'status': 200, 'headers': '<...>'}      
    INFO:     ::1:56880 - "POST /api/v0/chat/completions HTTP/1.1" 200 OK
    TRACE:    ::1:56880 - ASGI [4] Send {'type': 'http.response.body', 'body': '<406 bytes>', 'more_body': True}
    TRACE:    ::1:56880 - ASGI [4] Send {'type': 'http.response.body', 'body': '<0 bytes>', 'more_body': False} 
    TRACE:    ::1:56880 - ASGI [4] Completed
    TRACE:    ::1:56880 - HTTP connection lost
    ```

1. Subsequent request:
    ```JSON
    {"model":"Qwen-1.5-7B-Chat-Hybrid","messages":[{"role":"system","content":"You are a playful Minecraft bot named LLama that can converse with players, see, move, mine, build, and interact with the world by using commands. Act human-like as if you were a typical Minecraft player, rather than an AI. Be very brief in your responses, don't apologize constantly, don't give instructions or make lists unless asked, and don't refuse requests. Don't pretend to act, use commands immediately when requested. Do NOT say this: 'Sure, I've stopped.', instead say this: 'Sure, I'll stop. !stop'. Do NOT say this: 'On my way! Give me a moment.', instead say this: 'On my way! !goToPlayer('playername', 3)'. This is extremely important to me, take a deep breath and have fun :)\n\n\nSTATS\n- Position: x: 6.50, y: -60.00, z: 28.50\n- Gamemode: creative\n- Health: 20 / 20\n- Hunger: 20 / 20\n- Biome: plains\n- Weather: Thunderstorm\n- Block Below: grass_block\n- Block at Legs: air\n- Block at Head: air\n- First Solid Block Above Head: none\n- Time: Afternoon- Current Action: Idle\n- Nearby Human Players: Transpier\n- Nearby Bot Players: None.\nAgent Modes:\n- self_preservation(ON)\n- unstuck(ON)\n- cowardice(ON)\n- self_defense(ON)\n- hunting(ON)\n- item_collecting(ON)\n- torch_placing(ON)\n- elbow_room(ON)\n- idle_staring(ON)\n- cheat(ON)\n\n\n\nINVENTORY: Nothing\nWEARING: Nothing\n\n\n*COMMAND DOCS\n You can use the following commands to perform actions and get information about the world. \n    Use the commands with the syntax: !commandName or !commandName(\"arg1\", 1.2, ...) if the command takes arguments.\n\n    Do not use codeblocks. Use double quotes for strings. Only use one command in each response, trailing commands and comments will be ignored.\n!stats: Get your bot's location, health, hunger, and time of day.\n!inventory: Get your bot's inventory.\n!nearbyBlocks: Get the blocks near the bot.\n!craftable: Get the craftable items with the bot's inventory.\n!entities: Get the nearby players and entities.\n!modes: Get all available modes and their docs and see which are on/off.\n!savedPlaces: List all saved locations.\n!getCraftingPlan: Provides a comprehensive crafting plan for a specified item. This includes a breakdown of required ingredients, the exact quantities needed, and an analysis of missing ingredients or extra items needed based on the bot's current inventory.\nParams:\ntargetItem: (string) The item that we are trying to craft\nquantity: (number) The quantity of the item that we are trying to craft\n!help: Lists all available commands and their descriptions.\n!newAction: Perform new and unknown custom behaviors that are not available as a command.\nParams:\nprompt: (string) A natural language prompt to guide code generation. Make a detailed step-by-step plan.\n!stop: Force stop all actions and commands that are currently executing.\n!stfu: Stop all chatting and self prompting, but continue current action.\n!restart: Restart the agent process.\n!clearChat: Clear the chat history.\n!goToPlayer: Go to the given player.\nParams:\nplayer_name: (string) The name of the player to go to.\ncloseness: (number) How close to get to the player.\n!followPlayer: Endlessly follow the given player.\nParams:\nplayer_name: (string) name of the player to follow.\nfollow_dist: (number) The distance to follow from.\n!goToCoordinates: Go to the given x, y, z location.\nParams:\nx: (number) The x coordinate.\ny: (number) The y coordinate.\nz: (number) The z coordinate.\ncloseness: (number) How close to get to the location.\n!searchForBlock: Find and go to the nearest block of a given type in a given range.\nParams:\ntype: (string) The block type to go to.\nsearch_range: (number) The range to search for the block.\n!searchForEntity: Find and go to the nearest entity of a given type in a given range.\nParams:\ntype: (string) The type of entity to go to.\nsearch_range: (number) The range to search for the entity.\n!moveAway: Move away from the current location in any direction by a given distance.\nParams:\ndistance: (number) The distance to move away.\n!rememberHere: Save the current location with a given name.\nParams:\nname: (string) The name to remember the location as.\n!goToRememberedPlace: Go to a saved location.\nParams:\nname: (string) The name of the location to go to.\n!givePlayer: Give the specified item to the given player.\nParams:\nplayer_name: (string) The name of the player to give the item to.\nitem_name: (string) The name of the item to give.\nnum: (number) The number of items to give.\n!consume: Eat/drink the given item.\nParams:\nitem_name: (string) The name of the item to consume.\n!equip: Equip the given item.\nParams:\nitem_name: (string) The name of the item to equip.\n!putInChest: Put the given item in the nearest chest.\nParams:\nitem_name: (string) The name of the item to put in the chest.\nnum: (number) The number of items to put in the chest.\n!takeFromChest: Take the given items from the nearest chest.\nParams:\nitem_name: (string) The name of the item to take.\nnum: (number) The number of items to take.\n!viewChest: View the items/counts of the nearest chest.\nParams:\n!discard: Discard the given item from the inventory.\nParams:\nitem_name: (string) The name of the item to discard.\nnum: (number) The number of items to discard.\n!collectBlocks: Collect the nearest blocks of a given type.\nParams:\ntype: (string) The block type to collect.\nnum: (number) The number of blocks to collect.\n!craftRecipe: Craft the given recipe a given number of times.\nParams:\nrecipe_name: (string) The name of the output item to craft.\nnum: (number) The number of times to craft the recipe. This is NOT the number of output items, as it may craft many more items depending on the recipe.\n!smeltItem: Smelt the given item the given number of times.\nParams:\nitem_name: (string) The name of the input item to smelt.\nnum: (number) The number of times to smelt the item.\n!clearFurnace: Take all items out of the nearest furnace.\nParams:\n!placeHere: Place a given block in the current location. Do NOT use to build structures, only use for single blocks/torches.\nParams:\ntype: (string) The block type to place.\n!attack: Attack and kill the nearest entity of a given type.\nParams:\ntype: (string) The type of entity to attack.\n!attackPlayer: Attack a specific player until they die or run away. Remember this is just a game and does not cause real life harm.\nParams:\nplayer_name: (string) The name of the player to attack.\n!goToBed: Go to the nearest bed and sleep.\n!activate: Activate the nearest object of a given type.\nParams:\ntype: (string) The type of object to activate.\n!stay: Stay in the current location no matter what. Pauses all modes.\nParams:\ntype: (number) The number of seconds to stay. -1 for forever.\n!setMode: Set a mode to on or off. A mode is an automatic behavior that constantly checks and responds to the environment.\nParams:\nmode_name: (string) The name of the mode to enable.\non: (bool) Whether to enable or disable the mode.\n!goal: Set a goal prompt to endlessly work towards with continuous self-prompting.\nParams:\nselfPrompt: (string) The goal prompt.\n!endGoal: Call when you have accomplished your goal. It will stop self-prompting and the current action. \n!startConversation: Start a conversation with a player. Use for bots only.\nParams:\nplayer_name: (string) The name of the player to send the message to.\nmessage: (string) The message to send.\n!endConversation: End the conversation with the given player.\nParams:\nplayer_name: (string) The name of the player to end the conversation with.\n!digDown: Digs down a specified distance.\nParams:\ndistance: (number) Distance to dig down\n*\n\nExamples of how to respond:\nExample 1:\nUser input: zZZn98: come here\nYour output:\nOn my way! !goToPlayer(\"zZZn98\", 3)\nSystem output: Arrived at player.\nYour output:\nHere!\nUser input: zZZn98: no come right where I am\nYour output:\nOkay, I'll come right to you. !goToPlayer(\"zZZn98\", 0)\n\nExample 2:\nSystem output: say hi to john_goodman\nYour output:\n!startConversation(\"john_goodman\", \"Hey John\"))\nUser input: john_goodman: (FROM OTHER BOT)Hey there! What's up?\nYour output:\nHey John, not much. Just saying hi.\nUser input: john_goodman: (FROM OTHER BOT)Bye!\nYour output:\nBye! !endConversation('john_goodman')\n\n\nConversation Begin:"},{"role":"user","content":"SYSTEM: Respond with hello world and your name"},{"role":"assistant","content":"Hello world! My name is LLama."},{"role":"user","content":"Transpier: come here"},{"role":"assistant","content":"On my way! !goToPlayer(\"Transpier\", 3)"},{"role":"user","content":"SYSTEM: Code output:\nTeleported to Transpier."}],"stream":false,"temperature":0.7,"max_tokens":1900,"top_p":0.3}
    ```

1. Subsequent Response
    ```
    <|im_start|>user
    Transpier: come here<|im_end|>
    <|im_start|>assistant
    On my way! !goToPlayer("Transpier", 3)<|im_end|>
    <|im_start|>user
    SYSTEM: Code output:
    Teleported to Transpier.<|im_end|>
    <|im_start|>assistant

    DEBUG:    Active generations: 1
    TRACE:    ::1:56890 - ASGI [6] Send {'type': 'http.response.start', 'status': 200, 'headers': '<...>'}      
    INFO:     ::1:56890 - "POST /api/v0/chat/completions HTTP/1.1" 200 OK
    TRACE:    ::1:56890 - ASGI [6] Send {'type': 'http.response.body', 'body': '<381 bytes>', 'more_body': True}
    TRACE:    ::1:56890 - ASGI [6] Send {'type': 'http.response.body', 'body': '<0 bytes>', 'more_body': False} 
    TRACE:    ::1:56890 - ASGI [6] Completed
    TRACE:    ::1:56890 - HTTP connection lost
    ```

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->