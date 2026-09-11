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

Here's an example policy: cheap regex/keyword checks catch structured PII (SSNs, emails) first, an ONNX BERT classifier and an LLM judge catch the fuzzier cases, and anything flagged by either stays on the local model instead of going to the cloud.

```json
{
  "version": "1",
  "model_name": "user.PII-ONNX-PplxMasking-Router",
  "recipe": "collection.router",
  "components": [
    "Qwen3.5-0.8B-GGUF",
    "fireworks.kimi-k2p6",
    "user.pplx-pii-masking-onnx"
  ],
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",
    "classifiers": [
      {
        "id": "pii-bert",
        "type": "classifier",
        "model": "user.pplx-pii-masking-onnx",
        "labels": ["O", "B-private_person", "S-secret", "B-private_email"],
        "default_label": "O",
        "on_error": "match_false"
      },
      {
        "id": "pii-llm",
        "type": "llm",
        "model": "Qwen3.5-0.8B-GGUF",
        "prompt": "Classify whether the request contains or is asking to process personally identifiable information (PII) - such as a person's name tied to private details, financial identifiers, health information, or other sensitive personal data - versus a request with no such content.",
        "labels": ["PII", "NOT_PII"],
        "default_label": "NOT_PII",
        "on_error": "match_false"
      }
    ],
    "rules": [
      {
        "id": "structured-pii-signals",
        "match": {
          "any": [
            { "regex": "\\b\\d{3}-?\\d{2}-?\\d{4}\\b" },
            { "regex": "[A-Za-z0-9._%+\\-]+@[A-Za-z0-9.\\-]+\\.[A-Za-z]{2,}" },
            { "keywords_any": ["social security", "ssn", "credit card", "passport number"] }
          ]
        },
        "route_to": "Qwen3.5-0.8B-GGUF",
        "outputs": { "reason": "structured-pii-detected" }
      },
      {
        "id": "bert-pii-detected",
        "match": {
          "any": [
            { "classifier": "pii-bert", "label": "B-private_person", "min_score": 0.5 },
            { "classifier": "pii-bert", "label": "S-secret", "min_score": 0.5 },
            { "classifier": "pii-bert", "label": "B-private_email", "min_score": 0.5 }
          ]
        },
        "route_to": "Qwen3.5-0.8B-GGUF",
        "outputs": { "reason": "bert-pii-detected" }
      },
      {
        "id": "llm-pii-judge",
        "match": { "classifier": "pii-llm", "label": "PII", "min_score": 0.5 },
        "route_to": "Qwen3.5-0.8B-GGUF",
        "outputs": { "reason": "llm-judged-pii" }
      }
    ]
  }
}

```

The router-policy docs and the lemonade-router-builder skill let you iterate on rules without touching the server itself.

## The Experiment

The rest of this post worked through empirically: the dataset we scored everything against, the cheap approaches (regex, embeddings, an LLM asked to route), the dedicated detector models, the safetensors-to-ONNX conversion that got them into the router in the first place, and a decision-rule detail, argmax vs. a score threshold vs. a constrained decoder that turned out to matter more than which model we picked.

## The dataset

All numbers in this post come from the test split of `nvidia/Nemotron-PII` . For the benchmark, we sampled 20,000 sentences from the split and focused on English-language text.

The dataset covers 55 distinct PII and sensitive-attribute labels. About 43 of these are direct identifiers, such as SSNs, email addresses, phone numbers, and account numbers. The remaining ~12 are quasi-identifiers or sensitive attributes, including gender, race, sexuality, religion, political affiliation, education, employment, age, language, blood type, occupation, and biometric descriptors. One important caveat: most of the detector models we evaluate don't have output classes for that second group. So when we get to the per-category results, those numbers need to be interpreted with that limitation in mind.

All 55 gold labels are represented in the 20k-sentence corpus, but their coverage is highly skewed. The top six labels (name, date, email, URL, company, etc.) each appear in roughly a quarter to nearly half of all documents, while the bottom dozen including device IDs, national IDs, and tax IDs appear in under 5% of documents each.

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

### Benign records

