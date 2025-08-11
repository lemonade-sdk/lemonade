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

![Continue Extension in VS Code Marketplace](placeholder-image-1.png)
<!-- Image should show: VS Code Extensions view with Continue extension and Install button highlighted -->

### Configuring with Continue Hub

Continue Hub provides pre-configured model setups that work immediately with Lemonade Server.

1. **Access Lemonade Models**: Visit [hub.continue.dev](https://hub.continue.dev/?type=models&q=lemonade)
2. **Select Configuration**: Browse available Lemonade models and configurations

![Continue Hub Lemonade Models](placeholder-image-2.png)
<!-- Image should show: Continue Hub interface displaying Lemonade model options -->

3. **Add to Continue**: Click "Use" on your chosen configuration
4. **Automatic Setup**: The configuration is automatically added to your Continue extension

![Configuration Added to Continue](placeholder-image-3.png)
<!-- Image should show: Continue sidebar in VS Code with Lemonade model appearing in dropdown -->

### Adding Lemonade Models to Default Assistant

To add Lemonade models to your existing Continue setup:

1. Open the Continue sidebar in VS Code
2. Access the model configuration settings
3. Add your Lemonade Server endpoint and model details

For detailed configuration steps, see the [Continue configuration documentation](https://docs.continue.dev/reference/config).

![Continue Configuration Settings](placeholder-image-4.png)
<!-- Image should show: Continue configuration interface with Lemonade Server settings -->

## Working with Continue.dev

Continue.dev provides three interaction modes for different development tasks. See the [Continue documentation](https://docs.continue.dev) for detailed mode descriptions.

### Mode Selection Guide

- **Chat**: Code explanations, debugging discussions, architecture planning
- **Agent**: Multi-file refactoring, large-scale changes across projects

![Continue Modes Interface](placeholder-image-5.png)
<!-- Image should show: Continue sidebar showing Chat and Agent mode options -->

## Examples

*Note: Examples demonstrate Chat and Agent modes with local Lemonade models. Additional examples with alternative models will be added once testing hardware is available.*

### Example 1: Chat Mode - Building an Asteroids Game

**Scenario**: You want to quickly prototype a game concept through conversation with the AI.

**Chat Conversation**:
```
User: I need to create a simple Asteroids game in Python for a demo. Can you help me build it?

Model: I'll help you create an Asteroids game using Pygame. Let me build it with these features:
- Player ship that rotates and thrusts
- Asteroids that split when shot
- Score tracking and game over conditions
- Everything wraps around screen edges

Here's the complete game:
[Model generates ~250 lines of Python/Pygame code]

User: Can you add a yellow trail effect behind the ship?

Model: Sure! I'll add a trail effect by tracking previous ship positions...
[Model provides the trail implementation code]
```

Through this conversational approach, the model helps build and refine the game iteratively, explaining decisions and accommodating new requirements as they arise.

![Chat Mode Game Development](placeholder-image-6.png)
<!-- Image should show: Continue Chat interface with game development conversation -->

### Example 2: Agent Mode - Converting Callbacks to Async/Await

**Scenario**: You have older code using callback patterns that needs to be modernized to async/await for better readability and error handling.

**Agent Task**:
```
Convert all callback-based functions in the data service files to use async/await:
- Convert callback patterns to async/await
- Add proper try/catch error handling
- Update all calling code to use await
- Maintain the same functionality
```

**Example Conversion**:

**Before (callback pattern)**:
```javascript
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
async function fetchUserData(userId) {
  try {
    const result = await db.query('SELECT * FROM users WHERE id = ?', [userId]);
    const processed = await processUser(result);
    return processed;
  } catch (error) {
    throw new Error(`Failed to fetch user data: ${error.message}`);
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

![Agent Mode Async/Await Conversion](placeholder-image-7.png)
<!-- Image should show: Continue Agent interface showing multiple files being converted from callbacks to async/await -->

## Best Practices

### Working with Local Models

**Context Management**
- Start fresh conversations for new features
- Include only relevant code in prompts
- Clear chat history when switching tasks

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
4. Output format

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