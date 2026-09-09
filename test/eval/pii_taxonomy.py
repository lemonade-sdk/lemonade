"""
Canonical PII taxonomy and per-model label mappings.

The PII benchmarks in this directory score a binary: "did the model emit any
entity at all". That is the right question for routing (local vs cloud) but it
makes every model's recall look alike, because a model scores a hit by firing
ANY label anywhere - even one unrelated to the PII the document actually
contains. This module is the vocabulary layer that lets the same runs be
re-scored per category.

Design
------
Every taxonomy maps into ONE canonical interlingua rather than pairwise onto
each other: N mappings instead of N**2, and adding a model later is one table.

Three rules matter more than the mapping itself:

1. COVERAGE. Each model declares which canonical categories it can express at
   all (`coverage()`, derived from its mapping). Per-category recall is only
   computed over that set; gold categories outside it are reported as a
   declared coverage gap, NOT as a miss. Without this, a model is punished for
   lacking a class it never claimed - which is what the old "missed categories"
   column was really measuring.

2. COARSE-LABEL CREDIT. A model label may be coarser than the canonical class,
   so mappings are one-to-MANY: pplx's `account_number` expands to
   {FINANCIAL_ACCOUNT, GOV_ID, INTERNAL_ID, MEDICAL}. A prediction counts as
   detecting any gold category in its expansion, because flagging an SSN as
   "account number" genuinely is a catch for routing purposes. This is the
   generous reading; it is deliberate and is why `--strict` exists in the
   consumer script.

3. CATCH-ALLS DON'T ATTRIBUTE. pplx's `other_pii` and OpenMed's `MASKEDNUMBER`
   would otherwise match every category and make those models unbeatable. They
   map to CATCH_ALL, which counts toward the document-level binary but is
   excluded from per-category credit and tallied separately.

Nemotron-PII's own 55 labels are themselves mapped in, so gold and predictions
are compared in the same space and neither side is privileged.
"""

# ---------------------------------------------------------------------------
# Canonical categories
# ---------------------------------------------------------------------------

# Sensitive attributes are split four ways rather than lumped into one
# "demographic" bucket, because that lump would hide the single most
# discriminating fact in this comparison: mmBERT's NRP covers nationality /
# religion / politics but not gender, while OpenMed covers gender/sex but not
# religion or politics. One bucket would score both as "has demographics".
CANONICAL = [
    "PERSON_NAME",
    "CONTACT_EMAIL",
    "CONTACT_PHONE",
    "ADDRESS_LOCATION",
    "DATE_TIME",
    "DATE_OF_BIRTH",
    "AGE",
    "GOV_ID",
    "FINANCIAL_ACCOUNT",
    "CREDENTIAL_SECRET",
    "ORG_COMPANY",
    "OCCUPATION_EMPLOYMENT",
    "WEB_URL",
    "NETWORK_ID",
    "ACCOUNT_HANDLE",
    "MEDICAL",
    "BIOMETRIC",
    "PHYSICAL_ATTRIBUTE",
    "GENDER_SEXUALITY",
    "RACE_ETHNICITY_LANGUAGE",
    "BELIEF_POLITICAL",
    "EDUCATION",
    "VEHICLE",
    "INTERNAL_ID",
]

# Not a category: a label so generic it would match anything. Counts for the
# document-level binary, never for per-category attribution.
CATCH_ALL = "CATCH_ALL"

# Not a category: a label that carries no personal information at all (currency
# names, ordinal directions). Dropped entirely rather than mapped, so it can
# neither earn credit nor count as a false positive.
NOT_PII = "NOT_PII"


# ---------------------------------------------------------------------------
# Nemotron-PII (ground truth) - 55 labels
# ---------------------------------------------------------------------------

