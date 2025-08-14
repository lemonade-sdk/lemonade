# OpenHands

[OpenHands](https://github.com/All-Hands-AI/OpenHands) is an open-source AI coding agent. This document explains how to configure OpenHands to target local AI models using Lemonade server for code generation, editing, and chat capabilities. Note that much of this documentation is taken from OpenHands fantastic [guide on running local models](https://docs.all-hands.dev/usage/llms/local-llms), with some additional information on how to integrate with Lemonade Server.

## Prerequisites

- **Docker**: OpenHands leverages Docker containers. To see how to install docker for OpenHands, see their [documentation](https://docs.all-hands.dev/usage/local-setup).
- leverages [Docker](https://www.docker.com/).  
- **Lemonade Server**: Install and set up following the [Getting Started guide](https://lemonade-server.ai/docs/server/)
- **Server running**: Ensure Lemonade Server is running on `http://localhost:8000`
- **Models installed**: At least one model from the [supported models list](https://lemonade-server.ai/docs/server/server_models/) downloaded locally; this should match the one you will pick below from [Continue Hub](https://hub.continue.dev/lemonade)

## Installation

### Installing OpenHands

