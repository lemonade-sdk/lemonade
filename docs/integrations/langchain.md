# LangChain Integration Guide

[LangChain](https://github.com/langchain-ai/langchain) is a popular Python framework for building LLM-powered applications — including RAG pipelines, agents, and chatbots. This guide shows how to connect LangChain to Lemonade Server as a fully local, offline alternative to OpenAI.

---

## Prerequisites

- [Lemonade Server installed and running](https://lemonade-server.ai/docs/install/)
- Python 3.9+
- A model pulled via Lemonade (e.g. `lemonade pull Llama-3.2-3B-Instruct-Hybrid`)

---

## Setup (Under 5 Minutes)

### Step 1 — Install LangChain

```bash
pip install langchain langchain-openai
```

### Step 2 — Configure LangChain to use Lemonade Server

LangChain supports any OpenAI-compatible backend via `ChatOpenAI`. Point it to Lemonade's local server:

```python
from langchain_openai import ChatOpenAI

llm = ChatOpenAI(
    base_url="http://localhost:13305/api/v1",
    api_key="lemonade",           # required by LangChain but unused by Lemonade
    model="Llama-3.2-3B-Instruct-Hybrid",  # any model you have pulled
)
```

### Step 3 — Send your first message

```python
from langchain_core.messages import HumanMessage

response = llm.invoke([HumanMessage(content="What is the capital of France?")])
print(response.content)
# Paris
```

---

## Example 1 — Simple Chat

```python
from langchain_openai import ChatOpenAI
from langchain_core.messages import HumanMessage, SystemMessage

llm = ChatOpenAI(
    base_url="http://localhost:13305/api/v1",
    api_key="lemonade",
    model="Llama-3.2-3B-Instruct-Hybrid",
)

messages = [
    SystemMessage(content="You are a helpful assistant. Be concise."),
    HumanMessage(content="Explain what a vector database is in one sentence."),
]

response = llm.invoke(messages)
print(response.content)
```

---

## Example 2 — RAG Pipeline (Chat with Your Documents)

This example builds a full Retrieval-Augmented Generation pipeline using Lemonade as the LLM backend — fully local and offline.

**Additional prerequisite:** pull the embedding model before running:

```bash
lemonade pull nomic-embed-text-v1-GGUF
```

```bash
pip install langchain langchain-openai langchain-community langchain-chroma langchain-text-splitters pypdf
```

```python
from langchain_openai import ChatOpenAI, OpenAIEmbeddings
from langchain_community.document_loaders import PyPDFLoader
from langchain_text_splitters import RecursiveCharacterTextSplitter
from langchain_chroma import Chroma
from langchain_core.prompts import PromptTemplate
from langchain_core.output_parsers import StrOutputParser
from langchain_core.runnables import RunnablePassthrough

# ── Connect to Lemonade ──────────────────────────────────
LEMONADE_BASE_URL   = "http://localhost:13305/api/v1"
LEMONADE_API_KEY    = "lemonade"
MODEL_NAME          = "Llama-3.2-3B-Instruct-Hybrid"
EMBEDDING_MODEL     = "nomic-embed-text-v1-GGUF"

llm = ChatOpenAI(
    base_url=LEMONADE_BASE_URL,
    api_key=LEMONADE_API_KEY,
    model=MODEL_NAME,
)

# Requires the embedding model to be pulled first:
#   lemonade pull nomic-embed-text-v1-GGUF
# check_embedding_ctx_length=False disables LangChain's OpenAI-specific
# tokenizer check, which fails against non-OpenAI providers.
embeddings = OpenAIEmbeddings(
    base_url=LEMONADE_BASE_URL,
    api_key=LEMONADE_API_KEY,
    model=EMBEDDING_MODEL,
    check_embedding_ctx_length=False,
)

# ── Load and chunk your PDF ──────────────────────────────
loader = PyPDFLoader("your_document.pdf")
docs   = loader.load()

splitter = RecursiveCharacterTextSplitter(chunk_size=500, chunk_overlap=50)
chunks   = splitter.split_documents(docs)

# ── Store in ChromaDB ────────────────────────────────────
vectorstore = Chroma.from_documents(chunks, embeddings)
retriever   = vectorstore.as_retriever(search_kwargs={"k": 3})

# ── Build RAG chain ──────────────────────────────────────
prompt = PromptTemplate.from_template("""
Answer the question using ONLY the context below.
If unsure, say "I don't know based on this document."

Context: {context}
Question: {question}
Answer:
""")

def format_docs(docs):
    return "\n\n".join(doc.page_content for doc in docs)

rag_chain = (
    {"context": retriever | format_docs, "question": RunnablePassthrough()}
    | prompt
    | llm
    | StrOutputParser()
)

# ── Ask questions ────────────────────────────────────────
answer = rag_chain.invoke("What is the main topic of this document?")
print(answer)
```

---

## Example 3 — Prompt Template + Chain

```python
from langchain_openai import ChatOpenAI
from langchain_core.prompts import ChatPromptTemplate
from langchain_core.output_parsers import StrOutputParser

llm = ChatOpenAI(
    base_url="http://localhost:13305/api/v1",
    api_key="lemonade",
    model="Llama-3.2-3B-Instruct-Hybrid",
)

prompt = ChatPromptTemplate.from_template(
    "Summarize the following text in 3 bullet points:\n\n{text}"
)

chain = prompt | llm | StrOutputParser()

result = chain.invoke({"text": "Your long text goes here..."})
print(result)
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Connection refused` | Make sure Lemonade Server is running (`lemonade status`) |
| `Model not found` | Pull the model first: `lemonade pull MODEL_NAME` |
| `Embeddings not working` | Pull the model first: `lemonade pull nomic-embed-text-v1-GGUF`. Make sure `model=` and `check_embedding_ctx_length=False` are set in `OpenAIEmbeddings` |
| Slow responses | Use a smaller model (e.g. 3B instead of 7B) |

---

## Why Use Lemonade with LangChain?

- **100% offline** — no API keys, no internet required after setup
- **Drop-in replacement** — change one URL to switch from OpenAI to local
- **Full LangChain ecosystem** — chains, prompt templates, and RAG pipelines work out of the box
- **Privacy** — your documents never leave your machine

---

## Resources

- [Lemonade Server Documentation](https://lemonade-server.ai)
- [LangChain Documentation](https://python.langchain.com)
- [Lemonade Discord](https://discord.gg/5xXzkMu8Zk)