NEMOTRON = {
    "first_name": ["PERSON_NAME"],
    "last_name": ["PERSON_NAME"],
    "email": ["CONTACT_EMAIL"],
    "phone_number": ["CONTACT_PHONE"],
    "fax_number": ["CONTACT_PHONE"],
    "date": ["DATE_TIME"],
    "time": ["DATE_TIME"],
    "date_time": ["DATE_TIME"],
    "date_of_birth": ["DATE_OF_BIRTH"],
    "age": ["AGE"],
    "url": ["WEB_URL"],
    "ipv4": ["NETWORK_ID"],
    "ipv6": ["NETWORK_ID"],
    "mac_address": ["NETWORK_ID"],
    "http_cookie": ["NETWORK_ID"],
    "device_identifier": ["NETWORK_ID"],
    "user_name": ["ACCOUNT_HANDLE"],
    "company_name": ["ORG_COMPANY"],
    "occupation": ["OCCUPATION_EMPLOYMENT"],
    "employment_status": ["OCCUPATION_EMPLOYMENT"],
    "education_level": ["EDUCATION"],
    "gender": ["GENDER_SEXUALITY"],
    "sexuality": ["GENDER_SEXUALITY"],
    "race_ethnicity": ["RACE_ETHNICITY_LANGUAGE"],
    "language": ["RACE_ETHNICITY_LANGUAGE"],
    "religious_belief": ["BELIEF_POLITICAL"],
    "political_view": ["BELIEF_POLITICAL"],
    "street_address": ["ADDRESS_LOCATION"],
    "city": ["ADDRESS_LOCATION"],
    "state": ["ADDRESS_LOCATION"],
    "county": ["ADDRESS_LOCATION"],
    "country": ["ADDRESS_LOCATION"],
    "postcode": ["ADDRESS_LOCATION"],
    "coordinate": ["ADDRESS_LOCATION"],
    "ssn": ["GOV_ID"],
    "national_id": ["GOV_ID"],
    "tax_id": ["GOV_ID"],
    "certificate_license_number": ["GOV_ID"],
    "account_number": ["FINANCIAL_ACCOUNT"],
    "bank_routing_number": ["FINANCIAL_ACCOUNT"],
    "swift_bic": ["FINANCIAL_ACCOUNT"],
    "credit_debit_card": ["FINANCIAL_ACCOUNT"],
    "cvv": ["CREDENTIAL_SECRET"],
    "pin": ["CREDENTIAL_SECRET"],
    "password": ["CREDENTIAL_SECRET"],
    "api_key": ["CREDENTIAL_SECRET"],
    "medical_record_number": ["MEDICAL"],
    "health_plan_beneficiary_number": ["MEDICAL"],
    "blood_type": ["MEDICAL"],
    "biometric_identifier": ["BIOMETRIC"],
    "license_plate": ["VEHICLE"],
    "vehicle_identifier": ["VEHICLE"],
    "customer_id": ["INTERNAL_ID"],
    "employee_id": ["INTERNAL_ID"],
    "unique_id": ["INTERNAL_ID"],
}


# ---------------------------------------------------------------------------
# llm-semantic-router/mmbert32k-pii-detector-merged - 17 types (Presidio-style)
# ---------------------------------------------------------------------------

# NRP is Presidio's "nationality, religious or political group" - the reason
# this model is NOT simply blind to the religion/politics cases the way pplx
# is. Misses there are genuine misses, not a taxonomy gap.
MMBERT = {
    "PERSON": ["PERSON_NAME"],
    "TITLE": ["PERSON_NAME"],
    "EMAIL_ADDRESS": ["CONTACT_EMAIL"],
    "PHONE_NUMBER": ["CONTACT_PHONE"],
    "STREET_ADDRESS": ["ADDRESS_LOCATION"],
    "GPE": ["ADDRESS_LOCATION"],
    "ZIP_CODE": ["ADDRESS_LOCATION"],
    "DATE_TIME": ["DATE_TIME", "DATE_OF_BIRTH"],
    "AGE": ["AGE"],
    "US_SSN": ["GOV_ID"],
    "US_DRIVER_LICENSE": ["GOV_ID"],
    "CREDIT_CARD": ["FINANCIAL_ACCOUNT"],
    "IBAN_CODE": ["FINANCIAL_ACCOUNT"],
    "ORGANIZATION": ["ORG_COMPANY"],
    "DOMAIN_NAME": ["WEB_URL"],
    "IP_ADDRESS": ["NETWORK_ID"],
    "NRP": ["RACE_ETHNICITY_LANGUAGE", "BELIEF_POLITICAL"],
}


# ---------------------------------------------------------------------------
# OpenMed/privacy-filter-multilingual (+ v2) - 54 types (ai4privacy-style)
# ---------------------------------------------------------------------------

