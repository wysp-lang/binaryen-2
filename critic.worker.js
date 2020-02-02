importScripts('binaryen.js');

var ready = false;
var data;

function processData() {
  if (!ready || !data) return;
  var results = {
    oldSize: data.wasm.length
  };
  var module = new binaryen.readBinary(data.wasm);
  module.optimize();
  results.optSize = module.emitBinary().length;
  // Approximate --converge with a double optimize, as it tends to exponentially
  // decrease in benefit anyhow.
  module.optimize();
  results.convergeSize = module.emitBinary().length;
  module.dispose();
  postMessage(results);
}
  
binaryen.ready.then(() => {
  ready = true;
  processData();
});

onmessage = (message) => {
  data = message.data;
  processData();
};

