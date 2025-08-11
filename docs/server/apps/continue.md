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

![Continue Extension in VS Code Marketplace](../../../../assets/docs/guides/continue/continue_vscode_marketplace.png)
<!-- ![Continue Extension in VS Code Marketplace](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_vscode_marketplace.png) -->

### Configuring with Continue Hub

Continue Hub provides pre-configured model setups that work immediately with Lemonade Server.

1. **Access Lemonade Models**: Visit [hub.continue.dev](https://hub.continue.dev/?type=models&q=lemonade)
2. **Select Configuration**: Browse available Lemonade models and configurations

![Continue Hub Lemonade Models](../../../../assets/docs/guides/continue/continue_hub_lemonade.png)
<!-- ![Continue Hub Lemonade Models](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_hub_lemonade.png) -->

3. **Add to Continue**: Click "Use" on your chosen configuration
4. **Automatic Setup**: The configuration is automatically added to your Continue extension upon signing in

![Configuration Added to Continue](../../../../assets/docs/guides/continue/continue_extension_assistant.png)
<!-- ![Configuration Added to Continue](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_assistant.png) -->

### Adding Lemonade Models to your Assistant

To add Lemonade models to your existing Continue setup:

1. Open the Continue sidebar in VS Code
2. Access the model configuration settings
3. Add your Lemonade Server endpoint and model details

For detailed configuration steps, see the [Continue configuration documentation](https://docs.continue.dev/reference/config).

![Continue Configuration Settings](../../../../assets/docs/guides/continue/continue_hub_assistant_configed.png)
<!-- ![Continue Configuration Settings](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_hub_assistant_configed.png) -->
<!-- Image should show: Continue configuration interface with Lemonade Server settings -->

## Working with Continue.dev

Continue.dev provides three interaction modes for different development tasks. See the [Continue documentation](https://docs.continue.dev) for detailed mode descriptions.

### Mode Selection Guide

- **Chat**: Code explanations, debugging discussions, architecture planning
- **Agent**: Multi-file refactoring, large-scale changes across projects

![Continue Modes Interface](../../../../assets/docs/guides/continue/continue_extension_modes.png)
<!-- ![Continue Modes Interface](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_modes.png) -->
<!-- Image should show: Continue sidebar showing Chat and Agent mode options -->

## Examples

*Note: Examples demonstrate Chat and Agent modes with local Lemonade models. Additional examples with alternative models will be added once testing hardware is available.*

### Example 1: Chat Mode - Building an Asteroids Game

**Scenario**: You want to quickly prototype a game concept through conversation with the AI.

**Chat Conversation**:
```
User: Create a simple asteroids game using pygame

Model: Creating a simple Asteroids game using Pygame involves several steps. Pygame is a popular library for creating games and multimedia applications in Python. Below is a basic implementation of an Asteroids game. This version will include a player-controlled spaceship that can move, rotate, and shoot bullets to destroy asteroids.

First, ensure you have Pygame installed. You can install it using pip if you haven't already:

Now, let's create the game. We'll write the code in a single file.

# generated code ...

User: Add a yellow trail effect behind the ship

Model: To add a yellow trail effect behind the ship, you can keep track of the player's previous positions and draw small circles at those positions. This will create the illusion of a trail. Here's how you can modify the code to include this feature:

# generated code ...

[Model provides the trail implementation code]
```

Through this conversational approach, the model helps build and refine the game iteratively, explaining decisions and accommodating new requirements as they arise.

![Chat Mode Game Development](../../../../assets/docs/guides/continue/continue_extension_asteroids_gen.png)
<!-- ![Chat Mode Game Development](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_asteroids_gen.png) -->

After running the `pip install pygame` command to setup dependencies, and applying the chagnes to `main.py`, the game can be run using

`python .\main.py`

This results in the following:

![Asteroids Game Result](../../../../assets/docs/guides/continue/continue_extension_asteroids_result.png)
<!-- ![Asteroids Game Result](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_asteroids_result.png) -->


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

![Agent Mode Async/Await Conversion](../../../../assets/docs/guides/continue/continue_extension_async_result.png)
<!-- ![Agent Mode Async/Await Conversion](https://github.com/lemonade-sdk/assets/tree/main/docs/guides/continue/continue_extension_async_result.png) -->
<!-- Image should show: Continue Agent interface showing multiple files being converted from callbacks to async/await -->

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