OPENMED = {
    "FIRSTNAME": ["PERSON_NAME"],
    "MIDDLENAME": ["PERSON_NAME"],
    "LASTNAME": ["PERSON_NAME"],
    "PREFIX": ["PERSON_NAME"],
    "EMAIL": ["CONTACT_EMAIL"],
    "PHONE": ["CONTACT_PHONE"],
    "BUILDINGNUMBER": ["ADDRESS_LOCATION"],
    "STREET": ["ADDRESS_LOCATION"],
    "SECONDARYADDRESS": ["ADDRESS_LOCATION"],
    "CITY": ["ADDRESS_LOCATION"],
    "COUNTY": ["ADDRESS_LOCATION"],
    "STATE": ["ADDRESS_LOCATION"],
    "ZIPCODE": ["ADDRESS_LOCATION"],
    "GPSCOORDINATES": ["ADDRESS_LOCATION"],
    "DATE": ["DATE_TIME"],
    "TIME": ["DATE_TIME"],
    "DATEOFBIRTH": ["DATE_OF_BIRTH"],
    "AGE": ["AGE"],
    "SSN": ["GOV_ID"],
    "ACCOUNTNAME": ["FINANCIAL_ACCOUNT"],
    "BANKACCOUNT": ["FINANCIAL_ACCOUNT"],
    "BIC": ["FINANCIAL_ACCOUNT"],
    "IBAN": ["FINANCIAL_ACCOUNT"],
    "CREDITCARD": ["FINANCIAL_ACCOUNT"],
    "CREDITCARDISSUER": ["FINANCIAL_ACCOUNT"],
    "BITCOINADDRESS": ["FINANCIAL_ACCOUNT"],
    "ETHEREUMADDRESS": ["FINANCIAL_ACCOUNT"],
    "LITECOINADDRESS": ["FINANCIAL_ACCOUNT"],
    "PASSWORD": ["CREDENTIAL_SECRET"],
    "PIN": ["CREDENTIAL_SECRET"],
    "CVV": ["CREDENTIAL_SECRET"],
    "ORGANIZATION": ["ORG_COMPANY"],
    "JOBTITLE": ["OCCUPATION_EMPLOYMENT"],
    "JOBDEPARTMENT": ["OCCUPATION_EMPLOYMENT"],
    "OCCUPATION": ["OCCUPATION_EMPLOYMENT"],
    "URL": ["WEB_URL"],
    "IPADDRESS": ["NETWORK_ID"],
    "MACADDRESS": ["NETWORK_ID"],
    "USERNAME": ["ACCOUNT_HANDLE"],
    "USERAGENT": ["NETWORK_ID"],
    "IMEI": ["NETWORK_ID"],
    "EYECOLOR": ["PHYSICAL_ATTRIBUTE"],
    "HEIGHT": ["PHYSICAL_ATTRIBUTE"],
    "GENDER": ["GENDER_SEXUALITY"],
    "SEX": ["GENDER_SEXUALITY"],
    "VIN": ["VEHICLE"],
    "VRM": ["VEHICLE"],
    "MASKEDNUMBER": [CATCH_ALL],
    "AMOUNT": [NOT_PII],
    "CURRENCY": [NOT_PII],
    "CURRENCYCODE": [NOT_PII],
    "CURRENCYNAME": [NOT_PII],
    "CURRENCYSYMBOL": [NOT_PII],
    "ORDINALDIRECTION": [NOT_PII],
}


# ---------------------------------------------------------------------------
# perplexity-ai/pplx-pii-masking - 9 categories
# ---------------------------------------------------------------------------

PPLX = {
    "private_person": ["PERSON_NAME"],
    "private_email": ["CONTACT_EMAIL"],
    "private_phone": ["CONTACT_PHONE"],
    "private_address": ["ADDRESS_LOCATION"],
    "private_date": ["DATE_TIME", "DATE_OF_BIRTH"],
    "private_url": ["WEB_URL"],
    "account_number": ["FINANCIAL_ACCOUNT", "GOV_ID", "INTERNAL_ID", "MEDICAL"],
    "secret": ["CREDENTIAL_SECRET"],
    "other_pii": [CATCH_ALL],
}


# ---------------------------------------------------------------------------
# nvidia/gliner-PII
# ---------------------------------------------------------------------------

# GLiNER is zero-shot and pii_gliner_eval.py hands it the corpus's own gold
# label names at inference time, so its output vocabulary IS Nemotron's. That
# also means its numbers are not directly comparable to the fixed-vocabulary
# models: it was told exactly what to look for, in the dataset's own words.
GLINER = dict(NEMOTRON)


# openai/privacy-filter emits the same 9-category schema as pplx-pii-masking
# (verified against its run log: private_person, private_email, private_phone,
# private_address, private_date, private_url, account_number, secret), so it
# shares the mapping rather than duplicating it.
OPENAI_PRIVACY_FILTER = dict(PPLX)


# LLM-as-router emits a routing decision and a free-text rationale, never
# entity labels, so it has no output taxonomy to map. It is a general model
# asked whether a document contains PII, so nothing is out of scope: coverage
# is every canonical category. Only used by --doc-level scoring, which reads
# the pass/fail verdict rather than labels.
LLM_ROUTER = {cat.lower(): [cat] for cat in CANONICAL}


MODELS = {
    "nemotron": NEMOTRON,
    "llm": LLM_ROUTER,
    "mmbert": MMBERT,
    "openmed": OPENMED,
    "pplx": PPLX,
    "openai_pf": OPENAI_PRIVACY_FILTER,
    "gliner": GLINER,
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def normalize(labels, mapping) -> tuple[set[str], bool, set[str]]:
    """Map raw labels into canonical space.

    Returns (canonical categories, saw_catch_all, unmapped raw labels).
    NOT_PII labels are dropped. Unmapped labels are returned rather than
    silently ignored so a taxonomy drift (a model emitting a label this table
    has never seen) surfaces as a warning instead of a quiet zero.
    """
    canon: set[str] = set()
    catch_all = False
    unmapped: set[str] = set()
    for raw in labels:
        key = raw.strip()
        if not key:
            continue
        targets = mapping.get(key)
        if targets is None:
            unmapped.add(key)
            continue
        for target in targets:
            if target == CATCH_ALL:
                catch_all = True
            elif target != NOT_PII:
                canon.add(target)
    return canon, catch_all, unmapped


def coverage(mapping) -> set[str]:
    """Canonical categories this taxonomy can express at all.

    A model is only scored on these; gold categories outside are a declared
    coverage gap, not a miss.
    """
    covered: set[str] = set()
    for targets in mapping.values():
        for target in targets:
            if target not in (CATCH_ALL, NOT_PII):
                covered.add(target)
    return covered
