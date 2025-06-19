# AI Dev Gallery with Lemonade Server

## Overview

[AI Dev Gallery](https://aka.ms/ai-dev-gallery) is Microsoft's showcase application that demonstrates various AI capabilities through built-in samples and applications. It provides an easy way to explore and experiment with different AI models and scenarios, including text generation, chat applications, and more.

AI Dev Gallery has native integration with Lemonade Server, which means it can automatically detect and connect to your local Lemonade instance without manual URL configuration.

## Expectations

AI Dev Gallery works well with most models available in Lemonade. The built-in samples are designed to work with various model types and sizes, making it a great tool for testing and exploring different AI capabilities locally.

The application provides a user-friendly interface for experimenting with AI models through pre-built scenarios, making it accessible for both beginners and advanced users.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.
2. **Important**: Make sure your Lemonade Server is running before opening AI Dev Gallery.

### Install AI Dev Gallery

1. Open the Microsoft Store on Windows.
2. Search for "AI Dev Gallery" by Microsoft Corporation.
3. Click "Install" to download and install the application.

Alternatively, you can access AI Dev Gallery directly through [aka.ms/ai-dev-gallery](https://aka.ms/ai-dev-gallery).

### Connect to Lemonade

AI Dev Gallery has native integration with Lemonade Server, so no manual configuration is required. The application will automatically detect your running Lemonade Server instance.

**Important**: Ensure your Lemonade Server is running before launching AI Dev Gallery.

## Usage

AI Dev Gallery provides various built-in applications and samples to explore AI capabilities:

### Quick Start

1. Launch AI Dev Gallery.
2. Navigate to **Samples** → **Text** → **Chat** (or another text/code sample).
3. Click on the model selector above the chat window.
4. Select **Lemonade** from the available providers.
5. Choose your preferred model from the list of available models.

### Supported Scenarios

AI Dev Gallery supports various AI scenarios through its sample applications with Lemonade integration:

**Text Processing**:

- **Conversational AI**: Chat and Semantic Kernel Chat for interactive conversations
- **Content Generation**: Generate text for various purposes and creative writing
- **Language Tasks**: Translation, grammar checking, and paraphrasing
- **Text Analysis**: Sentiment analysis and content moderation
- **Information Retrieval**: Semantic search and retrieval augmented generation
- **Text Enhancement**: Summarization and custom parameter configurations

**Code Assistance**:

- **Code Generation**: Create code snippets and programs
- **Code Analysis**: Explain existing code and understand functionality


### Tips for Best Experience

- Start your Lemonade Server before opening AI Dev Gallery
- Try different models to see how they perform across various scenarios
- Explore different sample categories to understand various AI capabilities
- Use the built-in samples as starting points for your own AI experiments

## Troubleshooting

### AI Dev Gallery doesn't detect Lemonade

- Ensure Lemonade Server is running and accessible at `http://localhost:8000`
- Restart AI Dev Gallery after ensuring Lemonade Server is running

### Models not appearing in the selector

- Open `http://localhost:8000` in a browser and make sure to download the models you want to use through the "Model Manager" tab.

## Additional Resources

- [AI Dev Gallery Website](https://aka.ms/ai-dev-gallery)
- [Lemonade Server Models](../server_models.md)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
