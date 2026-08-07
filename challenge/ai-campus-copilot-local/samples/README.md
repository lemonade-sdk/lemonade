# Sample documents

Three fictional college documents for trying out AI Campus Copilot Local.

| File | Represents | Good for testing |
|------|------------|------------------|
| `semester-examination-notice.md` | Examination schedule circular | Deadlines, fees, eligibility rules, grounded Q&A |
| `campus-placement-announcement.md` | Campus recruitment announcement | Internship and placement extraction |
| `scholarship-and-events-circular.md` | Scholarship and events circular | Mixed deadline categories, hierarchical summarisation |

## These are fictional

Northfield Institute of Technology does not exist. Every organisation, person, date,
amount, email address and URL in these files is invented. They contain no personal
information and no copyrighted institutional material.

The `*-example.edu` and `*-example.com` domains are placeholders and are not real sites.

## Using them

The app accepts `.md` directly — upload any of these on the **Documents** page.

To test the PDF path, print one to PDF from your browser or editor:

```bash
# Any Markdown-to-PDF tool works. For example, with pandoc:
pandoc samples/semester-examination-notice.md -o notice.pdf
```

Then upload `notice.pdf`. The extractor is page-aware, so citations will refer to PDF
page numbers rather than Markdown sections.

## Questions worth trying

Against `semester-examination-notice.md`:

- When do the end-semester examinations begin?
- What happens if my attendance is 70%?
- How much is the late examination fee, and until when can I pay it?
- இந்த அறிவிப்பில் உள்ள முக்கியமான தேதிகள் என்ன?
- What is the hostel wifi password? *(should answer "not found in this document")*

Against `campus-placement-announcement.md`:

- Which companies are hiring interns, and what stipend do they offer?
- I have a CGPA of 6.8 with one backlog — which drives am I eligible for?
- What documents do I need to bring to the drive?