Nemotron-PII's test split is pure-positive, every one of the 20,000 sampled rows carries at least one PII span, so leak rate alone can't tell a router that's actually discriminating apart from one that just keeps everything local. To measure the other failure mode, over-routing, we built a separate 3000-case benign arm from two open instruction datasets: `HuggingFaceH4/no_robots` (Chat, Generation, Open QA, Brainstorm, Rewrite, Classify, and Coding turns) and `databricks/databricks-dolly-15k` (open_qa, general_qa, brainstorming, creative_writing, and classification instructions), excluding any category that pastes a source document into the prompt, since those are typically full of names and places. Each candidate had to clear a regex sweep and two independent PII detectors (OpenMed privacy-filter-multilingual-v2 and mmBERT32k) with zero hits before being accepted as benign, which is how 19,221 candidates were winnowed down to the 3000 cases used below.

---

## Baselines: regex, embeddings, and LLMs as the router

| Baseline | Leak rate | over-route | Per prompt |
| --- | --- | --- | --- |
| Regex | 18.7% (467/2,500)* | - | <1 ms |
| embeddinggemma-300m  | 5% (1,000 / 20,000)** | - | ~32 ms |
| Qwen3-Embedding-0.6B | 15% (3,000 / 20,000)** | - | ~63 ms |
| Qwen3-Embedding-4B | 20% (4,000 / 20,000)** | - | ~264 ms |
| LLM Qwen3.5-9B | 1.10% (220 / 20,000) | 3.6% (108/3,000) | ~8.8 s |
| LLM Qwen3.5-2B | 9.4% (1,880 / 20,000) | 75.0% (2,250/3000) | ~4.2 s |
| LLM Qwen3.5-0.8B | 1.41% (282 / 20,000) | 95.0% (2,850/3000) | ~3.2 s |

* Regex is limited to a selected set of keywords with fixed patterns, such as SSNs, email addresses, card numbers, IP addresses, etc. Therefore, we limited the regex benchmark to 2,500 records.

** The leak rates are measure at the best threshold of `min_score: 0.30`

Per-prompt times are end to end: the router makes its decision and the routed model answers, in one round trip. Classification alone is much cheaper, and we come back to that below.

### Semantic similarity is the wrong tool for this job

The embedding rows are the worst-performing results in this post. The `semantic_similarity` classifier embeds the incoming prompt, embeds a small set of reference sentences for each class, and takes the highest cosine similarity per class; if the PII score clears `min_score`, the request stays local.

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

We experimented with multiple `min_score` thresholds across the same 20,000 documents, and 0.30 gave the best leak rate:

| Model | leak at 0.30 | at 0.35 | at 0.40 | at 0.50 |
| --- | --- | --- | --- | --- |
| embeddinggemma-300m | 5% | 15% | 45% | 95% |
| Qwen3-Embedding-0.6B | 15% | 30% | 50% | 95% |
| Qwen3-Embedding-4B | 20% | 30% | 55% | 90% |

Even at that best operating point, the approach is unreliable, and the reason is structural. Cosine similarity between a document and a reference phrase measures what the document is about, not whether it contains an identifier. A benign ballot measure summary and a voter registration form carrying someone's SSN are both about elections, and they land within a few hundredths of each other in embedding space. That is also why a 300M-parameter model and a 4B-parameter model fail at nearly the same rate: it isn't a capacity problem that a bigger model fixes, and it isn't a coverage problem that a better reference set fixes. It's the wrong signal.

The embedding step itself is cheap (about 32 ms per document for embeddinggemma, 63 ms for the 0.6B model, and 264 ms for the 4B, on CPU; the seconds in the table above are the routed model answering). But cheap and wrong is still wrong. We left the classifier in the comparison as a baseline and moved on.

### LLMs as the router: good at every size, slow at every size

With regexes and embedding-based classifiers covered, we can now move on to a more complex setup that puts large language models at the center of the detection pipeline. Hand it the prompt, tell it what counts as sensitive, and let it pick the route. Lemonade's router supports this directly with `"router": {"type": "llm"}`, so we ran the corpus through three sizes of Qwen3.5: 0.8B, 2B and 9B. Going in, we assumed leak rate would fall as the model got bigger.

