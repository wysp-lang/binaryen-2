(module
 (export "wasm_func_a" (func $a_wasm_func))

 (func $a_wasm_func (param $x i32)
  (block $nothing
   ;; do nothing interesting
   (nop)
   (local.set $x (i32.const 10))
   (nop)
   (return)
  )
 )
)

