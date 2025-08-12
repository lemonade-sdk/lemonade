// News content data - easy to add new entries
const newsData = [
    {
    title: "AMD, ISV Security Partners Collaborate to Protect the AI PC",
    url: "https://www.amd.com/en/blogs/2025/isv-security-experiences-with-Ryzen-PRO.html",
    date: "August 05, 2025",
    description: "Lemonade Server and Ryzen™ AI NPUs bring real-time, on-device protection against phishing, deepfakes, and prompt attacks. No cloud lag, just smarter security where it counts.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/projects/isv-commercial-security-experiences-blog/security-shield-key-art.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center top;",
    type: "blog"
    },
    {
    title: "Lemonade Server & Open WebUI",
    url: "https://www.youtube.com/watch?v=yZs-Yzl736E",
    date: "July 31, 2025",
    description: "Easily integrate Lemonade Server with Open WebUI to unlock powerful local LLM capabilities on your PC. This video guides you through the installation and setup process.",
    image: "https://img.youtube.com/vi/yZs-Yzl736E/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center top;",
    type: "video"
    },
    {
    title: "Styrk AI & AMD: Guardrails for Your On-Device AI Revolution",
    url: "https://styrk.ai/styrk-ai-and-amd-guardrails-for-your-on-device-ai-revolution/",
    date: "July 14, 2025",
    description: "AMD and Styrk AI bring real-time, on-device LLM security. Powered by AMD NPUs, GPUs, and Lemonade Server, with built-in guardrails for filtering, adversarial detection, and prompt injection defense.",
    image: "https://styrk.ai/wp-content/uploads/2025/07/styrk-ai_amd-768x432.webp",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center top;",
    type: "blog"
    },
    {
    title: "AMD and XMPro Deliver Autonomous Intelligence at the Edge",
    url: "https://www.amd.com/en/developer/resources/technical-articles/2025/empowering-local-industrial-compute.html",
    date: "July 15, 2025",
    description: "AMD and XMPro have partnered to bring advanced AI capabilities to the industrial edge using AMD hardware and the Lemonade Server for efficient, private, local AI workloads.",
    image: "https://www.amd.com/en/developer/resources/technical-articles/2025/empowering-local-industrial-compute/_jcr_content/_cq_featuredimage.coreimg.jpeg/1752678519383/3598267-amd-x-xmpro-image-edit-1200x627-no-copy.jpeg",
    imageStyle: "width: 110%; height: 110%; object-fit: cover; object-position: center top;",
    type: "blog"
    },
    {
    title: "Rethinking Local AI: Lemonade Server’s Python Advantage",
    url: "https://www.amd.com/en/developer/resources/technical-articles/2025/rethinking-local-ai-lemonade-servers-python-advantage.html",
    date: "July 21, 2025",
    description: "Learn about why we chose Python for deploying local LLMs with Lemonade and how integrating with your app is incredibly easy.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/technical-blogs/rethinking-local-ai/figure%203%20our%20approach%20to%20python%20gives%20us%20both%20development%20agility%20and%20the%20expected%20level%20of%20production%20readines.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center top;",
    type: "blog"
    },
    {
    title: "Minions: On-Device and Cloud Language Model Collaboration on AMD Ryzen™ AI",
    url: "https://www.amd.com/en/developer/resources/technical-articles/2025/minions--on-device-and-cloud-language-model-collaboration-on-ryz.html",
    date: "July 08, 2025",
    description: "Minions, a new framework from Stanford’s Hazy Research Group, lets cloud models collaborate with lighter ones on-device—and now runs on Ryzen AI via AMD’s Lemonade Server.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/technical-blogs/minions-on-device-and-cloud-language-model-collaboration-on-ryzen-ai/3546073_Minions_Blog_Banner_1200x600.jpg",
    imageStyle: "width: 110%; height: 110%; object-fit: cover; object-position: center;",
    type: "blog"
    },
    {
    title: "Local Tiny Agents: MCP Agents on Ryzen™ AI with Lemonade Server",
    url: "https://www.amd.com/en/developer/resources/technical-articles/2025/local-tiny-agents--mcp-agents-on-ryzen-ai-with-lemonade-server.html",
    date: "June 10, 2025",
    description: "Model Context Protocol (MCP) is now available on AMD Ryzen™ AI PCs and can be used by installing AMD Lemonade Server and connecting it to projects like Hugging Face's Tiny Agents via streaming tool calls.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/technical-blogs/local-tiny-agents--mcp-agents-on-ryzen-ai-with-lemonade-server/lemonade.png",
    imageStyle: "width: 90%; height: 95%; object-fit: cover; object-position: center; transform: scale(1.2);",
    type: "blog"
    },
    {
    title: "Unlocking a Wave of LLM Apps on Ryzen™ AI through Lemonade Server",
    url: "https://www.amd.com/en/developer/resources/technical-articles/unlocking-a-wave-of-llm-apps-on-ryzen-ai-through-lemonade-server.html",
    date: "April 17, 2025",
    description: "Lemonade Server enables LLM acceleration on Windows and Linux without code changes—using hybrid NPU+iGPU execution on Ryzen™ AI 300-series PCs and GPU acceleration on Linux.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/projects/technical-blogs/3328050-lemonade-server/3328050-lemonade-server-blog.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "blog"
    },
    {
    title: "LLMs on AMD Ryzen™ AI PCs",
    url: "https://www.youtube.com/watch/qMdMJF89c8g",
    date: "March 31, 2025",
    description: "The video explains how Ryzen™ AI 300-series PCs use NPUs and integrated GPUs to accelerate LLMs through hybrid task partitioning, and introduces the Ryzen AI Software Stack.",
    image: "https://img.youtube.com/vi/qMdMJF89c8g/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Running LLMs on AMD Ryzen™ AI PCs using Lemonade SDK",
    url: "https://www.youtube.com/watch/Ys7n5OouwtI",
    date: "March 31, 2025",
    description: "Watch how the Lemonade SDK lets you experiment with LLMs on Ryzen™ AI 300-series PCs using high-level APIs, including setup and prompting via its CLI.",
    image: "https://img.youtube.com/vi/Ys7n5OouwtI/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Using the Lemonade SDK",
    url: "https://www.youtube.com/watch/1_QU_w1zF7Y",
    date: "March 31, 2025",
    description: "Watch to see how the Lemonade SDK can be used to run performance benchmarks and evaluate model accuracy using the MMLU test suite.",
    image: "https://img.youtube.com/vi/1_QU_w1zF7Y/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Integrating Lemonade into your Python App",
    url: "https://www.youtube.com/watch/aeHRGzxxYRQ",
    date: "March 31, 2025",
    description: "This video shows how to use the Lemonade SDK to integrate LLMs into an app, enhancing a basic search tool and running it locally on Ryzen™ AI 300-series PCs.",
    image: "https://img.youtube.com/vi/aeHRGzxxYRQ/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Lemonade Server + Open WebUI",
    url: "https://www.youtube.com/watch/PXNTDZREJ_A",
    date: "March 31, 2025",
    description: "Introducing Lemonade Server with Open WebUI integration. See how easy it is to install Lemonade Server, download models and get Open WebUI running LLMs on your local PC.",
    image: "https://img.youtube.com/vi/PXNTDZREJ_A/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Lemonade Server + Microsoft AI Toolkit",
    url: "https://www.youtube.com/watch/JecpotOZ6qo",
    date: "April 24, 2025",
    description: "Introducing Lemonade Server with Microsoft AI Toolkit integration. See how easy it is to install Lemonade Server, download models and get Microsoft AI Toolkit running LLMs on your local PC.",
    image: "https://img.youtube.com/vi/PXNTDZREJ_A/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Lemonade Server + Continue AI Coding Assistant",
    url: "https://www.youtube.com/watch/bP_MZnDpbUc",
    date: "April 30, 2025",
    description: "Introducing Lemonade Server with Continue AI Coding Assistant integration. See how easy it is to install Lemonade Server, download models and get Continue AI Coding Assistant running LLMs on your local PC.",
    image: "https://img.youtube.com/vi/bP_MZnDpbUc/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Lemonade Server + PEEL for Local LLM Support in PowerShell",
    url: "https://www.youtube.com/watch/A-8QYktB0Io",
    date: "June 6, 2025",
    description: "Introducing Lemonade Server with PEEL for Local LLM Support in PowerShell integration. See how easy it is to install Lemonade Server, download models and get PEEL running LLMs on your local PC.",
    image: "https://img.youtube.com/vi/A-8QYktB0Io/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Introducing Lemonade Server for Local LLM Server with GPU and NPU Acceleration",
    url: "https://www.youtube.com/watch/mcf7dDybUco",
    date: "July 14, 2025",
    description: "See how to install Lemonade Server and run LLMs locally with GPU and NPU acceleration on your PC—no code changes needed.",
    image: "https://img.youtube.com/vi/mcf7dDybUco/maxresdefault.jpg",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "video"
    },
    {
    title: "Hugging Face MCP Course: https://huggingface.co/learn/mcp-course/unit2/lemonade-server",
    url: "https://huggingface.co/learn/mcp-course/unit2/lemonade-server",
    date: "July 8, 2025",
    description: "AMD Partnered with Hugging Face to provide a guide on how to accelerate our end-to-end Tiny Agents application using AMD Neural Processing Unit (NPU) and integrated GPU (iGPU).",
    image: "https://huggingface.co/datasets/mcp-course/images/resolve/main/unit0/1.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center;",
    type: "tutorial"
    },
    {
    title: "GAIA: An Open-Source Project from AMD for Running Local LLMs on Ryzen™ AI",
    url: "https://www.amd.com/en/developer/resources/technical-articles/gaia-an-open-source-project-from-amd-for-running-local-llms-on-ryzen-ai.html",
    date: "March 20, 2025",
    description: "GAIA is an application with multiple agents that runs local LLMs on Ryzen™ AI using Lemonade Server.",
    image: "https://www.amd.com/content/dam/amd/en/images/blogs/designs/projects/technical-blogs/3328050-lemonade-server/3328050-lemonade-server-blog.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "blog"
    },
    {
    title: "Lemonade Server v8.1.0 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.1.0",
    date: "July 30, 2025",
    description: "Support for Ryzen™ AI Software v1.5.0 and NPU-only execution.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v8.0.6 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.0.6",
    date: "July 17, 2025",
    description: "Overhauled llamacpp support in the Lemonade Developer CLI.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v8.0.5 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.0.5",
    date: "July 14, 2025",
    description: "Added device enumeration capability on Windows and Linux to `lemonade system-info` command.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v8.0.4 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.0.4",
    date: "July 09, 2025",
    description: "Added `reranking` and `embeddings` support to Lemonade Server.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v8.0.3 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.0.3",
    date: "June 27, 2025",
    description: "Support for large (sharded) GGUF models. Added `Llama-4-Scout-17B-16E-Instruct-GGUF` to server models list.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v8.0.0 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v8.0.0",
    date: "June 19, 2025",
    description: "Major release with Ubuntu support, model manager, and Windows tray app.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v7.0.2 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v7.0.2",
    date: "June 02, 2025",
    description: "lm-evaluation-harness is now fully integrated as an automated Lemonade CLI tool.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
    {
    title: "Lemonade Server v7.0.1 Release",
    url: "https://github.com/lemonade-sdk/lemonade/releases/tag/v7.0.1",
    date: "May 30, 2025",
    description: "Added support for GGUF models and llama.cpp backend to Lemonade Server.",
    image: "https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/docs/banner.png",
    imageStyle: "width: 100%; height: 100%; object-fit: cover; object-position: center; transform: scale(1.1);",
    type: "release"
    },
];
