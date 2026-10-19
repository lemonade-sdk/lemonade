import {
  createFlowDiagram,
  styles_default
} from "./chunk-MX3EQCGM.mjs";
import "./chunk-RTBOCTTP.mjs";
import "./chunk-HPLX5OYV.mjs";
import "./chunk-BYAF6FXG.mjs";
import "./chunk-PWCFYZI5.mjs";
import "./chunk-STSWKLMO.mjs";
import "./chunk-ZICDAICJ.mjs";
import "./chunk-MPM6TOF7.mjs";
import "./chunk-DKH7L6O3.mjs";
import "./chunk-PLCLPJVV.mjs";
import "./chunk-XXV7AQMY.mjs";
import "./chunk-RTI7CJYH.mjs";
import "./chunk-YV36FAQU.mjs";
import "./chunk-2V3FENDJ.mjs";
import "./chunk-AQ2D6KKF.mjs";
import "./chunk-OX6ZKUVE.mjs";
import "./chunk-IPM4HZQ6.mjs";
import "./chunk-NEZ6ONQZ.mjs";
import "./chunk-FO5PYUIK.mjs";
import {
  __name
} from "./chunk-PTVI3W5X.mjs";

// src/diagrams/swimlanes/styles.ts
var getStyles = /* @__PURE__ */ __name((options) => `${styles_default(options)}
  .swimlane.cluster rect {
    stroke: ${options.clusterBorder} !important;
  }
  [data-look="neo"].cluster rect {
    filter: none;
  }
`, "getStyles");
var styles_default2 = getStyles;

// src/diagrams/swimlanes/swimlanesDiagram.ts
var diagram = createFlowDiagram({ defaultLayout: "swimlane", styles: styles_default2 });
export {
  diagram
};
