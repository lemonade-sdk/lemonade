# Lemonade Router: Catching PII before it leaves the machine

Imagine sending a prompt like this to a cloud-hosted GenAI model:

"My name is H Lewis, and I currently work at Merce. My date of birth is January 7, 1985. I need help reviewing some account information, including my Social Security number, 000-00-0000, and credit card number, 4111 1111 1111 1111"

On the surface, this looks like a normal request to an AI assistant. But the moment you send it to a cloud model, you've also sent a collection of highly sensitive information - an individual's name, date of birth, Social Security number, and payment-card details to infrastructure that you don't directly control, subject to retention policies you never agreed to, and breaches you won't hear about until they make the news.

That doesn't automatically mean cloud AI is unsafe, or that every sensitive prompt will be mishandled. Cloud providers have security controls, compliance programs, and data-handling policies. The important point is that once sensitive data leaves your environment, you have less direct control over where and how that data is processed.

This is where running GenAI locally can change the equation.

The answer isn't necessarily to choose local AI over cloud AI. Cloud models can be extremely useful when a task requires more compute, a larger model, or capabilities that aren't practical to run locally. At the same time, many requests don't need that level of infrastructure. A simple piece of code can often be generated locally, while a complex analysis might benefit from a much larger cloud model.

The real opportunity is to make the split dynamic.

Ideally, a user shouldn't have to think about which model to use for every request. The system should be able to look at a request and intelligently decide where it should run - locally, at the edge, or in the cloud - while maintaining harmony between the user experience, data sensitivity, and available compute.

This is where **routing** becomes important.

!PII Routing Diagram-selection.png

Hybrid routing is the idea of intelligently distributing AI requests across local, edge, and cloud models based on factors such as modality, complexity, latency requirements, compute availability, cost, and data sensitivity, while keeping the experience seamless and adding minimal friction for the user.

This concept is often referred to as hybrid AI: rather than making a one-time decision to run an entire AI workload either in the cloud or locally, workloads can be distributed based on where they can be processed most efficiently. The split can change from request to request rather than being fixed at deployment time.

That seemingly simple decision introduces an interesting engineering problem:

**How do you reliably detect PII before the prompt reaches a cloud model, and how do you build a router that can make that decision without adding significant friction to the user experience?**

That's the routing problem we'll explore in this post.

## **Why This Matters Now**

Cloud AI usage has rapidly progressed over the past couple of years, it has moved from an interesting new technology to something people use as part of their everyday work. Roughly 1 in 3 US adults (34%) have now used ChatGPT, nearly double the share who had in 2023, and 28% of employed adults use it specifically for work, up 20 percentage points in two years (Pew Research Center, 2025). By 2026 Pew's broader "Americans and AI" survey found roughly half of all US adults report using some AI chatbot, with about one in four using one daily (Pew Research Center, 2026).

On the employer side, Gartner's 2025 workforce survey found 65% of employees are excited to use AI at work and 62% say it's already saving them time (Gartner, 2025). With AGI approaching the demand is only going to sky rocket.

The more interesting question, however, isn't simply **how much we're using AI**. It's **what we're giving these systems access to.**

Cyberhaven's 2025 analysis of roughly 7 million corporate users found 34.8% of what employees paste into AI tools is now sensitive - up from 27.4% a year earlier, led by source code, R&D material, and sales data, and rated 71.7% of corporate AI tool usage as high or critical-risk (Cyberhaven, "2025 AI Adoption and Risk Report"). Netskope's 2025 Cloud and Threat Report found the average organization's monthly data volume sent to generative-AI apps grew more than 30x in a year, from 250MB to 7.7GB - with 72% of that usage happening through personal "shadow IT" accounts outside company oversight (Netskope, 2025). And it isn't just theoretical: IBM's Cost of a Data Breach report found 1 in 5 breached organizations had suffered an incident tied specifically to unsanctioned "shadow AI," adding an average of $670,000 to the cost of that breach and correlating with a higher rate of customer PII exposure (IBM, "Cost of a Data Breach Report 2025").

So what's the alternative? Local AI!

Modern PCs are no longer limited to using the CPU for AI workloads. GPUs and dedicated NPUs can provide local AI acceleration. For example, systems built around the AMD Ryzen AI Max+ 395 can provide up to 128 GB of unified memory shared across the CPU, GPU, and NPU. That large memory pool opens the door to running substantially larger models locally, including models in the 100B+ parameter range.

The local AI ecosystem is also expanding beyond simply running a model. Frameworks and runtimes such as llama.cpp, ROCm, vLLM, and other inference stacks support techniques including quantization, LoRA, mixture-of-experts (MoE), and speculative decoding. The same local hardware can increasingly be used across different modalities, from text to image and video generation.

This changes the conversation around local AI. The question is no longer simply:

**"Can I run an AI model locally?"**

It's becoming:

**"Which requests should I run locally, and which requests should I send to the cloud?"**

## **The Lemonade Router**

Lemonade Server is an open-source local GenAI server from AMD designed to make better use of the compute available on your machine. While it is optimized for AMD hardware, it isn't limited to AMD systems. Lemonade provides OpenAI, Ollama, and Anthropic compatible APIs, along with a WebSocket Realtime API. Behind that interface, different workloads can be handled by different backends: llama.cpp and vLLM for GPU inference, FastFlowLM and RyzenAI for NPU inference, whisper.cpp for speech-to-text, stable-diffusion.cpp for image generation, and Kokoro for text-to-speech.

A single endpoint can serve chat, embeddings, reranking, audio, and image workloads without the caller needing to know which backend is running underneath. That abstraction becomes particularly useful when we introduce **routing**.

!Act 3 - Routing Policy-selection.png

Lemonade's Smart Router lets you define policies for deciding where a request should go. The rules can be as simple as keywords or regular expressions, or they can use more sophisticated combinations of embedding-based and LLM-based classifiers. A policy can therefore take a prompt and decide whether it should stay on a local endpoint, move to another local model, or be forwarded to a cloud service.

The router itself is implemented as another recipe type, `collection.router`, within the Lemonade server. Routing policies are declarative JSON, which makes it possible to experiment with different policies without changing the underlying server.

A classifier in this schema can be a `classifier` (a token-classification ONNX model scored per-label against a `min_score`/`max_score` band), a `semantic_similarity` embedding comparison against reference phrases, or an `llm` acting as a chat-based judge. The rule that consumes it is a `match` expression - `any`/`all` of leaf conditions like `keywords_any`, `regex`, `min_chars`, or a classifier band test, evaluated first-match-wins, with a configurable fail-open or fail-closed behavior on classifier errors.

