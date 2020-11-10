;; speculative inlining with a return_call in the contents
(module
 (func $0 (param $x i32) (result i32)
  (return_call $0 (i32.const 3)
 )
 (func $1
  (call $0
 )
)
