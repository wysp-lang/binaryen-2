(module
 (import "env" "importedf" (func $imported-func))
 (global $global (mut i32) (i32.const 1))
 (start $startey)
 (export "foo" (func $foobar))
 (export "ex_of_im" (func $wrapped_imported_func))
 (func $foobar (result i32)
  (i32.add
   (i32.const 41)
   (i32.const 1)
  )
 )
 (func $startey
  ;; TODO: otpimize globals
  (global.set $global
   (i32.const 2)
  )
  (call $imported-func)
 )
 (func $wrapped_imported_func
  (call $imported-func)
 )
)
