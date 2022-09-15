import { ba_se } from 'mod.ule';

function asmFunc(importObject) {
 var Math_imul = Math.imul;
 var Math_fround = Math.fround;
 var Math_abs = Math.abs;
 var Math_clz32 = Math.clz32;
 var Math_min = Math.min;
 var Math_max = Math.max;
 var Math_floor = Math.floor;
 var Math_ceil = Math.ceil;
 var Math_trunc = Math.trunc;
 var Math_sqrt = Math.sqrt;
 var nan = NaN;
 var infinity = Infinity;
 var mod_ule = importObject["mod.ule"];
 var base = mod_ule["ba.se"];
 function $0() {
  base();
 }
 
 return {
  "exported": $0
 };
}

var retasmFunc = asmFunc({
  'mod.ule': {
    ba_se,
  }
});
export var exported = retasmFunc.exported;
