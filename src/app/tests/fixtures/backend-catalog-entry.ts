/* One entry point so the bundled modules share a single store instance:
 * modelPresentation resolves labels through the same catalog the test seeds. */
export { parseBackendCatalog } from '../../src/features/backends/backendCatalog';
export {
  attachBackendCatalog,
  getBackendCatalogSnapshot,
  refreshBackendCatalog,
  resetBackendCatalogForTests,
} from '../../src/features/backends/backendCatalogStore';
export {
  OTHER_RUNTIMES,
  orderSections,
  sectionForModality,
  sectionMeta,
} from '../../src/features/backends/backendSections';
export { backendCompactLabel, backendLabel } from '../../src/modelPresentation';
