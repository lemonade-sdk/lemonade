# Three-minute demo script

Target length: **3:00**. Timings are cumulative.

## Before recording

- [ ] Lemonade Server running; `curl http://127.0.0.1:13305/api/v1/health` returns ok
- [ ] One chat model and one embedding model downloaded
- [ ] `npm run dev` running; <http://localhost:3000> open
- [ ] **Local data cleared** (Privacy → Delete all local data) so the demo starts empty
- [ ] `samples/semester-examination-notice.md` and
      `samples/campus-placement-announcement.md` ready to drag
- [ ] Browser zoom ~110%, dark mode on, notifications silenced

---

## 0:00 – 0:20 — The repository

**Show:** GitHub repo `JEROME-PRAKASH-L/lemonade`, then the `challenge/` folder.

> "This is my fork of the official Lemonade repository. Lemonade Server is a community
> project maintained by the Lemonade community and AMD contributors — I haven't touched
> it. My work is one self-contained folder: `challenge/ai-campus-copilot-local`. It's
> Apache-2.0, same as upstream."

Scroll the challenge folder so `README.md`, `LICENSE`, `src/`, `tests/` and `docs/` are
visible.

---

## 0:20 – 0:40 — Lemonade running locally

**Show:** terminal with the health response, then the app's Setup page.

> "Lemonade Server is running on this machine on port 13305. Here's its health endpoint."

Switch to **Setup**.

> "The app checks the connection itself — server version, what's loaded, and my OS and
> processor straight from Lemonade's system-info endpoint. No model name is hardcoded:
> this list is my actual installed models, filtered by capability. Text-generation
> models on the left, embedding models on the right."

Point at the **Ready to use** badge.

---

## 0:40 – 1:05 — Upload and process

**Show:** Documents page. Drag `semester-examination-notice.md` in.

> "A fictional examination circular — exam dates, fee deadlines, an attendance rule.
> The file is parsed in the browser; it never leaves this machine."

Let the progress bar run.

> "Page-aware extraction, then chunking at 500 words with 80-word overlap, then
> embeddings from Lemonade in batches. Those are real timings, measured — extraction,
> chunking and embedding, each reported separately."

---

## 1:05 – 1:40 — Grounded answer with citations

**Show:** Ask page. Type: *"What happens if my attendance is 70 percent?"*

> "The question gets embedded, matched against stored chunks by cosine similarity, and
> only the relevant passages go to the model."

Let it stream.

> "The answer cites a page. And here—" *(expand the sources panel)* "—are the exact
> chunks it used, with page, chunk number and similarity score. Everything is checkable."

Now ask: *"What is the hostel wifi password?"*

> "This isn't in the document. It says so, instead of inventing one. That's the whole
> point — the system prompt treats document text as untrusted data and refuses to answer
> beyond it."

*(Optional, if time allows: ask a question in Tamil.)*

---

## 1:40 – 2:05 — Deadlines and placements

**Show:** Deadlines page. Press **Extract deadlines**.

> "Same local model, but now asked for structured JSON, validated with Zod. Exam dates,
> fee deadlines, scholarship windows — each with the sentence it came from."

Switch to the **Timeline** view.

> "And note this: dates the document states ambiguously are left un-normalised rather
> than guessed. A hallucinated deadline is worse than no deadline."

Switch document to the placement announcement, open **Careers**, press extract.

> "Organisation, role, eligibility, skills, stipend, deadline, application link. Fields
> the document doesn't state stay blank."

---

## 2:05 – 2:30 — Real performance

**Show:** Performance page. Press **Run benchmark**.

> "Five fixed questions through the full pipeline. Question embedding, retrieval, time
> to first token, total generation — all measured on this hardware. Tokens per second
> comes from Lemonade's own stats endpoint; where Lemonade doesn't report a value, the
> field stays blank. I'm not making numbers up."

Show the export buttons.

> "Exportable as JSON or CSV."

---

## 2:30 – 2:50 — Privacy

**Show:** Privacy page.

> "Here's exactly where data goes: the file stays in browser memory, extracted text goes
> to localhost, embeddings and chat history live in IndexedDB on this machine. No cloud
> AI provider — there's no OpenAI or Anthropic code path in this project at all."

Point at the network-activity indicator in the top bar.

> "This indicator is live on every page, so you can always see when the app is talking
> to the model. And one button deletes everything."

---

## 2:50 – 3:00 — Licence and attribution

**Show:** the README attribution section.

> "Apache-2.0, matching upstream. Lemonade Server is the Lemonade community's and AMD
> contributors' work; my contribution is the application in this folder. Built by Jerome
> Prakash L for the AMD Lemonade Developer Challenge. Thanks for watching."

---

## If you overrun

Cut in this order:

1. The Tamil question
2. The Careers extraction (mention it instead)
3. The earlier-runs list on Performance

## Do not claim on camera

- Any throughput number not visible on screen at that moment
- That scanned PDFs work — they do not
- That the app was tested across multiple hardware configurations