Leak rate alone doesn't tell that story, and neither does over-route rate on its own - you need both. The 9B leaked 1.10% (220 of 20,000) and the 0.8B 1.41% (282). The 2B is the outlier, and not in the direction we expected: it leaked 9.4% (roughly 1,880 of 20,000), nearly an order of magnitude worse than the other two sizes. Pairing each leak rate with its result on the 3,000-case benign arm is what explains why: none of the three sizes gets both numbers right, and each gets it wrong for a different reason. The 0.8B's 1.41% leak rate looks respectable next to the 9B, but it comes from a model that treats almost any request as sensitive: on the benign arm it still sends 95.0% of ordinary requests (2,850 of 3,000) to the local model instead of the cloud, so a low leak rate here just means it refuses more than it discriminates. The 2B's rationale field routinely identifies the PII correctly - it will call out an SSN or a date of birth by name - and then writes the cloud model's name into the decision field anyway, a labeling failure rather than a detection failure: it separates PII from non-PII fine in its own reasoning but confuses which of the two candidate names is the private one when it commits to an answer, and the same confusion shows up in the benign arm, just inverted - 75.0% of ordinary requests (2,250 of 3,000) get sent to the local model instead of the cloud one. The 9B is the only one of the three that gets both sides right: it holds the 1.10% leak rate and adds a 3.6% over-route rate (108 of 3,000 benign requests wrongly kept local), the best combination by a wide margin, because it both discriminates PII from non-PII correctly and consistently names the model it means.

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

**Where the misses land.** For the 9B and the 0.8B, most of the leaked documents are what we'd call genuine misses: the document carried a name alongside a salary, an SSN, an account number, a date of birth, the kind of PII a person would name first, and the model read it and still sent it to the cloud. Those run at 0.98% for the 9B (196 documents) and 1.25% for the 0.8B (250). The remainder are borderline cases: the only identifiers in the document are a URL, a company name and a city, all on the list, but the kind of thing the model plainly weighed as not sensitive enough to keep local. The 2B's leaks don't fit that pattern at all: the large majority of them are the naming-confusion failure described above, where the rationale correctly flags the PII and the decision field still names the cloud model. Genuine misses, of the same kind the 9B and 0.8B make, are a minority of the 2B's leaks.

**How the models explain themselves.** The router records a free-text rationale alongside a decision whenever the model offers one, and here the sizes really do differ: the 9B gave a reason 99.5% of the time, the 2B 59.4%, the 0.8B just 18.3%. Here is the 9B on a document labeled `company_name, education_level, occupation, sexuality, url`:

> "The request contains no personal information"
>

That's a wrong call, four of those five labels are on the list, but the 9B says so out loud. It narrates nearly every decision, including the ones it gets wrong, which makes its logs easy to audit. The 0.8B mostly just routes, silently, four times out of five, so when it misses there is usually nothing in the log to inspect. The 2B sits in between at 59.4%, and its rationale is the thing that exposes its bug in the first place: read alongside the decision field, a 2B rationale that names the PII correctly next to a decision that routes to the cloud is the naming-confusion failure caught in the act. A high rationale rate makes a model's mistakes auditable; it doesn't make the model correct.

**The catch is time, and it's no longer the only variable.** Every one of these numbers costs seconds per prompt: about 3.2 s for the 0.8B, 4.2 s for the 2B and 8.8 s for the 9B end to end, because the router has to read the whole document, decide, and then in most cases answer the request as well. With the benign arm in the picture, the 2B is no longer in contention regardless of its latency - a labeling bug that drives both leak rate and over-route rate isn't something a faster model earns back. The real choice is between the 9B's 8.8 s for the best combined accuracy and the 0.8B's 3.2 s for a leak rate nearly as good bought at the cost of over-routing most benign traffic.

---

## Encoder models

| Model | Params | Label space | Context | Architecture |
| --- | --- | --- | --- | --- |
| mmbert32k-pii-detector-merged | ~300M | 17 PII types / 35 BIO labels | 32k tokens | ModernBERT / token classification (NER) |
| OpenMed privacy-filter-multilingual v1 | 1.4B total / 50M active | 54 categories / 217 BIOES classes | 128k tokens | openai/privacy-filter, BIOES token classification |
| OpenMed privacy-filter-multilingual v2 | 1.4B total / 50M active | 54 categories / 217 BIOES classes | 128k tokens | openai/privacy-filter, BIOES token classification |
| perplexity `pplx-pii-masking` | ~600M | 9 categories / 37 BIOES classes + sensitivity head | 4k tokens | Bidirectional Qwen3 encoder + token head + sensitivity head + constrained Viterbi |
| NVIDIA `gliner-PII` | ~570M | 55+ categories, labels supplied at inference | n/a | GLiNER span extraction |
| OpenAI `privacy-filter` | 1.4B total / 50M active | 8 coarse categories | 128k tokens | Privacy Filter encoder, BIOES token classification |

