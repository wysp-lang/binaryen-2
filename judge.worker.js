importScripts('binaryen.js');

var ready = false;
var data;

function processData() {
  if (!ready || !data) return;
  var module = new binaryen.readBinary(data.wasm);
  module.optimize();
  var binary = module.emitBinary();
  module.dispose();
  postMessage({
    size: binary.length
  });
}
  
binaryen.ready.then(() => {
  ready = true;
  processData();
});

onmessage = (message) => {
  data = message.data;
  processData();
};

