import {
  ClassDB,
  classDiagram_default,
  classRenderer_v3_unified_default,
  styles_default
} from "./chunk-WBLI2NX4.mjs";
import "./chunk-RTBOCTTP.mjs";
import "./chunk-HPLX5OYV.mjs";
import "./chunk-BYAF6FXG.mjs";
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

// src/diagrams/class/classDiagram-v2.ts
var diagram = {
  parser: classDiagram_default,
  get db() {
    return new ClassDB();
  },
  renderer: classRenderer_v3_unified_default,
  styles: styles_default,
  init: /* @__PURE__ */ __name((cnf) => {
    if (!cnf.class) {
      cnf.class = {};
    }
    cnf.class.arrowMarkerAbsolute = cnf.arrowMarkerAbsolute;
  }, "init")
};
export {
  diagram
};