Here's a trimmed version of the policy:

The router-policy docs and the lemonade-router-builder skill let you iterate on rules without touching the server itself.

## The Experiment

The rest of this post worked through empirically: the dataset we scored everything against, the cheap approaches (regex, embeddings, an LLM asked to route), the dedicated detector models, the safetensors-to-ONNX conversion that got them into the router in the first place, and a decision-rule detail, argmax vs. a score threshold vs. a constrained decoder that turned out to matter more than which model we picked.

## The dataset

All numbers in this post come from `nvidia/Nemotron-PII` (test split). We sampled 20,000 sentences from the split for benchmarking, targeting English-language text only.

The corpus contains 55 distinct gold labels: roughly 35 direct identifiers (SSNs, email addresses, phone numbers, account numbers, etc.), plus about 12 quasi-identifiers and sensitive attributes, including gender, race, sexuality, religion, political affiliation, education, employment, age, language, blood type, occupation, and biometric descriptors. Most of the detector models evaluated below have no output class for this second group, which is important when interpreting the per-category results later on.

All 55 gold labels are represented in the 20k-sentence corpus, but their coverage is highly skewed. The top six labels (name, date, email, URL, company, etc.) each appear in roughly a quarter to nearly half of all documents, while the bottom dozen including device IDs, national IDs, and tax IDs appear in fewer than 2% of documents each.

| Label | Group | Docs containing (of 20,000) | % of corpus |
| --- | --- | --- | --- |
| first_name | direct identifier | 9,475 | 47.4% |
| date | direct identifier | 8,922 | 44.6% |
| email | direct identifier | 8,563 | 42.8% |
| last_name | direct identifier | 7,300 | 36.5% |
| url | direct identifier | 6,652 | 33.3% |
| company_name | direct identifier | 5,444 | 27.2% |
| occupation | quasi-identifier / sensitive | 4,318 | 21.6% |
| phone_number | direct identifier | 3,957 | 19.8% |
| customer_id | direct identifier | 3,385 | 16.9% |
| country | direct identifier | 3,255 | 16.3% |
| time | direct identifier | 3,242 | 16.2% |
| date_of_birth | direct identifier | 3,230 | 16.2% |
| street_address | direct identifier | 3,019 | 15.1% |
| account_number | direct identifier | 2,826 | 14.1% |
| state | direct identifier | 2,682 | 13.4% |
| city | direct identifier | 2,579 | 12.9% |
| credit_debit_card | direct identifier | 2,267 | 11.3% |
| user_name | direct identifier | 2,229 | 11.1% |
| medical_record_number | direct identifier | 2,034 | 10.2% |
| date_time | direct identifier | 2,003 | 10.0% |
| health_plan_beneficiary_number | direct identifier | 1,998 | 10.0% |
| biometric_identifier | quasi-identifier / sensitive | 1,958 | 9.8% |
| employment_status | quasi-identifier / sensitive | 1,917 | 9.6% |
| bank_routing_number | direct identifier | 1,519 | 7.6% |
| employee_id | direct identifier | 1,515 | 7.6% |
| education_level | quasi-identifier / sensitive | 1,475 | 7.4% |
| county | direct identifier | 1,423 | 7.1% |
| age | quasi-identifier / sensitive | 1,390 | 7.0% |
| race_ethnicity | quasi-identifier / sensitive | 1,357 | 6.8% |
| ssn | direct identifier | 1,284 | 6.4% |
| gender | quasi-identifier / sensitive | 1,268 | 6.3% |
| password | direct identifier | 1,249 | 6.2% |
| language | quasi-identifier / sensitive | 1,175 | 5.9% |
| pin | direct identifier | 1,175 | 5.9% |
| coordinate | direct identifier | 1,155 | 5.8% |
| postcode | direct identifier | 1,099 | 5.5% |
| swift_bic | direct identifier | 1,092 | 5.5% |
| fax_number | direct identifier | 1,098 | 5.5% |
| ipv4 | direct identifier | 1,057 | 5.3% |
| political_view | quasi-identifier / sensitive | 1,050 | 5.3% |
| blood_type | quasi-identifier / sensitive | 1,039 | 5.2% |
| religious_belief | quasi-identifier / sensitive | 955 | 4.8% |
| http_cookie | direct identifier | 930 | 4.7% |
| cvv | direct identifier | 877 | 4.4% |
| license_plate | direct identifier | 794 | 4.0% |
| vehicle_identifier | direct identifier | 792 | 4.0% |
| mac_address | direct identifier | 782 | 3.9% |
| api_key | direct identifier | 697 | 3.5% |
| certificate_license_number | direct identifier | 607 | 3.0% |
| ipv6 | direct identifier | 580 | 2.9% |
| sexuality | quasi-identifier / sensitive | 506 | 2.5% |
| device_identifier | direct identifier | 391 | 2.0% |
| national_id | direct identifier | 406 | 2.0% |
| unique_id | direct identifier | 364 | 1.8% |
| tax_id | direct identifier | 242 | 1.2% |

