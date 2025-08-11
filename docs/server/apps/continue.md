# Continue.dev

Continue.dev is an open-source AI code assistant for VS Code and JetBrains that enables developers to leverage local AI models through Lemonade Server for code generation, editing, and chat capabilities.

## Prerequisites

- Visual Studio Code (1.80+) or JetBrains IDE
- Lemonade Server running on `http://localhost:8000`
- At least one model loaded in Lemonade Server

## Installation

### Installing Continue Extension

1. Open VS Code
2. Navigate to the Extensions marketplace
3. Search for "Continue" 
4. Install the Continue extension

![Continue Extension in VS Code Marketplace](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_vscode_marketplace.png)

### Configuring with Continue Hub

Continue Hub provides pre-configured model setups that work immediately with Lemonade Server.

1. **Access Lemonade Models**: Visit [hub.continue.dev](https://hub.continue.dev/?type=models&q=lemonade)
2. **Select Configuration**: Browse available Lemonade models and configurations

![Continue Hub Lemonade Models](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_hub_lemonade.png)

3. **Add to Continue**: Click "Use" on your chosen configuration
4. **Automatic Setup**: The configuration is automatically added to your Continue extension upon signing in

![Configuration Added to Continue](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_assistant.png)

### Adding Lemonade Models to your Assistant

To add Lemonade models to your existing Continue setup:

1. Open the Continue sidebar in VS Code
2. Access the model configuration settings
3. Add your Lemonade Server endpoint and model details

For detailed configuration steps, see the [Continue configuration documentation](https://docs.continue.dev/reference/config).

![Continue Configuration Settings](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_hub_assistant_configed.png)

## Working with Continue.dev

Continue.dev provides three interaction modes for different development tasks. See the [Continue documentation](https://docs.continue.dev) for detailed mode descriptions.

### Mode Selection Guide

- **Chat**: Code explanations, debugging discussions, architecture planning
- **Agent**: Multi-file refactoring, large-scale changes across projects

![Continue Modes Interface](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_modes.png) -->

## Examples

### Example 1: Chat Mode - Building an Asteroids Game

**Scenario**: You want to quickly prototype a game concept through conversation with the AI.

**Chat Conversation**:
```
User: I want to create an asteroids game using PyGame. What guidelines should I follow in the code to do so?

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

# <model genearted code here>...

### Explanation of the Code:
1. **Initialize PyGame**: Set up the display and define constants.
2. **Player Class**: Handles the player's movement and drawing.
3. **Asteroid Class**: Handles the asteroid's movement and drawing.
4. **Bullet Class**: Handles the bullet's movement and drawing.
5. **Game Loop**: Processes input, updates game state, and renders the screen.

This code provides a basic framework for an Asteroids game. You can expand on it by adding more features like collision detection, scoring, and different levels of difficulty.


User: what change could we make to ensure that the ship is unchanged, but has a yellow trail effect when it moves?

Model: To create a yellow trail effect for the player's ship, you can maintain a list of the ship's previous positions and draw small circles or short lines at these positions. This will give the appearance of a trailing effect as the ship moves.

Here's how you can modify the `Player` class and the main game loop to include this feature:

# <model genearted>...

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

![Chat Mode Game Development](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_asteroids_gen.png)

After running the `pip install pygame` command to setup dependencies, and applying the chagnes to `main.py`, the game can be run using

`python .\main.py`

This results in the following:

![Asteroids Game Result](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_asteroids_result.png)

### Example 2: Agent Mode - Converting Callbacks to Async/Await

**Scenario**: You have older code using callback patterns that needs to be modernized to async/await for better readability and error handling.

**Agent Task**:
```
Convert all callback-based functions in @run.js use async/await:
- Convert callback patterns to async/await
- Add proper try/catch error handling
- Update all calling code to use await
- Maintain the same functionality
```

**Example Conversion**:

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

![Agent Mode Async/Await Conversion](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_async_result.png)

## Best Practices

### Working with Local Models

**Context Management**
- Start fresh conversations for new features
- Include only relevant code in prompts
- Clear chat history when switching tasks

**Model Selection & Assistant Scoping**
- Avoid adding too many models to an assistant—Continue may switch to a model it thinks is better, but hardware capabilities aren't always considered.
- Building custom assistants tailored to specific languages or domains (e.g., a JavaScript/TypeScript assistant scoped to Node.js) leads to more relevant results.
- Scope assistants for particular tasks, such as coding with MCP, using specific prompts, or working with targeted data. See [continue.dev customization](https://docs.continue.dev/customization/overview) for more on effective scoping.

**Model Loading**
- Pre-load models in Lemonade Server before coding sessions
- Keep server running throughout development
- Use appropriate model sizes for different tasks

**Iterative Development**
- Generate initial implementation
- Test immediately
- Refine with targeted prompts
- No API costs allow unlimited iterations

### Effective Prompts

**Good Prompt Structure:**
1. Clear task description
2. Specific requirements
3. Technical constraints
4. Using `@` for agent mode

**Example Evolution:**
- Vague: "Create a game"
- Better: "Create an Asteroids game in Python"
- Best: "Create an Asteroids game in Python using Pygame, under 300 lines, with ship controls and asteroid splitting"

## Common Issues

**Issue**: Model not appearing in Continue  
**Solution**: Verify Lemonade Server is running and model is loaded

**Issue**: Slow response times  
**Solution**: Ensure model is pre-loaded, check available RAM

**Issue**: Missing error handling in generated code  
**Solution**: Explicitly request "with comprehensive error handling"

**Issue**: Inconsistent code style  
**Solution**: Provide example of desired style in prompt

## Resources

- [Continue.dev Documentation](https://docs.continue.dev)
- [Continue Hub](https://hub.continue.dev/?type=models&q=lemonade)
- [Lemonade Discord](https://discord.gg/lemonade)
- [Example Projects](https://github.com/lemonade-sdk/examples)