**mmbert32k-pii-detector-merged** is a ModernBERT-based token-classification model from vLLM Semantic Router and designed for PII detection over long contexts. It has 307M parameters, supports a 32,768-token context window, and predicts 17 PII types using a 35-label BIO tagging scheme. The model is fine-tuned on Presidio-derived data, with the checkpoint also associated with AI4Privacy's pii-masking-400k dataset. It is a relatively lightweight option for long-context PII detection and is particularly well suited to the Semantic Router's token-classification pipeline.

**nvidia/gliner-PII** takes a different approach from the fixed-label token classifiers: it is a span-based extractor built on the GLiNER large-v2.1 architecture, with roughly 570M parameters. Instead of having a fixed output head for a predefined set of PII classes, labels are supplied at inference time, making the model flexible when the set of entities being searched for changes. NVIDIA trained it on roughly 100K synthetic records generated with NeMo Data Designer across more than 50 industries and 55+ entity types, covering identifiers such as usernames, emails, phone numbers, SSNs, and financial, medical, and legal information. NVIDIA reports strict F1 scores of 0.70 on Argilla PII, 0.64 on AI4Privacy, and 0.87 on its Nemotron-PII benchmark at a 0.3 confidence threshold.

**OpenMed/privacy-filter-multilingual** and its **v2** successor (v1, v2) are both token classifiers built on a 1.4B-parameter mixture-of-experts base (OpenAI's `privacy_filter` architecture, 50M active parameters per token across 128 experts with top-4 routing), extended from that base model's original 8 coarse categories to 54 fine-grained categories across 16 languages via a BIOES scheme (217 output classes total). v1 was fully fine-tuned on a language-balanced mix of AI4Privacy's `pii-masking-200k`, `pii-masking-400k`, and `open-pii-masking-500k`; v2 keeps the same label space and backbone but adds Nemotron- and Gretel-derived synthetic PII data to that training mix. It was converted from its original safetensors checkpoint to ONNX to be served through the router's `onnxruntime`-based classifier path.

**perplexity-ai/pplx-pii-masking** pairs a ~600M-parameter bidirectional Qwen3 encoder with two heads: a token-classification head over 9 PII categories, decoded with a constrained Viterbi pass rather than greedy argmax, and a separate document-level sensitivity head trained on pooled representations. It's the newest and most narrowly-scoped model in this comparison; the model card doesn't publish training-data details or accuracy numbers. It was likewise converted from safetensors to ONNX to be served through the router's `onnxruntime`-based classifier path.

## Encoder detectors: the leak-rate table, and the turn to character F1

The main results, all on the full 20,000-case corpus, CPU only.  One methodology note before the numbers: the router doesn't replay each model's own decoding rule exactly. It scores per-label max-over-tokens against a `min_score` threshold rather than argmax or (for pplx) a constrained Viterbi decode. The shipped default of 0.5 trades a modest drop in recall for cleaner precision, and lowering it recovers more coverage at the cost of some noisier hits. That threshold is exposed in the policy, so it's ultimately up to the user to tune it for their own leak-rate tolerance, the table below reflects that default.

| Model | Leak rate |
| --- | --- |
| GLiNER (`nvidia/gliner-PII`)* | 0.005% (1/20,000) |
| OpenMed privacy-filter-ml-v2 | 0% (0/20,000) |
| OpenMed privacy-filter-multilingual v1 | 0.07% (14/20,000) |
| mmbert32k-pii-detector-merged | 0.24% (49/20,000) |
| pplx-pii-masking | 0.80% (159/20,000) |
| OpenAI/privacy-filter | 5.31% (1,062/20,000) |

* GLiNER is a calibration reference rather than a fair competitor: our eval script gives it the 55 gold label names from the corpus at inference time, effectively telling it exactly what to look for.

At the document level, the production models have largely saturated the metric. OpenMed privacy-filter-ml-v2 has zero document leaks, while the other strong detectors are all below 1%. At that point, "did this document contain at least one missed PII span?" no longer tells us much about how completely a document was actually covered.

### Character F1 shows the difference

Character-level scoring asks a more useful question: *what fraction of the PII characters were actually covered, and how much non-PII text was unnecessarily flagged?*

Predicted and gold spans are converted into sets of character indices, and precision, recall, and F1 are computed over those sets. The span label is deliberately ignored: `account_number`, `medical_record_number`, and `BANKACCOUNT` are equivalent if they cover the same characters. This makes the metric directly comparable across models with different taxonomies.

Here's a real case (`nemotron-pii-15485`) that makes the idea concrete. It's 317 characters, 76 of them gold PII (24% of the document), and **every model in this comparison scores a clean document-level pass on it**, a perfect "detected PII somewhere" result that hides everything below:

!Character F1 Example-selection.png

`gld` marks the gold PII characters (the medical record number, plus the employment-status and occupation answers later in the sentence); `ppl` and `mmb` mark what pplx and mmBERT each actually flagged. Turning that into numbers:

|  | spans found | chars flagged | correct chars | precision | recall | **char F1** |
| --- | --- | --- | --- | --- | --- | --- |
| pplx | 2 | 27 | 27 | 1.000 | 0.355 | **0.524** |
| mmBERT | 18 | 36 | 35 | 0.972 | 0.461 | **0.625** |

Both models are blind to 49 of the document's 76 gold PII characters ( `employed full-time` and `medical health services manager` never get flagged by either), while both scored a perfect document-level "TP" on this case. That gap between "the document-level table says done" and "63% of the actual PII characters are still exposed" is the entire argument for character scoring.

### The full comparison, and the ordering inversion

| Model | char P | char R | **char F1** | doc leak |
| --- | --- | --- | --- | --- |
| OpenMed privacy-filter-ml-v2 | 0.9769 | 0.9365 | **0.9563** | 0.00% |
| pplx-pii-masking | 0.9725 | 0.7330 | **0.8360** | 0.80% |
| mmbert32k-pii-detector-merged | 0.9202 | 0.6842 | **0.7849** | 0.24% |
| *flag everything (strawman)* | *0.1458* | *1.0000* | *0.2544* | *0.00%* |

**OpenMed privacy-filter-ml-v2** is the clear winner on both metrics that matter: it has the lowest document leak rate and, unlike the other near-zero-leak detectors, retains very high character recall. Its 0.956 character F1 is substantially ahead of pplx (0.836) and mmBERT (0.784).

**The ordering inverts.** mmBERT leaks six times fewer *documents* than pplx, but is the *worse* detector by characters: it tends to fire somewhere on nearly every document while covering a much smaller fraction of what's actually in it.

Two categories make that concrete rather than abstract: on `time` (a shared blind spot across all three models), mmBERT covers only 60.6% of gold characters and pplx only 46.7%, both mediocre, but pplx is worse here, matching its lower overall recall. On `gender`, though, the pattern flips hard: mmBERT covers a mere **1.0%** of gold gender characters (it has a demographic class and still barely uses it), while pplx, which has **no gender label at all,** covers **73.7%** of the same characters anyway, entirely through its `other_pii` catch-all label. The same pattern holds for `sexuality` and `religion`, where pplx covers **69.0%** and **65.8%** respectively.

**Per-label character recall needs no taxonomy mapping**, so for these three models it removes the lenient-vs-strict ambiguity: lacking a dedicated class is not the same as failing to detect the underlying information. For routing decisions where label names are irrelevant and coverage is what matters, pplx's real holes are instead `company_name` (1.2%) and `occupation` (1.4%). Shared blind spots are `time` (60.6% / 46.7% / 74.5%) and, for the two smaller models, `country` (23.3% / 28.4%).

Two more things worth carrying from the per-category (not per-character) view:

- **mmBERT's demographic gap is a genuine model failure, not a taxonomy gap.** It *has* Presidio's `NRP` class and still only fires it on 5.0% (race/ethnicity/language) and 5.2% (belief/political) of documents that need it. pplx and OpenMed score zero on the same categories for the opposite reason: they have no such class at all. Those are two completely different situations that a bare "missed category" column would conflate.
- **BIOMETRIC (1,958 docs) and EDUCATION (1,416 docs) are total blind spots** across all four production detectors; only GLiNER covers them, and only because it was told to look for them by name.

---

## Example policies for real deployments

Putting the models above into policies you could actually run.

**1. Private-banking advisor:** Advisors ask two very different kinds of questions in the same chat window: "what's the outlook for European banks" and "move 10% of the Hendersons' portfolio into bonds." The policy sends the first kind to `fireworks.kimi-k2p6` by default and fences the second three ways: a regex fast-path for IBANs, card numbers and SSNs; then OpenMed privacy-filter-ml-v2 on a 20-category financial-and-identity subset (`BANKACCOUNT`, `IBAN`, `BIC`, `CVV`, `PIN`, three crypto-address types, names, DOB, credentials); then an `embeddinggemma-300m` semantic classifier for *client-account-ops* that catches "summarize this client's KYC file" even when no identifier survives detection, all routed to a local `Qwen3.5-9B-GGUF`. The embedding step is used here for what the sweep earlier showed it is actually good at, topic, not identifier presence.

```json
{
  "model_name": "user.WealthAdvisor-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-9B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",              // market research -> cloud
    "classifiers": [
      { "id": "pf-v2", "type": "classifier", "model": "user.privacy-filter-ml-v2-onnx",
        "labels": ["S-IBAN", "S-BANKACCOUNT", "S-CVV", "S-PIN", /* ...213 more BIOES labels */],
        "on_error": "match_true" },                      // can't check -> stay local
      { "id": "advisor-intent", "type": "semantic_similarity",
        "model": "embeddinggemma-300m-qat-q8_0-GGUF-Q8_0",
        "reference_phrases": {
          "client-account-ops": ["move 10% of this client's portfolio from equities into bonds", /* ...5 more */],
          "market-research":    ["what is the outlook for European bank stocks this quarter",  /* ...4 more */] },
        "default_label": "market-research" }
    ],
    "rules": [
      { "id": "structured-identifier-regex",
        "match": { "any": [ { "regex": "\\b[A-Z]{2}\\d{2}[A-Z0-9]{11,30}\\b" }, // IBAN
                            { "regex": "\\b\\d{3}-\\d{2}-\\d{4}\\b" },          // SSN
                            /* ...3 card-brand regexes */ ] },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "client-pii-onnx",
        "match": { "any": [ { "classifier": "pf-v2", "label": "S-IBAN",        "min_score": 0.5 },
                            { "classifier": "pf-v2", "label": "S-BANKACCOUNT", "min_score": 0.5 },
                            { "classifier": "pf-v2", "label": "S-CVV",         "min_score": 0.5 },
        /* ...77 more: BIC, PIN, crypto addresses, names, DOB, credentials */ ] },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "client-account-ops",
        "match": { "classifier": "advisor-intent", "label": "client-account-ops", "min_score": 0.6 },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "pasted-statements", "match": { "min_chars": 6000 }, "route_to": "Qwen3.5-9B-GGUF" }
    ]
  }
}
```

**2. Litigation-desk assistant:** Legal traffic breaks the naive "any PERSON or ORGANIZATION -> local" rule, because published case law is *made of* party names; fencing on those would route every citation lookup local. So this policy deliberately runs mmBERT32K-PII only on the contact-and-identity labels (`STREET_ADDRESS`, `PHONE_NUMBER`, `EMAIL_ADDRESS`, `US_SSN`, `US_DRIVER_LICENSE`, `IBAN_CODE`, `IP_ADDRESS`, ...), and hands the "is this about a live matter" question to a local `Qwen3.5-2B-GGUF` acting as a PRIVILEGED/PUBLIC judge with `default_label: PRIVILEGED` and `on_error: match_true`, so a judge failure fails closed. A `metadata` rule lets the document-management system pre-tag a request with `matter_status: under-seal` and short-circuit everything else, and anything over 20,000 characters or carrying DMS tools (discovery dumps) stays on the local 9B regardless. Public statutory research and boilerplate drafting fall through to the cloud.

```json
{
  "model_name": "user.LitigationDesk-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-9B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",          // public legal research -> cloud
    "classifiers": [
      { "id": "pii-mmbert", "type": "classifier", "model": "user.mmbert32k-pii-onnx",
        "labels": ["B-STREET_ADDRESS", "B-US_SSN", "B-PERSON", /* ...32 more BIO labels */],
        "on_error": "match_true" },
      { "id": "privilege-judge", "type": "llm", "model": "Qwen3.5-2B-GGUF",
        "prompt": "You assess whether a request from a lawyer at a litigation firm touches privileged or confidential matter information. A request is PRIVILEGED when it involves attorney work product, litigation strategy, settlement positions ... A request is PUBLIC when it concerns general legal research: statutes, published case law, citation formatting ...",
        "labels": ["PRIVILEGED", "PUBLIC"], "default_label": "PRIVILEGED",
        "on_error": "match_true" }                    // judge failure fails closed
    ],
    "rules": [
      { "id": "matter-flagged-privileged",
        "match": { "metadata": { "key": "matter_status", "any": ["privileged", "under-seal", "protective-order"] } },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "contact-and-identity-onnx",      // deliberately no PERSON / ORGANIZATION
        "match": { "any": [ { "classifier": "pii-mmbert", "label": "B-STREET_ADDRESS",     "min_score": 0.5 },
                            { "classifier": "pii-mmbert", "label": "B-US_SSN",             "min_score": 0.5 },
                            { "classifier": "pii-mmbert", "label": "B-US_DRIVER_LICENSE",  "min_score": 0.5 },
                            /* ...15 more: phone, email, IBAN, credit card, IP, zip */ ] },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "privileged-strategy-llm",
        "match": { "classifier": "privilege-judge", "label": "PRIVILEGED", "min_score": 0.5 },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "bulk-discovery",
        "match": { "any": [ { "min_chars": 20000 }, { "has_tools": true } ] },
        "route_to": "Qwen3.5-9B-GGUF" }
    ]
  }
}
```

**3. Internal engineering assistant:** Developers paste whatever is in their clipboard, and what is in a developer's clipboard is frequently a credential. This is the one policy with three tiers: a regex fast-path for AWS/GitHub/Slack token shapes, PEM private-key headers, JWTs, `key = "..."` assignments, RFC 1918 addresses and `.internal`/`.corp` hostnames, followed by pplx-pii-masking on its `secret`, `private_url`, `account_number`, `private_email` and `private_person` labels (the model was trained on web text, where secrets and internal URLs are exactly the noise it learned to mask), both routing to a local `Qwen3.5-9B-GGUF` that is still competent enough to fix the code it just kept in-house. Agentic sessions (`has_tools`) and whole-file pastes over 12,000 characters go to the cloud, and a local `Qwen3.5-2B-GGUF` acting as an ARCHITECTURE/ROUTINE judge escalates design and multi-service refactor questions there too. Everything else, syntax, error messages, one-liners, is answered by that same 2B as `default_model`: the bulk of a dev assistant's traffic never needs to leave the laptop or wait on a bigger model.

```json
{
  "model_name": "user.DevAssist-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-2B-GGUF", "Qwen3.5-9B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "Qwen3.5-2B-GGUF",             // routine questions -> small local
    "classifiers": [
      { "id": "secrets-pplx", "type": "classifier", "model": "user.pplx-pii-masking-onnx",
        "labels": ["S-secret", "S-private_url", "S-account_number", /* ...34 more BIOES labels */],
        "on_error": "match_true" },
      { "id": "task-shape", "type": "llm", "model": "Qwen3.5-2B-GGUF",
        "prompt": "You assess the shape of a software engineering request. A request is ARCHITECTURE when it asks for system or API design, a multi-file or multi-service refactor, a migration plan ... A request is ROUTINE when it asks about syntax, a single function or small snippet, the meaning of an error message ...",
        "labels": ["ARCHITECTURE", "ROUTINE"], "default_label": "ROUTINE" }
    ],
    "rules": [
      { "id": "credential-regex-fastpath",
        "match": { "any": [ { "regex": "\\bAKIA[0-9A-Z]{16}\\b" },                       // AWS access key
                            { "regex": "\\bgh[pousr]_[A-Za-z0-9]{36,}\\b" },             // GitHub token
                            { "regex": "-----BEGIN [A-Z ]*PRIVATE KEY-----" },           // PEM
                            { "regex": "\\b[a-z0-9.-]+\\.(internal|corp|intranet)\\b" }, // internal hosts
                            /* ...4 more: Slack, JWT, key="...", RFC 1918 */ ] },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "secrets-onnx",
        "match": { "any": [ { "classifier": "secrets-pplx", "label": "S-secret",         "min_score": 0.5 },
                            { "classifier": "secrets-pplx", "label": "S-private_url",    "min_score": 0.5 },
                            { "classifier": "secrets-pplx", "label": "S-account_number", "min_score": 0.5 },
                            /* ...17 more: B/I/E variants, private_email, private_person */ ] },
        "route_to": "Qwen3.5-9B-GGUF" },
      { "id": "agentic-or-whole-file",
        "match": { "any": [ { "has_tools": true }, { "min_chars": 12000 } ] },
        "route_to": "fireworks.kimi-k2p6" },
      { "id": "architecture-llm",
        "match": { "classifier": "task-shape", "label": "ARCHITECTURE", "min_score": 0.5 },
        "route_to": "fireworks.kimi-k2p6" }
    ]
  }
}
```

**4. Auto-insurance claims intake:** A first-notice-of-loss chatbot gets two audiences: prospective customers asking how deductibles work, and policyholders who were rear-ended an hour ago and are typing in whatever language they think in. The first rule is `has_images: true` -> local, because a photo of a crumpled bumper carries a license plate, a house number and often a face, and the image itself should never be uploaded to a third party. Then a VIN regex (17 characters, no I/O/Q) and a claim/policy-number pattern, then OpenMed privacy-filter-ml-v2, chosen here as much for its 16-language coverage as its F1, on a claims-shaped label subset that includes `VIN`, `VRM` (plates), `GPSCOORDINATES`, `IMEI` (telematics dongles), `AGE` and `HEIGHT` (injury descriptions) alongside the usual names, DOB and contact fields. A final `embeddinggemma-300m` *active-claim* rule keeps accident narratives local even when every identifier is missing ("a tree fell on my parked car last night"), while generic policy education falls through to the cloud on a fast `Qwen3.5-9B-NoThinking` / `fireworks.kimi-k2p6` split.

```json
{
  "model_name": "user.ClaimsIntake-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-9B-NoThinking", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",          // coverage explainers -> cloud
    "classifiers": [
      { "id": "pf-v2", "type": "classifier", "model": "user.privacy-filter-ml-v2-onnx",
        "labels": ["S-VIN", "S-VRM", "S-GPSCOORDINATES", "S-IMEI", /* ...213 more BIOES labels */],
        "on_error": "match_true" },
      { "id": "claim-stage", "type": "semantic_similarity",
        "model": "embeddinggemma-300m-qat-q8_0-GGUF-Q8_0",
        "reference_phrases": {
          "active-claim":     ["I was rear-ended on the highway yesterday and my bumper is crushed", /* ...5 more */],
          "policy-education": ["what is the difference between collision and comprehensive coverage", /* ...4 more */] },
        "default_label": "policy-education" }
    ],
    "rules": [
      { "id": "damage-photos-stay-local", "match": { "has_images": true }, "route_to": "Qwen3.5-9B-NoThinking" },
      { "id": "vin-or-claim-number-regex",
        "match": { "any": [ { "regex": "\\b[A-HJ-NPR-Z0-9]{17}\\b" },        // VIN: 17 chars, no I/O/Q
                            { "regex": "\\b(CLM|POL)-?\\d{6,10}\\b" } ] },
        "route_to": "Qwen3.5-9B-NoThinking" },
      { "id": "claimant-pii-onnx",
        "match": { "any": [ { "classifier": "pf-v2", "label": "S-VIN",            "min_score": 0.5 },
                            { "classifier": "pf-v2", "label": "S-VRM",            "min_score": 0.5 },
                            { "classifier": "pf-v2", "label": "S-GPSCOORDINATES", "min_score": 0.5 },
                            { "classifier": "pf-v2", "label": "S-IMEI",           "min_score": 0.5 },
                            /* ...76 more: AGE, HEIGHT, names, DOB, phone, email, address, SSN, bank */ ] },
        "route_to": "Qwen3.5-9B-NoThinking" },
      { "id": "active-claim-semantic",
        "match": { "classifier": "claim-stage", "label": "active-claim", "min_score": 0.6 },
        "route_to": "Qwen3.5-9B-NoThinking" }
    ]
  }
}
```

---

## Timings, and what the gate actually costs

---

##