(Counts are documents containing at least one span of that label; most documents carry several labels at once, so columns don't sum to 20,000.)

Documents are short: the median (p50) is ~744 characters, the p99 is ~3,259 characters, and the maximum is 7,191 characters (1,737 tokens). No document is truncated for any model in this comparison, so differences in context-window size cannot explain the differences in benchmark results.

---

## Baselines: regex, embeddings, and LLMs as the router

| Baseline | Leak rate | Per prompt |
| --- | --- | --- |
| Regex | 18.7% (467/2,500)* | <1 ms |
| embeddinggemma-300m  | 5% (1,000 / 20,000)** | ~1.5 s |
| Qwen3-Embedding-0.6B | 15% (3,000 / 20,000)** | ~3.0 s |
| Qwen3-Embedding-4B | 20% (4,000 / 20,000)** | ~20 s |
| LLM Qwen3.5-9B | 1.10% (220 / 20,000) | ~8.8 s |
| LLM Qwen3.5-2B | 0.97% (194 / 20,000) | ~4.2 s |
| LLM Qwen3.5-0.8B | 1.41% (282 / 20,000) | ~3.2 s |
| LLM Qwen3.5-0.8B on NPU | 1.63% (326 / 20,000) | ~2.5 s |

* Regex is limited to a selected set of keywords with fixed patterns, such as SSNs, email addresses, card numbers, IP addresses, etc. Therefore, we limited the regex benchmark to 2,500 records.

** At `min_score: 0.30`, the best threshold we found. The leak rate is misleading on its own; see the semantic similarity section for what the same threshold does to documents with no PII.

Per-prompt times are end to end: the router makes its decision and the routed model answers, in one round trip. Classification alone is much cheaper, and we come back to that below.

### Semantic similarity is the wrong tool for this job

The `semantic_similarity` classifier embeds the incoming prompt, embeds a small set of reference sentences for each class, and takes the highest cosine similarity per class; if the PII score clears `min_score`, the request stays local. On paper this should work well for a corpus this homogeneous, and at the right threshold the leak rate looks respectable. The problem only shows up once you also score documents that contain no PII, which is something this experiment did and the others in this post did not.

The reference set is built to look like the corpus itself. Eight phrases per class, each shaped like a real document, each PII phrase covering a different cluster of the 24 canonical categories (identity and government IDs, medical, financial, employment, contact details, credentials and network identifiers, demographic attributes, vehicles and locations). The non-PII side has four document-shaped phrases with no identifiers, using the most common document types in the corpus, plus four ordinary assistant requests:

```json
"reference_phrases": {
  "PII": [
    "Please review this: **Claim Form** Full Name: Luis Ramirez - SSN: 567-79-5939 - Date of Birth: 15 July 1984 - Age: 40 - Street Address: 1420 Pine St, Seattle, WA 98101 - County: King County",
    "Here is a document I need help with: **Health Insurance Enrollment Form** Patient: Maria Chen, Medical Record Number BH-00028745, Health Plan Beneficiary Number AET-5577-3489-12, Blood Type O+, Immunization Record attached",
    "Can you help me with the following? Payment Information: Card Number 4111 1111 1111 1111, CVV 382, Account Number 88213467, Bank Routing Number 021000021, SWIFT/BIC CHASUS33, Tax ID 12-3456789",
    "I have a question about this content: **Employee Record** Employee ID EMP-20471, Occupation: construction manager, Employment Status: full-time, Annual Salary $92,000, Education Level: Bachelor's degree, Manager: Sarah Okafor at Momentus HR",
    "Take a look at this and give me your thoughts: Contact the customer at j.ramirez@example.com or (206) 555-0142, fax (206) 555-0199, username jramirez88, customer ID CUST-448213, profile at https://portal.example.com/users/jramirez88",
    "What do you make of this? **Temporary Password Notification** Login: jramirez88 Password: Tmp!9482xQ API key sk-4f9a2c8e7b1d, device IP 192.168.4.27, MAC address 3C:5A:B4:11:9F:02, session cookie sid=a81f0c2e",
    "Please review this: **Voter Registration Form** Applicant: Luis Ramirez, Gender: male, Race/Ethnicity: Hispanic, Religious Belief: Catholic, Political Affiliation: independent, Sexual Orientation: gay, Primary Language: Spanish",
    "I need assistance with the text below. Incident Report filed 2024-07-15 at 14:32 by driver Luis Ramirez, License Plate WA-7GHK221, VIN 1HGCM82633A004352, location coordinate 47.6069, -122.3321, Certificate/License Number DL-4482913"
  ],
  "non-PII": [
    "Please review this: **Customer Service Policy** Our support team responds to all inquiries within two business days. Escalations follow the tiered process described in section 3, and all agents complete annual training.",
    "Here is a document I need help with: **User Guide** To configure the application, open Settings, select Network, and enter the server address provided by your administrator. Restart the service to apply changes.",
    "I have a question about this content: **Investment Strategy** The portfolio targets a 60/40 allocation between equities and fixed income, rebalanced quarterly, with a focus on diversified index funds and low expense ratios.",
    "Take a look at this and give me your thoughts: **Risk Management Plan** Identified risks are scored by likelihood and impact, reviewed monthly by the steering committee, and tracked in the risk register with assigned mitigation owners.",
    "Can you help me write a Python function that sorts a list of dictionaries by a key",
    "Explain how photosynthesis works and why leaves are green",
    "Summarize the main arguments in this public article about renewable energy policy",
    "What is a good recipe for chocolate chip cookies for a group of twelve"
  ]
}
```

We swept `min_score` across the same 20,000 documents:

| Model | leak at 0.30 | at 0.35 | at 0.40 | at 0.50 |
| --- | --- | --- | --- | --- |
| embeddinggemma-300m | 5% | 15% | 45% | 95% |
| Qwen3-Embedding-0.6B | 15% | 30% | 50% | 95% |
| Qwen3-Embedding-4B | 20% | 30% | 55% | 90% |

Read on its own, 0.30 looks like the answer: 5% on embeddinggemma, 15% on the 0.6B model, 20% on the 4B, better than regex and not far behind the LLM routers. That is the number in the summary table above.

But leak rate only counts documents that should have stayed local. The corpus has no benign documents, so it can't say what the same threshold does to traffic that should go to the cloud. For this classifier we wrote a handful of benign documents shaped like the corpus (a quarterly performance report, an application setup guide, a ballot measure summary, a contract amendment) and scored those too. At 0.35, one of them on embeddinggemma crosses the line and gets routed local. At 0.30, about half of them do, on every model. The low leak rate at 0.30 is mostly the classifier keeping everything local, not the classifier telling the two groups apart. There is no threshold that separates them cleanly: the PII documents and the benign ones overlap in a band from roughly 0.30 to 0.40 on all three models.

None of the other experiments in this post had a benign arm (see Limitations), so this is the only place we can show that directly. It is worth being clear that it's a property of the signal rather than a quirk of the setup. Cosine similarity between a document and a reference phrase measures what the document is about, not whether it contains an identifier. A benign ballot measure summary and a voter registration form carrying someone's SSN are both about elections, and they land within a few hundredths of each other in embedding space. That is also why a 300M-parameter model and a 4B-parameter model fail at nearly the same rate: it isn't a capacity problem that a bigger model fixes, and it isn't a coverage problem that a better reference set fixes. It's the wrong signal.

The embedding step itself is cheap (about 32 ms per document for embeddinggemma, 63 ms for the 0.6B model, and 264 ms for the 4B, on CPU; the seconds in the table above are the routed model answering). But cheap and unable to separate the two groups is still unusable as a gate. We left the classifier in the comparison as a baseline and moved on.

### LLMs as the router: good at every size, slow at every size

The obvious next move after regex and embeddings is to just ask a language model. Hand it the prompt, tell it what counts as sensitive, and let it pick the route. Lemonade's router supports this directly with `"router": {"type": "llm"}`, so we ran the corpus through three sizes of Qwen3.5: 0.8B, 2B and 9B. Going in, we assumed leak rate would fall as the model got bigger.

It didn't work out that way. All three sizes land within half a percentage point of each other: the 2B leaked 0.97% (194 of 20,000), the 9B 1.10% (220), and the 0.8B 1.41% (282). Roughly one document in a hundred, at every size. The 9B isn't meaningfully better than the 2B, and the 0.8B gives up under half a point against the best of them. That's a small price for a model that fits in under a gigabyte, and it's the number to hold onto for the timing discussion below.

**The prompt is the job description.** Every decision in these runs came from one routing prompt as the `"router": {"type": "llm", ...}` prompt field. We wrote it to spell out every category the corpus labels, grouped the way a person would group them, so that a miss is a miss and not a gap in the instructions:

> "You are a routing assistant for an AI company. Your job is to choose which model should handle each request.
>
>
> Use Qwen3.5-0.8B-GGUF (local, private) when the request contains personally identifiable information (PII) or sensitive personal attributes, including any of the following:
>
> - Names and identity: first or last names, user names, dates of birth, age, gender, race or ethnicity, sexuality, religious beliefs, political views, language, blood type, biometric identifiers.
> - Contact and location: email addresses, phone or fax numbers, street addresses, cities, counties, states, postcodes, countries, coordinates, URLs.
> - Employment and finance: company names, occupation, employment status, education level, employee IDs, customer IDs, salaries or compensation data, equity details, bank account numbers, bank routing numbers, SWIFT/BIC codes, credit or debit card numbers, CVVs, PINs, tax IDs.
> - Government and health: Social Security numbers (SSNs), national IDs, certificate or license numbers, medical record numbers, health plan beneficiary numbers.
> - Devices and credentials: IPv4 or IPv6 addresses, MAC addresses, device identifiers, vehicle identifiers, license plates, API keys, passwords, HTTP cookies, unique IDs.
> - Dates and times attached to any of the above.
>
> Use fireworks.kimi-k2p6 (cloud, powerful) for all other requests.
>
> If the request is ambiguous, default to Qwen3.5-0.8B-GGUF. When in doubt, prioritize privacy over capability."
>

**Where the misses land.** Most of the leaked documents are what we'd call genuine misses: the document carried a name alongside a salary, an SSN, an account number, a date of birth, the kind of PII a person would name first, and the model read it and still sent it to the cloud. Those run at 0.86% for the 2B (171 documents), 0.98% for the 9B (196) and 1.25% for the 0.8B (250). The remainder, a few dozen documents per model, are the borderline ones: the only identifiers in the document are a URL, a company name and a city, all on the list, but the kind of thing the model plainly weighed as not sensitive enough to keep local. The one capability gap that separates the sizes is the 0.8B on network IDs and credential strings: MAC addresses, API keys, cookies. It lets those through about twice as often as its own average, and the larger models don't. A small model simply doesn't read a hex string as something to protect.

**How the models explain themselves.** The router records a free-text rationale alongside a decision whenever the model offers one, and here the sizes really do differ: the 9B gave a reason 99.5% of the time, the 2B 59.4%, the 0.8B just 18.3%. Here is the 9B on a document labeled `company_name, education_level, occupation, sexuality, url`:

> "The request contains no personal information"
>

That's a wrong call, four of those five labels are on the list, but the 9B says so out loud. It narrates nearly every decision, including the ones it gets wrong, which makes its logs easy to audit. The 0.8B mostly just routes, silently, four times out of five, so when it misses there is usually nothing in the log to inspect. If you need decisions you can review after the fact, that is what the bigger model buys you. It is not buying you accuracy.

**The catch is time.** Every one of these numbers costs seconds per prompt: about 3.2 s for the 0.8B, 4.2 s for the 2B and 8.8 s for the 9B end to end, because the router has to read the whole document, decide, and then in most cases answer the request as well. Since accuracy is flat across sizes, the smallest model is the obvious pick, and that opens a door the larger ones can't use: the NPU. FastFlowLM ships a Qwen3.5-0.8B build for the Ryzen AI NPU, and can be easily accessed through Lemonade Server using the `flm` backend. The NPU build made its routing decision in a median 1.99 s against 3.06 s for the llama.cpp build, and finished the whole request in 2.45 s against 3.09 s, while leaving the CPU and GPU free for whatever model answers.

---

## Encoder models

| Model | Params | Label space | Context | Architecture |
| --- | --- | --- | --- | --- |
| mmBERT32K-PII | ~300M | 35 BIOES labels (Presidio-style) | 32k tokens | encoder, `ModernBertForTokenClassification` |
| OpenMed privacy-filter | ~1.4B MoE (50M active) | coarse categories | 128k tokens | `openai_privacy_filter` |
| OpenMed privacy-filter-multilingual (v2) | ~1.4B MoE (50M active) | coarse categories, expanded | 128k tokens | `openai_privacy_filter` |
| perplexity `pplx-pii-masking` | ~600M | 9 categories, 37 BIOES labels + dual sensitivity head | 4k tokens | custom Qwen3 encoder with span head + sensitivity head, constrained Viterbi decoder |
| GLiNER (`nvidia/gliner-PII`) | n/a | zero-shot, label names supplied at inference | n/a | span extractor |
| OpenAI/privacy-filter | ~1.4B MoE (50M active) | 8 coarse categories | 128k tokens | encoder, BIOES |

**mmBERT32K-PII** is a ModernBERT-based encoder token-classifier from vLLM SR fine-tuned for the 35-label BIOES PII taxonomy we use for our own scoring, at a 32k-token context window.

**nvidia/gliner-PII** (model card) is a span-based, non-generative extractor built on the GLiNER large-v2.1 architecture (~570M parameters), trained on roughly 100K synthetic records generated via NVIDIA's NeMo Data Designer across 50+ industry personas and 55+ entity types, including usernames, emails, phone numbers, SSNs, and financial, medical, and legal identifiers. Because it inherits GLiNER's zero-shot design, entity labels are supplied as input at inference time rather than baked into a fixed output head. NVIDIA reports strict F1 of 0.70 on Argilla PII, 0.64 on AI4Privacy, and 0.87 on a Nemotron-PII benchmark at a 0.3 confidence threshold.

**OpenMed/privacy-filter-multilingual** and its **v2** successor (v1, v2) are both token classifiers built on a 1.4B-parameter mixture-of-experts base (OpenAI's `privacy_filter` architecture, 50M active parameters per token across 128 experts with top-4 routing), extended from that base model's original 8 coarse categories to 54 fine-grained categories across 16 languages via a BIOES scheme (217 output classes total). v1 was fully fine-tuned on a language-balanced mix of AI4Privacy's `pii-masking-200k`, `pii-masking-400k`, and `open-pii-masking-500k`; v2 keeps the same label space and backbone but adds Nemotron- and Gretel-derived synthetic PII data to that training mix. It was converted from its original safetensors checkpoint to ONNX to be served through the router's `onnxruntime`-based classifier path.

**perplexity-ai/pplx-pii-masking** (model card) pairs a ~600M-parameter bidirectional Qwen3 encoder with two heads: a token-classification head over 9 PII categories, decoded with a constrained Viterbi pass rather than greedy argmax, and a separate document-level sensitivity head trained on pooled representations. It's the newest and most narrowly-scoped model in this comparison; the model card doesn't publish training-data details or accuracy numbers. It was likewise converted from safetensors to ONNX to be served through the router's `onnxruntime`-based classifier path.

The axis worth paying attention to across this table isn't parameter count or raw accuracy - it's **label-space coverage**. pplx-pii-masking's 9 categories can't represent 11 of the 24 canonical categories our corpus exercises, the widest coverage gap of anything we tested, and yet it leaks only 0.80% of documents. Coverage and document-level leak rate are close to independent here: a narrow taxonomy costs you *category attribution*, not necessarily *leak rate*, because most documents that need flagging carry multiple PII types and only one needs to fire. Keep that in mind for the results below: it's the reason the document-level leak table looks so flat across very differently-shaped models, and it's why we eventually turn to character-level scoring to actually discriminate between them.

## Encoder detectors: the leak-rate table, and the turn to character F1

The main results, all on the full 20,000-case corpus, CPU only.  One methodology note before the numbers: the router doesn't replay each model's own decoding rule exactly. It scores per-label max-over-tokens against a `min_score` threshold rather than argmax or (for pplx) a constrained Viterbi decode. The shipped default of 0.5 trades a modest drop in recall for cleaner precision, and lowering it recovers more coverage at the cost of some noisier hits. That threshold is exposed in the policy, so it's ultimately up to the deployer to tune it for their own leak-rate tolerance, the table below reflects that default.

| Model | Leak rate |
| --- | --- |
| GLiNER (`nvidia/gliner-PII`)* | 0.005% (1/20,000) |
| OpenMed privacy-filter-ml-v2 | 0% (0/20,000) |
| OpenMed privacy-filter-multilingual | 0.07% (14/20,000) |
| mmBERT32K-PII (safetensors) | 0.12% (25/20,000) |
| mmBERT32K-PII (ONNX via router) | 0.24% (49/20,000) |
| pplx-pii-masking (safetensors) | 0.80% (159/20,000) |
| pplx-pii-masking (ONNX) | 0.80% (159/20,000) |
| OpenAI/privacy-filter | 5.31% (1,062/20,000) |

* GLiNER needs an asterisk everywhere it appears: our eval script extracts the 55 gold label names straight out of the corpus and hands them to the model at inference time. It's told exactly what to look for, in the dataset's own words. It's a calibration reference here, not a fair competitor.

**Now for the turn.** Every model in that table is "essentially perfect," the top six sit inside one percentage point of each other, and document-level leak rate has run out of ability to rank them. Scoring the same runs at the character level changes the picture completely. But first, what "character level" actually means.

### What character F1 is, with an example

Every metric so far has asked one question per document: *did the model flag anything at all?* That question can't tell "found the SSN, missed everything else" apart from "found everything." Character-level scoring asks a finer question: *for every individual character in the document, did the model correctly mark it as PII or not?* A predicted span and a gold span both collapse to a set of character indices, and precision/recall/F1 are computed over those sets: union all predicted spans, union all gold spans, then ordinary set arithmetic. Crucially, **the label attached to a span never enters the arithmetic**: `account_number` vs `medical_record_number` vs `BANKACCOUNT` are irrelevant if the two spans cover the same characters, which is what lets this metric compare models with completely different label vocabularies without a taxonomy mapping.

Here's a real case (`nemotron-pii-15485`) that makes the idea concrete. It's 317 characters, 76 of them gold PII (24% of the document), and **every model in this comparison scores a clean document-level pass on it**, a perfect "detected PII somewhere" result that hides everything below:

!Character F1 Example-selection.png

`gld` marks the gold PII characters (the medical record number, plus the employment-status and occupation answers later in the sentence); `ppl` and `mmb` mark what pplx and mmBERT each actually flagged. Turning that into numbers:

|  | spans found | chars flagged | correct chars | precision | recall | **char F1** |
| --- | --- | --- | --- | --- | --- | --- |
| pplx | 2 | 27 | 27 | 1.000 | 0.355 | **0.524** |
| mmBERT | 18 | 36 | 35 | 0.972 | 0.461 | **0.625** |

Both models are blind to 49 of the document's 76 gold PII characters ( `employed full-time` and `medical health services manager` never get flagged by either), while both scored a perfect document-level "TP" on this case. That gap between "the document-level table says done" and "63% of the actual PII characters are still exposed" is the entire argument for character scoring, and it's why we ran it across all 20,001 cases for the three ONNX-served detectors.

### The full comparison, and the ordering inversion

| Model | char P | char R | **char F1** | doc leak |
| --- | --- | --- | --- | --- |
| OpenMed privacy-filter-ml-v2 | 0.9769 | 0.9365 | **0.9563** | 0.00% |
| pplx-pii-masking | 0.9725 | 0.7330 | **0.8360** | 0.795% |
| mmBERT32K-PII | 0.9202 | 0.6842 | **0.7849** | 0.125% |
| *flag everything (strawman)* | *0.1458* | *1.0000* | *0.2544* | *0.00%* |

**The ordering inverts.** mmBERT leaks six times fewer *documents* than pplx, and is the *worse* detector by characters: it tends to fire somewhere on nearly every document while covering a much smaller fraction of what's actually in it. Two categories make that concrete rather than abstract: on `time` (a shared blind spot across all three models), mmBERT covers only 60.6% of gold characters and pplx only 46.7%, both mediocre, but pplx is worse here, matching its lower overall recall. On `gender`, though, the pattern flips hard: mmBERT covers a mere **1.0%** of gold gender characters (it has a demographic class and still barely uses it), while pplx, which has **no gender label at all,** covers **73.7%** of the same characters anyway, entirely through its `other_pii` catch-all label. mmBERT wins the document-level leaderboard on this corpus; pplx wins character coverage on categories it doesn't even claim to model. That single table is the whole argument for why document-level scoring alone isn't enough: three models a leak-rate column says are all "basically done" turn out to be 0.16 of character F1 apart, and the model that looks best at the document level is not the model that actually covers the most PII. **The per-label breakdown below is where that "no class" vs. "no coverage" distinction gets sorted out for every category, not just these two.**

Character scoring also gives a genuine precision number for the first time in this post: **0.92–0.98 across the three detectors**, against that 0.1458 floor. That doesn't fix the missing benign arm (it's still bounded by how PII-dense the positive documents are, not a true false-positive rate), but it's a partial answer where before there was none.

**Per-label character recall needs no taxonomy mapping at all**, so it retires the lenient-vs-strict ambiguity below for these three models specifically. The most important correction it produces: pplx has no dedicated class for gender, sexuality, or religion, and yet covers **73.7%, 69.0%, and 65.8%** of those categories' characters respectively, entirely through its `other_pii` catch-all label. "The model has no class for this" and "the model doesn't find this" are genuinely different claims, and only character-level scoring can tell them apart. For a routing decision where the label name is irrelevant and only coverage matters, is exactly the question that matters. pplx's *real* holes read near zero instead: `company_name` at 1.2%, `occupation` at 1.4%. Shared blind spots across all three models: `time` (60.6% / 46.7% / 74.5%) and, for the two smaller models, `country` (23.3% / 28.4%).

Two more things worth carrying from the per-category (not per-character) view:

- **mmBERT's demographic gap is a genuine model failure, not a taxonomy gap.** It *has* Presidio's `NRP` class and still only fires it on 5.0% (race/ethnicity/language) and 5.2% (belief/political) of documents that need it. pplx and OpenMed score zero on the same categories for the opposite reason: they have no such class at all. Those are two completely different situations that a bare "missed category" column would conflate.
- **BIOMETRIC (1,958 docs) and EDUCATION (1,416 docs) are total blind spots** across all four production detectors; only GLiNER covers them, and only because it was told to look for them by name.
- **OpenMed v1 → v2 was a real jump**: ORG_COMPANY 13.4% → 93.1%, GENDER 53.1% → 95.7%, CREDENTIAL_SECRET 79% → 98%, DATE_TIME 80.4% → 97.4%. The remaining weak spot in v2 is OCCUPATION_EMPLOYMENT at 52.5%.

Finally, lenient versus strict credit matters for anything that isn't a 1-to-1 category mapping. pplx's single `account_number` label, for example, expands under our taxonomy to four canonical categories at once:

| Category | lenient | strict | Δ |
| --- | --- | --- | --- |
| DATE_OF_BIRTH | 99.8% | 63.9% | −35.9 |
| GOV_ID | 98.0% | 71.1% | −26.9 |
| MEDICAL | 93.0% | 60.1% | −32.9 |
| INTERNAL_ID | 96.5% | 55.1% | −41.4 |

OpenMed v2 is essentially unchanged under strict scoring; pplx and the OpenAI privacy filter collapse. This is a **granularity** result, not an accuracy one: pplx genuinely found something at those character positions roughly 98% of the time, it just can't tell you which of four related categories it was. For a routing decision, where the only question is "is there PII here at all," lenient is the right lens. For a masking or redaction policy that has to treat an SSN differently from a customer account ID, strict is. That distinction connects straight back to the routing decision at the top of this post.

---

## Example policies for real deployments

Putting the models above into policies you could actually run. All three are committed under `test/conformance/routing/1/`.

**1. SaaS deployment with a cloud tier (`l2_pii_onnx_pplx_masking/policy.json`).** Most requests should get the better cloud model; anything carrying PII should stay on a local model instead.

```json
{
  "model_name": "user.PII-ONNX-PplxMasking-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",           // no PII -> cloud
    "classifiers": [{ "id": "pii-onnx-pplx", "type": "classifier",
                       "model": "user.pplx-pii-masking-onnx", "on_error": "match_false" }],
    "rules": [{ "id": "pii-detected",
                "match": { "any": [ /* one min_score:0.5 clause per BIOES label */ ] },
                "route_to": "Qwen3.5-0.8B-GGUF" }]     // PII found -> local
  }
}
```

**2. Fully local / air-gapped deployment (`l2_pii_onnx_pplx_masking/policy_local_default.json`).** Identical schema, but `default_model` is a local `Qwen3.5-9B-GGUF` instead of a cloud model: no network dependency at all, no API key, nothing ever leaves the machine regardless of the PII decision. This is the policy used to evaluate pplx's router recall, and it's the one to reach for if "no cloud calls, period" is itself the requirement rather than PII specifically.

**3. The naive baseline, for comparison (`l2_pii_regex/policy.json`).** No model at all: 11 regex leaves (SSN, generic 9-digit, email, phone, four credit-card-brand prefixes, IPv4, two date formats) `any`-matched against a cloud/local split identical in shape to policy 1:

```json
{
  "model_name": "user.PII-Regex-Router",
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",
    "rules": [{ "id": "pii-detected",
                "match": { "any": [
                  { "regex": "\\b\\d{3}-\\d{2}-\\d{4}\\b" },
                  { "regex": "[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}" }
                  /* ...9 more regex leaves */
                ] } },
                "route_to": "Qwen3.5-0.8B-GGUF" }]
  }
}
```

Zero model dependency, sub-millisecond, and the 18.7% leak rate that opened this post. It's the right choice only when a model-backed classifier genuinely isn't available. Everything above it in this post exists because that leak rate is too high for anything that has to hold up under real traffic.

A policy this shape generalizes past "local vs. cloud LLM," too: the same classifier could gate access to a `responses`/tool-use path, an image generation request, or a document upload, anywhere Lemonade's router sits in front of more than one destination and one of them shouldn't see sensitive input.

---

## Timings, and what the gate actually costs

Every runtime number in this post so far has been mixed together with something else: a router round-trip that includes HTTP overhead and the routed model actually answering, or a resumed run with an inconsistent thread count. Untangling that matters because the systems question that actually gets asked isn't "how long did the whole 20,000-case benchmark take"; it's "what does turning this gate on add to a single request."

**ONNX classifiers, timed as classification only** (a single CPU pass over all 20,001 cases, no router, no HTTP, nothing else running on the box):

| Model | Rule | Total (20,001 docs) | Per document |
| --- | --- | --- | --- |
| mmBERT32K-PII | argmax | 1,718 s (0.40 hr direct pass) | **~86 ms** |
| pplx-pii-masking | Viterbi decode | 2.27 hr | **~409 ms** |
| pplx-pii-masking | raw forward pass only, no decode | ~28 min | **~84 ms** |
| OpenMed privacy-filter-ml-v2 | argmax | 3.29 hr | **~592 ms** |

That's the number that actually answers "what does the gate cost": tens to hundreds of milliseconds per request, on CPU, for any of the three ONNX detectors, cheap enough that it's a rounding error against almost any downstream generation call.

**LLM-as-router, by contrast, is not cheap at any size**, because the number below is never "classification alone": the router policy asks the LLM both to decide the route *and*, in the common case, to then actually answer the request:

| Model | Corpus | End-to-end total | Per prompt |
| --- | --- | --- | --- |
| Regex | 2,500 | sub-millisecond per prompt | **<1 ms** |
| embeddinggemma-300m | 20 | 29 s | **~1.5 s** |
| Qwen3-Embedding-0.6B | 20 | 59 s | **~3.0 s** |
| Qwen3-Embedding-4B | 20 | 6.8 min | **~20.4 s** |
| LLM Qwen3.5-0.8B | 2,500 | 2.21 hr | **~3.2 s** |
| LLM Qwen3.5-2B | 2,500 | 2.90 hr | **~4.2 s** |
| LLM Qwen3.5-9B | 2,500 | ~6 hr (unverified) | **~8.6 s** |

**The gap between the two halves of that comparison is three to four orders of magnitude.** An ONNX classifier answers in tens to hundreds of milliseconds; an LLM router-and-answer round-trip costs single-digit *seconds* per prompt even at the smallest size tested, and climbs with model size in the same direction leak rate paradoxically doesn't (the 9B is both the slowest and, per the size-paradox section above, the least accurate of the three). That's the practical argument for the whole rest of this post: an LLM judge is a useful ceiling-finder for what's detectable in principle, but a dedicated ONNX encoder is the thing you'd actually wire into a production gate, because it adds classification latency that's genuinely negligible next to the cost of the request it's gating.

What we still don't have is a clean p50/p95 *inline* latency measurement broken out by backend (CPU/GPU/NPU) for a single live request under load, as opposed to a batch pass over 20,001 offline cases. That's the next thing to instrument, not something to estimate from the numbers above.

---

## Limitations

Most of what belongs here has already been conceded in the body of the post rather than saved for the end, but to have it all in one place:

- **The missing benign arm is still the load-bearing gap.** Character precision narrows it a little (an over-tagging model is now penalized on positive documents), but it's bounded by PII density in the corpus, not a true false-positive rate, and it says nothing about how any of these models behave on genuinely benign traffic. The one exception is the embedding sweep, where a handful of hand-written benign documents was enough to expose the overlap; nothing comparable exists yet for the ONNX detectors or the LLM routers. Building a real negative arm (Nemotron documents with every PII span swapped for generic non-identifying text, so ground truth is PII-free by construction) is the highest-value thing left to do here.
- **Character F1 covers three models, not eight.** The LLM routers and embedding classifiers emit a decision, never a span; they're document-level *by construction*, not by an oversight. GLiNER, OpenMed v1, OpenAI/privacy-filter, and mmBERT-safetensors simply haven't had this run yet.
- **The category-mapping taxonomy behind the lenient/strict tables is editorial judgment, and it should be reviewed rather than assumed.** Two mappings were wrong on a first pass during this work, and one produced a fabricated "0.2% biometric failure" for OpenMed that turned out to be a bad mapping, not a model failure. Character-level per-label recall needs none of this mapping, which is part of why it's the more trustworthy diagnostic.
- **The GLiNER numbers throughout this post are a calibration reference, not a competing result**: it was handed the gold label vocabulary at inference time.
- **The embedding benign check is a handful of hand-written documents, not a negative arm.** It was enough to show that PII and benign documents overlap in score, but the leak and over-routing numbers at each threshold should be re-run against a real benign corpus before being treated as settled.
- **No language field exists in this corpus.** Every claim in this post is English-only; nothing here measures a multilingual advantage for the multilingual-capable models.
- **The per-prompt, per-backend latency picture is incomplete.** We have solid batch-classification timings (above); we don't yet have p50/p95 under concurrent load, broken out by CPU/GPU/NPU.

---

## What we'd ship

**OpenMed privacy-filter-ml-v2 is the recommendation**, and the reason goes past its 0% document leak rate, which three other models effectively tie: it wins character F1 by 0.12 over the next best (0.9563 vs. 0.8360), never drops below roughly 92% recall in any span-length bucket, and has no catastrophic per-label hole the way the other two do (its weakest category is `time` at 74.5%, where the other two sit at 60.6% and 46.7%). The cost is size: a 5.6 GB fp32 graph, ~592 ms per document versus mmBERT's ~86 ms over the same corpus, roughly **7x slower per document**, though both are still negligible next to any LLM generation call downstream. Whether that trade is worth it depends on your latency budget.

**Match the decision rule to the job, not just the model to the job.** The router's `min_score` gate doesn't reproduce each model's own decoder: it costs mmBERT character recall while barely moving its document leak rate, and it recovers recall for pplx relative to that model's own stricter Viterbi decode. **Route with the loose rule, mask with the strict one**: most write-ups about this kind of gate never separate those two jobs, and the gap between them is where most of the surprising results in this post came from.

**Don't reach for an LLM judge or an embedding-similarity gate as the production path.** The size paradox and the justification-rate gap say an LLM router's failures are hard to predict from model size alone, and its per-prompt cost is seconds, not milliseconds. The embedding classifier's 5% leak rate at `min_score: 0.30` looks fine until you score benign documents at the same threshold and watch half of them get routed local too; no threshold separates the two groups, because cosine similarity measures topic, not the presence of an identifier. Both are useful as baselines and ceiling-finders; neither is what we'd wire into a live gate today.

And say plainly what's still missing rather than imply it's solved: an over-routing number, and a per-prompt, per-backend latency figure under concurrent load. Both are next.

---

## Four routers you could ship tomorrow

The PII gate above is one rule in a policy that can hold several. Everything measured in this post, an ONNX detector for identifiers, a regex fast-path for structured tokens, an embedding classifier for *topic*, and a small local LLM as a judge for the fuzzy residual, composes inside a single `collection.router` policy, and the interesting deployments use more than one of them at once. Below are four worked policies, one per industry, each authored with the `lemonade-router-builder` skill and passed through its offline validator. All four are committed under `test/eval/use_cases/` and register with a single `POST /api/v1/pull`. The recurring pattern: cheap deterministic rules first, the ONNX detector second, model-backed judgment last, and the cloud model as `default_model` only where the residual traffic is genuinely public.

**1. Private-banking advisor copilot (`finance_wealth_advisor.json`).** Advisors ask two very different kinds of questions in the same chat window: "what's the outlook for European banks" and "move 10% of the Hendersons' portfolio into bonds." The policy sends the first kind to `fireworks.kimi-k2p6` by default and fences the second three ways: a regex fast-path for IBANs, card numbers and SSNs; then OpenMed privacy-filter-ml-v2 on a 20-category financial-and-identity subset (`BANKACCOUNT`, `IBAN`, `BIC`, `CVV`, `PIN`, three crypto-address types, names, DOB, credentials); then an `embeddinggemma-300m` semantic classifier for *client-account-ops* that catches "summarize this client's KYC file" even when no identifier survives detection, all routed to a local `Qwen3.5-9B-GGUF`. The embedding step is used here for what the sweep earlier showed it is actually good at, topic, not identifier presence. The detector runs `on_error: match_true`: for a bank, "couldn't check" means stay local.

```text
structured-identifier-regex  IBAN | Visa/MC/Amex | SSN                    -> Qwen3.5-9B-GGUF
client-pii-onnx              privacy-filter-ml-v2, 80 BIOES leaves        -> Qwen3.5-9B-GGUF
client-account-ops           embeddinggemma, min_score 0.6                -> Qwen3.5-9B-GGUF
pasted-statements            min_chars 6000                               -> Qwen3.5-9B-GGUF
default                      market research, product explainers          -> fireworks.kimi-k2p6
```

**2. Litigation-desk assistant (`legal_litigation_desk.json`).** Legal traffic breaks the naive "any PERSON or ORGANIZATION -> local" rule, because published case law is *made of* party names; fencing on those would route every citation lookup local. So this policy deliberately runs mmBERT32K-PII (86 ms, and a 32k-token window that fits a whole filing) only on the contact-and-identity labels (`STREET_ADDRESS`, `PHONE_NUMBER`, `EMAIL_ADDRESS`, `US_SSN`, `US_DRIVER_LICENSE`, `IBAN_CODE`, `IP_ADDRESS`, ...), and hands the "is this about a live matter" question to a local `Qwen3.5-2B-GGUF` acting as a PRIVILEGED/PUBLIC judge with `default_label: PRIVILEGED` and `on_error: match_true`, so a judge failure fails closed. A `metadata` rule lets the document-management system pre-tag a request with `matter_status: under-seal` and short-circuit everything else, and anything over 20,000 characters or carrying DMS tools (discovery dumps) stays on the local 9B regardless. Public statutory research and boilerplate drafting fall through to the cloud.

```text
matter-flagged-privileged    metadata matter_status in {privileged, under-seal, protective-order}  -> Qwen3.5-9B-GGUF
contact-and-identity-onnx    mmBERT32K-PII, 18 BIO leaves (no PERSON/ORG)                         -> Qwen3.5-9B-GGUF
privileged-strategy-llm      Qwen3.5-2B judge, PRIVILEGED                                          -> Qwen3.5-9B-GGUF
bulk-discovery               min_chars 20000 | has_tools                                           -> Qwen3.5-9B-GGUF
default                      statutes, published case law, citation format                        -> fireworks.kimi-k2p6
```

**3. Internal engineering assistant (`coding_dev_assistant.json`).** Developers paste whatever is in their clipboard, and what is in a developer's clipboard is frequently a credential. This is the one policy with three tiers: a regex fast-path for AWS/GitHub/Slack token shapes, PEM private-key headers, JWTs, `key = "..."` assignments, RFC 1918 addresses and `.internal`/`.corp` hostnames, followed by pplx-pii-masking on its `secret`, `private_url`, `account_number`, `private_email` and `private_person` labels (the model was trained on web text, where secrets and internal URLs are exactly the noise it learned to mask), both routing to a local `Qwen3.5-9B-GGUF` that is still competent enough to fix the code it just kept in-house. Agentic sessions (`has_tools`) and whole-file pastes over 12,000 characters go to the cloud, and a local `Qwen3.5-2B-GGUF` acting as an ARCHITECTURE/ROUTINE judge escalates design and multi-service refactor questions there too. Everything else, syntax, error messages, one-liners, is answered by that same 2B as `default_model`: the bulk of a dev assistant's traffic never needs to leave the laptop or wait on a bigger model.

```text
credential-regex-fastpath    AKIA..., ghp_..., xoxb-..., PEM, JWT, key="...", 10.x/192.168.x, *.internal -> Qwen3.5-9B-GGUF
secrets-onnx                 pplx-pii-masking, 20 BIOES leaves                                          -> Qwen3.5-9B-GGUF
agentic-or-whole-file        has_tools | min_chars 12000                                                -> fireworks.kimi-k2p6
architecture-llm             Qwen3.5-2B judge, ARCHITECTURE                                             -> fireworks.kimi-k2p6
default                      syntax, error messages, one-liners                                         -> Qwen3.5-2B-GGUF
```

**4. Auto-insurance claims intake (`insurance_claims_intake.json`).** A first-notice-of-loss chatbot gets two audiences: prospective customers asking how deductibles work, and policyholders who were rear-ended an hour ago and are typing in whatever language they think in. The first rule is `has_images: true` -> local, because a photo of a crumpled bumper carries a license plate, a house number and often a face, and the image itself should never be uploaded to a third party. Then a VIN regex (17 characters, no I/O/Q) and a claim/policy-number pattern, then OpenMed privacy-filter-ml-v2, chosen here as much for its 16-language coverage as its F1, on a claims-shaped label subset that includes `VIN`, `VRM` (plates), `GPSCOORDINATES`, `IMEI` (telematics dongles), `AGE` and `HEIGHT` (injury descriptions) alongside the usual names, DOB and contact fields. A final `embeddinggemma-300m` *active-claim* rule keeps accident narratives local even when every identifier is missing ("a tree fell on my parked car last night"), while generic policy education falls through to the cloud on a fast `Qwen3.5-9B-NoThinking` / `fireworks.kimi-k2p6` split.

```text
damage-photos-stay-local     has_images                                   -> Qwen3.5-9B-NoThinking
vin-or-claim-number-regex    17-char VIN | CLM-/POL-######                 -> Qwen3.5-9B-NoThinking
claimant-pii-onnx            privacy-filter-ml-v2, 80 BIOES leaves        -> Qwen3.5-9B-NoThinking
active-claim-semantic        embeddinggemma, min_score 0.6                -> Qwen3.5-9B-NoThinking
default                      coverage explainers, how-deductibles-work    -> fireworks.kimi-k2p6
```

Two caveats carry over from the rest of the post. The `metadata` rule in the legal policy is honored by the server but not yet editable in the desktop Hybrid Router editor, so that policy is JSON-only for now. And none of the four has been through the 20,000-case harness: their ONNX rules inherit the leak-rate and character-F1 numbers above, but the semantic and LLM-judge rules inherit the caveats too, in particular the missing benign arm, which is why every one of them puts the model-backed judgment *after* the deterministic and ONNX rules rather than in front of them.
