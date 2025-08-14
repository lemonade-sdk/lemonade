# Continue.dev

Continue.dev is an open-source AI code assistant for VS Code and JetBrains that enables developers to leverage local AI models through Lemonade Server for code generation, editing, and chat capabilities.

## Prerequisites

Before using Continue.dev with Lemonade Server, ensure you have:

- **IDE**: [Visual Studio Code (1.80+)](https://code.visualstudio.com/) or [JetBrains IDE](https://www.jetbrains.com/ides/)
- **Lemonade Server**: Install and set up following the [Getting Started guide](https://lemonade-server.ai/docs/server/)
- **Server running**: Ensure Lemonade Server is running on `http://localhost:8000`
- **Models installed**: At least one model from the [supported models list](https://lemonade-server.ai/docs/server/server_models/) downloaded locally; this should match the one you will pick below from [Continue Hub](https://hub.continue.dev/lemonade)

## Installation

### Installing Continue Extension

1. Open VS Code
2. Navigate to the Extensions marketplace
3. Search for "Continue" 
4. Install the Continue extension

![Continue Extension in VS Code Marketplace](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_vscode_marketplace.png)

### Configuring with Continue Hub

Continue Hub provides pre-configured model setups that work immediately with Lemonade Server. **Note**: The models shown on Continue Hub must be installed on your local machine to function properly.

1. **Install Models Locally**: First, ensure you have the desired models installed using the [lemonade-server CLI](https://lemonade-server.ai/docs/server/lemonade-server-cli/):
   ```bash
   lemonade-server pull <model-name>
   ```
   For example: `lemonade-server pull Qwen2.5-7B-Instruct-Hybrid`

2. **Access Lemonade Models**: Visit [hub.continue.dev](https://hub.continue.dev/lemonade)
3. **Select Configuration**: Browse available Lemonade models and configurations - all models listed are from the [supported models list](https://lemonade-server.ai/docs/server/server_models/)

![Continue Configuration Settings](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_hub_assistant_configed.png)


4. **Add to Continue**: Click the plus icon `(+)` on your chosen configuration to add to your assistant - see the [Continue.dev assistants guide](https://docs.continue.dev/guides/understanding-assistants) for detailed setup instructions
5. **Automatic Setup**: The configuration is automatically added to your Continue extension upon signing in via the extension in your chosen IDE.

![Configuration Added to Continue](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_extension_assistant.png)

## Working with Continue.dev

Continue.dev provides three interaction modes for different development tasks. See the [Continue documentation](https://docs.continue.dev) for detailed mode descriptions.

### Mode Selection Guide

- **Chat**: Code explanations, debugging discussions, architecture planning
- **Agent**: Multi-file refactoring, large-scale changes across projects

![Continue Modes Interface](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_extension_modes.png)

## Examples

### Example 1: Chat Mode - Building an Asteroids Game

**Scenario**: You want to quickly prototype a game concept through conversation with the AI.

**Chat Conversation**:

> **User**: I want to create an asteroids game using PyGame. What guidelines should I follow in the code to do so?

```
Model: Creating an Asteroids game using PyGame is a fun and educational project! Below are some guidelines you should follow to structure your code effectively:

### 1. Initialize PyGame
- Always start by initializing PyGame and setting up the display.

### 2. Define Game Constants
- Define constants for screen dimensions, colors, speeds, and other game settings.

### 3. Create Classes for Game Objects
- Define classes for different game objects like the Player, Asteroids, Bullets, etc.
- Each class should handle its own rendering and movement.

### 4. Implement Game Loop
- The main game loop should handle updating the game state and rendering the screen.

### 5. Handle User Input
- Implement a way to handle keyboard and mouse inputs to control the player's ship.

### 6. Collision Detection
- Implement collision detection between bullets and asteroids, and between the player ship and asteroids.

### 7. Game Logic
- Implement game logic for spawning asteroids, shooting bullets, and updating the game state.

### 8. Score and Game Over Conditions
- Keep track of the player's score and implement conditions for game over (e.g., when the player's ship collides with an asteroid).

Here's a simple structure to get you started:

# <model generated code here>...

### Explanation of the Code:
1. **Initialize PyGame**: Set up the display and define constants.
2. **Player Class**: Handles the player's movement and drawing.
3. **Asteroid Class**: Handles the asteroid's movement and drawing.
4. **Bullet Class**: Handles the bullet's movement and drawing.
5. **Game Loop**: Processes input, updates game state, and renders the screen.

This code provides a basic framework for an Asteroids game. You can expand on it by adding more features like collision detection, scoring, and different levels of difficulty.
```

> **User**: what change could we make to ensure that the ship is unchanged, but has a yellow trail effect when it moves?

```
Model: To create a yellow trail effect for the player's ship, you can maintain a list of the ship's previous positions and draw small circles or short lines at these positions. This will give the appearance of a trailing effect as the ship moves.

Here's how you can modify the `Player` class and the main game loop to include this feature:

# <model generated>...

### Explanation of Changes:
1. **Player Class**:
   - Added a `trail` list to store the previous positions of the ship.
   - Updated the `move` method to append the current position to the `trail` list and remove the oldest position if the trail exceeds the specified length (`TRAIL_LENGTH`).
   - Updated the `draw` method to draw small yellow circles at each position in the `trail` list.

2. **Game Loop**:
   - No changes were needed in the main game loop as the trail effect is handled within the `Player` class.

This code will create a yellow trail effect behind the player's ship as it moves across the screen. You can adjust the `TRAIL_LENGTH` constant to make the trail longer or shorter.
```

Through this conversational approach, the model helps build and refine the game iteratively, explaining decisions and accommodating new requirements as they arise.

![Chat Mode Game Development](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_extension_asteroids_gen.png)

After running the `pip install pygame` command to setup dependencies, and applying the changes to `main.py`, the game can be run using `python .\main.py`. This results should resemble the following:

![Asteroids Game Result](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_extension_asteroids_result.png)

### Example 2: Agent Mode - Converting Callbacks to Async/Await

**Scenario**: You have older code using callback patterns that needs to be modernized to async/await for better readability and error handling.

**Agent Task**:

> Convert all callback-based functions in `@run.js` use async/await:
> - Convert callback patterns to async/await
> - Add proper try/catch error handling
> - Update all calling code to use await
> - Maintain the same functionality


#### Example Conversion

**Before (callback pattern)**:
```javascript
const db = require('./db');

function processUser(user, callback) {
  setTimeout(() => {
    if (!user || !user.id) {
      callback(new Error('Invalid user'), null);
    } else {
      callback(null, { ...user, processed: true });
    }
  }, 100);
}

function fetchUserData(userId, callback) {
  db.query('SELECT * FROM users WHERE id = ?', [userId], (err, result) => {
    if (err) {
      callback(err, null);
    } else {
      processUser(result, (processErr, processed) => {
        if (processErr) {
          callback(processErr, null);
        } else {
          callback(null, processed);
        }
      });
    }
  });
}
```

**After (async/await)**:
```javascript
const db = require('./db');

async function processUser(user) {
  return new Promise((resolve, reject) => {
  setTimeout(() => {
    if (!user || !user.id) {
        reject(new Error('Invalid user'));
    } else {
        resolve({ ...user, processed: true });
    }
  }, 100);
  });
}

async function fetchUserData(userId) {
  try {
    const result = await new Promise((resolve, reject) => {
  db.query('SELECT * FROM users WHERE id = ?', [userId], (err, result) => {
    if (err) {
          reject(err);
    } else {
          resolve(result);
        }
      });
    });

    const processedUser = await processUser(result);
    return processedUser;
  } catch (err) {
    throw err;
  }
}
```

**Agent Actions**:
1. Identifies all callback-based functions in service files
2. Converts each to async/await syntax
3. Updates error handling to use try/catch blocks
4. Updates all calling code to use await
5. Ensures promise chains are properly handled

The Agent intelligently handles nested callbacks, error propagation, and ensures all calling code is updated consistently.

![Agent Mode Async/Await Conversion](https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/guides/continue/continue_extension_async_result.png)

## Best Practices

### Setup & Configuration
- **Install Lemonade Server** following the [setup guide](https://lemonade-server.ai/docs/server/) before starting development
- **Install models locally** using `lemonade-server pull <model-name>` - see the [supported models list](https://lemonade-server.ai/docs/server/server_models/) for available options
- Pre-load models in Lemonade Server before coding sessions and keep the server running throughout development
- For agent mode with GGUF models, use increased context size: `lemonade-server serve --ctx-size 8192`
- Build custom assistants tailored to specific languages/domains (e.g., JavaScript/TypeScript for Node.js projects)  
- Avoid adding too many models with the same mode to prevent suboptimal model switching
- See [continue.dev customization](https://docs.continue.dev/customization/overview) for effective scoping strategies

### Development Workflow
- Start fresh conversations for new features; clear chat history when switching tasks
- Include only relevant code in prompts to maintain focus
- Structure prompts with clear task descriptions, specific requirements, and technical constraints
- Use `@` symbol to invoke agent mode for multi-file operations
- Evolve prompts from vague ("Create a game") to specific ("Create an Asteroids game in Python using Pygame, under 300 lines, with ship controls and asteroid splitting")
- Generate initial implementation, test immediately, then refine with targeted prompts
- Take advantage of unlimited iterations with local models for continuous improvement

## Common Issues

**Issue**: Model not appearing in Continue  
**Solution**: Verify Lemonade Server is running and model is loaded locally. Check the [supported models list](https://lemonade-server.ai/docs/server/server_models/) and install with `lemonade-server pull <model-name>`

**Issue**: Slow response times  
**Solution**: Ensure model is pre-loaded, check available RAM

**Issue**: Missing error handling in generated code  
**Solution**: Explicitly request "with comprehensive error handling"

**Issue**: Inconsistent code style  
**Solution**: Provide example of desired style in prompt

## Resources

- [Lemonade Server Setup Guide](https://lemonade-server.ai/docs/server/)
- [Lemonade Server Supported Models](https://lemonade-server.ai/docs/server/server_models/)
- [Continue.dev Documentation](https://docs.continue.dev)
- [Continue Hub](https://hub.continue.dev/?type=models&q=lemonade)
- [Lemonade Discord](https://discord.gg/lemonade)
- [Example Projects](https://github.com/lemonade-sdk/examples)
