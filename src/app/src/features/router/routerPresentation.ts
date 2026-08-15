import type { TranslationParams } from '../../i18n';

type RouterTranslate = (key: string, params?: TranslationParams) => string;

const exactValidation: Record<string, string> = {
  'Router name is required.': 'validation.routerNameRequired',
  'Select at least one candidate model.': 'validation.candidateRequired',
  'Default model must be one of the selected candidates.': 'validation.defaultCandidate',
  'Natural-language router: model is required.': 'validation.naturalModelRequired',
  'Natural-language router: routing instruction is required.': 'validation.naturalInstructionRequired',
  'Add at least one routing rule.': 'validation.ruleRequired',
};

export function localizeRouterValidationMessage(message: string, t: RouterTranslate): string {
  const exact = exactValidation[message];
  if (exact) return t(exact);

  let match = /^Candidate (\d+) is empty\.$/.exec(message);
  if (match) return t('validation.candidateEmpty', { index: Number(match[1]) });
  match = /^Candidate "([\s\S]+)" is duplicated\.$/.exec(message);
  if (match) return t('validation.candidateDuplicate', { name: match[1] });
  match = /^Classifier (\d+): ID is required\.$/.exec(message);
  if (match) return t('validation.classifierIdRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): duplicate ID "([\s\S]+)"\.$/.exec(message);
  if (match) return t('validation.classifierIdDuplicate', { index: Number(match[1]), id: match[2] });
  match = /^Classifier (\d+): model is required\.$/.exec(message);
  if (match) return t('validation.classifierModelRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): add at least one output label\.$/.exec(message);
  if (match) return t('validation.classifierOutputRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): labels must be unique after trimming whitespace\.$/.exec(message);
  if (match) return t('validation.classifierLabelsUnique', { index: Number(match[1]) });
  match = /^Classifier (\d+): default label must be declared in labels\.$/.exec(message);
  if (match) return t('validation.classifierDefaultDeclared', { index: Number(match[1]) });
  match = /^Classifier (\d+): prompt is required for an LLM classifier\.$/.exec(message);
  if (match) return t('validation.classifierPromptRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): add at least one semantic concept\.$/.exec(message);
  if (match) return t('validation.semanticConceptRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): concept names cannot be empty\.$/.exec(message);
  if (match) return t('validation.semanticConceptNameRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+): concept "([\s\S]+)" needs at least one phrase\.$/.exec(message);
  if (match) return t('validation.semanticPhraseRequired', { index: Number(match[1]), concept: match[2] });
  match = /^Classifier (\d+): concept names must be unique after trimming whitespace\.$/.exec(message);
  if (match) return t('validation.semanticConceptUnique', { index: Number(match[1]) });
  match = /^Classifier (\d+): default label must be one of the concept names\.$/.exec(message);
  if (match) return t('validation.semanticDefaultConcept', { index: Number(match[1]) });
  match = /^Rule (\d+): ID is required\.$/.exec(message);
  if (match) return t('validation.ruleIdRequired', { index: Number(match[1]) });
  match = /^Rule (\d+): ID may contain only letters, numbers, dot, underscore, and hyphen\.$/.exec(message);
  if (match) return t('validation.ruleIdFormat', { index: Number(match[1]) });
  match = /^Rule (\d+): duplicate ID "([\s\S]+)"\.$/.exec(message);
  if (match) return t('validation.ruleIdDuplicate', { index: Number(match[1]), id: match[2] });
  match = /^Rule (\d+): route target must be a selected candidate\.$/.exec(message);
  if (match) return t('validation.ruleTargetCandidate', { index: Number(match[1]) });
  match = /^Rule (\d+): outputs JSON must be an object\.$/.exec(message);
  if (match) return t('validation.outputsObject', { index: Number(match[1]) });
  match = /^Rule (\d+): outputs JSON is invalid\.$/.exec(message);
  if (match) return t('validation.outputsInvalid', { index: Number(match[1]) });

  match = /^([\s\S]+): nesting exceeds (\d+) levels\.$/.exec(message);
  if (match) return t('validation.nesting', { path: match[1], count: Number(match[2]) });
  match = /^([\s\S]+): NOT requires exactly one condition\.$/.exec(message);
  if (match) return t('validation.notSingle', { path: match[1] });
  match = /^([\s\S]+): (ALL|ANY) gate requires at least one condition\.$/.exec(message);
  if (match) return t('validation.gateOne', { path: match[1], gate: match[2] });
  match = /^([\s\S]+): (ALL|ANY) gate needs at least 2 conditions - add another or remove the gate\.$/.exec(message);
  if (match) return t('validation.gateTwo', { path: match[1], gate: match[2] });
  match = /^([\s\S]+): duplicate ([\s\S]+) conditions are not allowed in the same gate\.$/.exec(message);
  if (match) return t('validation.duplicateCondition', { path: match[1], type: match[2] });
  match = /^([\s\S]+): add at least one keyword\.$/.exec(message);
  if (match) return t('validation.keywordRequired', { path: match[1] });
  match = /^([\s\S]+): regex cannot be empty\.$/.exec(message);
  if (match) return t('validation.regexRequired', { path: match[1] });
  match = /^([\s\S]+): regex is invalid\.$/.exec(message);
  if (match) return t('validation.regexInvalid', { path: match[1] });
  match = /^([\s\S]+): regex contains a nested unbounded quantifier rejected by the server\.$/.exec(message);
  if (match) return t('validation.regexUnsafe', { path: match[1] });
  match = /^([\s\S]+): UTF-8 byte bound must be a non-negative integer\.$/.exec(message);
  if (match) return t('validation.byteBound', { path: match[1] });
  match = /^([\s\S]+): select a declared classifier\.$/.exec(message);
  if (match) return t('validation.declaredClassifier', { path: match[1] });
  match = /^([\s\S]+): label "([\s\S]+)" is not declared by classifier "([\s\S]+)"\.$/.exec(message);
  if (match) return t('validation.labelDeclared', { path: match[1], label: match[2], classifier: match[3] });
  match = /^([\s\S]+): select a label or configure a default label on classifier "([\s\S]+)"\.$/.exec(message);
  if (match) return t('validation.labelOrDefault', { path: match[1], classifier: match[2] });
  match = /^([\s\S]+): min score must be in \[0, 1\]\.$/.exec(message);
  if (match) return t('validation.minScoreRange', { path: match[1] });
  match = /^([\s\S]+): max score must be in \[0, 1\]\.$/.exec(message);
  if (match) return t('validation.maxScoreRange', { path: match[1] });
  match = /^([\s\S]+): min score cannot exceed max score\.$/.exec(message);
  if (match) return t('validation.scoreOrder', { path: match[1] });
  match = /^([\s\S]+): metadata key is required\.$/.exec(message);
  if (match) return t('validation.metadataKey', { path: match[1] });
  match = /^([\s\S]+): metadata "any" requires at least one value\.$/.exec(message);
  if (match) return t('validation.metadataAnyValue', { path: match[1] });
  return message;
}

const exactImportErrors: Record<string, string> = {
  'Router JSON must be an object.': 'importErrors.rootObject',
  'Router model_name must be a string.': 'importErrors.modelNameString',
  'Router models must be an array when present.': 'importErrors.modelsArray',
  'Embedded models are not editable in GUI3; import is rejected to avoid a lossy save.': 'importErrors.embeddedModels',
  'Router JSON is missing routing.': 'importErrors.missingRouting',
  'Natural-language router must be an object.': 'importErrors.naturalObject',
  'Natural-language routing cannot be combined with rules or classifiers.': 'importErrors.naturalExclusive',
  'Natural-language router type must be "llm".': 'importErrors.naturalType',
  'routing.rules must be an array.': 'importErrors.rulesArray',
  'routing.classifiers must be an array.': 'importErrors.classifiersArray',
  'Rule match must be an object.': 'importErrors.matchObject',
  'Rule match must contain only one logical operator.': 'importErrors.oneLogicalOperator',
  'Rule match cannot mix logical operators with leaf conditions.': 'importErrors.noMixedOperators',
  'Rule match "not" must be an object.': 'importErrors.notObject',
  'Rule match classifier label/score fields require a "classifier" condition.': 'importErrors.classifierFieldsRequireClassifier',
  'Rule match "metadata" must be an object.': 'importErrors.metadataObject',
  'Rule match metadata requires a non-empty string "key".': 'importErrors.metadataKey',
  'Rule match metadata requires exactly one comparator: equals, any, or exists.': 'importErrors.metadataComparator',
  'Rule match metadata "equals" must be a string.': 'importErrors.metadataEqualsString',
  'Rule match metadata "any" must be a non-empty array.': 'importErrors.metadataAnyArray',
  'Rule match metadata "any" items must be non-empty strings.': 'importErrors.metadataAnyItems',
  'Rule match metadata "any" items cannot have leading or trailing whitespace in GUI3.': 'importErrors.metadataAnyWhitespace',
  'Rule match metadata "exists" must be a boolean.': 'importErrors.metadataExistsBoolean',
  'Unsupported or empty rule condition.': 'importErrors.emptyCondition',
};

/**
 * Translate Router import/schema diagnostics at the UI boundary. The parser is
 * deliberately kept language-neutral because its exact strings are also part
 * of runtime/data-contract tests. Unknown server/future-schema diagnostics are
 * returned unchanged rather than being guessed at.
 */
export function localizeRouterImportError(message: string, t: RouterTranslate): string {
  const validation = localizeRouterValidationMessage(message, t);
  if (validation !== message) return validation;

  const exact = exactImportErrors[message];
  if (exact) return t(exact);

  let match = /^Expected recipe "([\s\S]+)"\.$/.exec(message);
  if (match) return t('importErrors.expectedRecipe', { recipe: match[1] });
  match = /^Only router schema version ([\s\S]+) as a string is supported\.$/.exec(message);
  if (match) return t('importErrors.schemaVersion', { version: match[1] });
  match = /^Rule (\d+) must be an object\.$/.exec(message);
  if (match) return t('importErrors.ruleObject', { index: Number(match[1]) });
  match = /^Rule (\d+) is missing match\.$/.exec(message);
  if (match) return t('importErrors.ruleMissingMatch', { index: Number(match[1]) });
  match = /^Rule (\d+) outputs must be an object\.$/.exec(message);
  if (match) return t('importErrors.ruleOutputsObject', { index: Number(match[1]) });
  match = /^Rule match nesting exceeds (\d+) levels\.$/.exec(message);
  if (match) return t('importErrors.matchNesting', { count: Number(match[1]) });
  match = /^Rule match "(all|any)" must be a non-empty array\.$/.exec(message);
  if (match) return t('importErrors.logicalArray', { operator: match[1] });
  match = /^Rule match "(keywords_any|keywords_all)" must be a non-empty array\.$/.exec(message);
  if (match) return t('importErrors.keywordArray', { field: match[1] });
  match = /^Rule match "(keywords_any|keywords_all)" items must be non-empty strings\.$/.exec(message);
  if (match) return t('importErrors.keywordItems', { field: match[1] });
  match = /^Rule match "(keywords_any|keywords_all)" items cannot have leading or trailing whitespace in GUI3\.$/.exec(message);
  if (match) return t('importErrors.keywordWhitespace', { field: match[1] });
  match = /^Rule match "(regex|classifier|label)" must be a non-empty string\.$/.exec(message);
  if (match) return t('importErrors.matchNonEmptyString', { field: match[1] });
  match = /^Rule match "(min_chars|max_chars)" must be a non-negative integer\.$/.exec(message);
  if (match) return t('importErrors.nonNegativeInteger', { field: match[1] });
  match = /^Rule match "(has_tools|has_images)" must be a boolean\.$/.exec(message);
  if (match) return t('importErrors.booleanField', { field: match[1] });
  match = /^Rule match "(min_score|max_score)" must be a number\.$/.exec(message);
  if (match) return t('importErrors.numberField', { field: match[1] });
  match = /^Unsupported metadata field(s?): ([\s\S]+)\.$/.exec(message);
  if (match) return t('importErrors.unsupportedMetadata', { fields: match[2] });
  match = /^Unsupported rule condition field(s?): ([\s\S]+)\.$/.exec(message);
  if (match) return t('importErrors.unsupportedCondition', { fields: match[2] });
  match = /^([\s\S]+) contains unsupported field(s?): ([\s\S]+)\.$/.exec(message);
  if (match) return t('importErrors.unsupportedFields', { context: match[1], fields: match[3] });
  match = /^([\s\S]+) must be a non-empty string\.$/.exec(message);
  if (match) return t('importErrors.nonEmptyString', { context: match[1] });
  match = /^([\s\S]+) must be (an array|a non-empty array)\.$/.exec(message);
  if (match) return t(match[2] === 'an array' ? 'importErrors.array' : 'importErrors.nonEmptyArray', { context: match[1] });
  match = /^([\s\S]+) items must be non-empty strings\.$/.exec(message);
  if (match) return t('importErrors.nonEmptyItems', { context: match[1] });
  match = /^Unsupported classifier type "([\s\S]+)"\. GUI3 can safely edit classifier, semantic_similarity, and llm classifiers only\.$/.exec(message);
  if (match) return t('importErrors.classifierType', { type: match[1] });
  match = /^Classifier (\d+) on_error must be "match_true" or "match_false"\.$/.exec(message);
  if (match) return t('importErrors.classifierOnError', { index: Number(match[1]) });
  match = /^Classifier (\d+): semantic_similarity cannot contain labels\.$/.exec(message);
  if (match) return t('importErrors.semanticLabels', { index: Number(match[1]) });
  match = /^Classifier (\d+): semantic_similarity prompt is not editable in GUI3\.$/.exec(message);
  if (match) return t('importErrors.semanticPrompt', { index: Number(match[1]) });
  match = /^Classifier (\d+): semantic_similarity requires non-empty reference_phrases\.$/.exec(message);
  if (match) return t('importErrors.semanticReferences', { index: Number(match[1]) });
  match = /^Classifier (\d+): semantic concept names cannot be empty\.$/.exec(message);
  if (match) return t('importErrors.semanticConceptEmpty', { index: Number(match[1]) });
  match = /^Classifier (\d+): semantic concept names cannot have leading or trailing whitespace in GUI3\.$/.exec(message);
  if (match) return t('importErrors.semanticConceptWhitespace', { index: Number(match[1]) });
  match = /^Classifier (\d+) concept "([\s\S]+)" phrases cannot have leading or trailing whitespace in GUI3\.$/.exec(message);
  if (match) return t('importErrors.semanticPhraseWhitespace', { index: Number(match[1]), concept: match[2] });
  match = /^Classifier (\d+): ([\s\S]+) reference_phrases are not editable in GUI3\.$/.exec(message);
  if (match) return t('importErrors.referencePhrasesUnsupported', { index: Number(match[1]), type: match[2] });
  match = /^Classifier (\d+): prompt on a classifier would be lost by GUI3; import is rejected to avoid a lossy save\.$/.exec(message);
  if (match) return t('importErrors.classifierPromptLossy', { index: Number(match[1]) });
  match = /^Classifier (\d+) labels cannot have leading or trailing whitespace in GUI3\.$/.exec(message);
  if (match) return t('importErrors.classifierLabelsWhitespace', { index: Number(match[1]) });
  match = /^Classifier (\d+): LLM classifiers require at least one label\.$/.exec(message);
  if (match) return t('importErrors.llmLabelRequired', { index: Number(match[1]) });
  match = /^Classifier (\d+) default_label must be a non-empty string\.$/.exec(message);
  if (match) return t('importErrors.defaultLabelString', { index: Number(match[1]) });
  match = /^Classifier (\d+) default_label cannot have leading or trailing whitespace in GUI3\.$/.exec(message);
  if (match) return t('importErrors.defaultLabelWhitespace', { index: Number(match[1]) });
  match = /^Router components do not match editable routing dependencies([\s\S]*)\.$/.exec(message);
  if (match) return t('importErrors.componentsMismatch', { details: match[1] });
  return message;
}

export function localizeRouterGraphError(message: string, t: RouterTranslate): string {
  if (message === 'This condition is already present in this gate.') return t('graph.errors.duplicate');
  if (message === 'NOT can contain only one condition.') return t('graph.errors.notSingle');
  const match = /^A ([\s\S]+) condition is already in this gate\.$/.exec(message);
  return match ? t('graph.errors.duplicateType', { type: match[1] }) : message;
}

export function localizeProviderEndpointError(message: string, t: RouterTranslate): string {
  const mapping: Record<string, string> = {
    'Endpoint is required.': 'endpointErrors.required',
    'Endpoint must be a valid absolute URL.': 'endpointErrors.absoluteUrl',
    'Endpoint must use http:// or https://.': 'endpointErrors.protocol',
    'Endpoint must include a host.': 'endpointErrors.host',
    'Endpoint must not include embedded credentials.': 'endpointErrors.credentials',
    'Endpoint must not include a URL fragment.': 'endpointErrors.fragment',
    'Plain HTTP endpoints require explicit insecure HTTP opt-in.': 'endpointErrors.insecureOptIn',
  };
  const key = mapping[message];
  return key ? t(key) : message;
}
